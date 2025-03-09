// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 * Copyright (C) 2025 Philipp Jungkamp <philipp.jungkamp@rwth-aachen.de>
 * Copyright (C) 2025 Jonas Dohmen <jonas.dohmen@rwth-aachen.de>
 * Copyright (C) 2025 Antonios Salios <antonios.salios@rwth-aachen.de>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>

#include "ouichefs.h"
#include "bitmap.h"

static const struct inode_operations ouichefs_inode_ops;

static int get_inode(struct super_block *sb, uint32_t istore)
{
	int ret;
	struct buffer_head *bh;
	struct ouichefs_inode *disk_inode;

	bh = ouichefs_bread_inode(&disk_inode, sb, istore);
	if (!bh)
		return -EIO;

	ret = ouichefs_get_inode(sb, istore, disk_inode);
	mark_buffer_dirty(bh);
	brelse(bh);
	return ret;
}

static int put_inode(struct super_block *sb, uint32_t istore)
{
	int ret;
	struct buffer_head *bh;
	struct ouichefs_inode *disk_inode;

	bh = ouichefs_bread_inode(&disk_inode, sb, istore);
	if (!bh)
		return -EIO;

	ret = ouichefs_put_inode(sb, istore, disk_inode);
	mark_buffer_dirty(bh);
	brelse(bh);
	return ret;
}

static int cow_update_dir_block(struct ouichefs_dir_block *dir,
				struct inode *inode)
{
	int err, i;
	struct ouichefs_file *file;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	for (i = 0; i < OUICHEFS_MAX_DIR_FILES; i++) {
		file = &dir->files[i];

		if (!file->istore || file->istore == ci->isrc)
			break;
	}

	if (i == OUICHEFS_MAX_DIR_FILES || file->istore != ci->isrc)
		return err = -ESRCH;

	file->istore = ci->idest;

	return 0;
}

struct buffer_head *cow_copy_index(struct inode *inode)
{
	int err, i;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh_old = NULL, *bh_new = NULL;
	struct ouichefs_file_block *fb;
	uint32_t block;
	uint32_t new = 0;

	new = get_free_block(sbi);
	if (!new) {
		err = -ENOSPC;
		goto bcopy_err;
	}

	bh_old = sb_bread(sb, ci->index_block);
	bh_new = sb_bread(sb, new);
	if (!bh_old || !bh_new) {
		err = -EIO;
		goto bcopy_block;
	}

	pr_debug("(index %u -> %u)", ci->index_block, new);
	memcpy(bh_new->b_data, bh_old->b_data,
	       sizeof(struct ouichefs_dir_block));

	if (!S_ISREG(inode->i_mode))
		goto skip;

	fb = (struct ouichefs_file_block *)bh_new->b_data;
	for (i = 0; i < OUICHEFS_MAX_FILE_BLOCKS; i++) {
		if (!fb->blocks[i])
			continue;

		block = get_free_block(sbi);
		if (!block) {
			err = -ENOSPC;
			goto bcopy_file_block;
		}

		/* This is wrong on so many buffer layer abstractions */
		brelse(bh_old);
		bh_old = sb_bread(sb, fb->blocks[i]);
		if (!bh_old) {
			put_block(sbi, block);
			goto bcopy_file_block;
		}

		bh_old->b_blocknr = block;
		mark_buffer_dirty(bh_old);
		sync_dirty_buffer(bh_old);
		bh_old->b_blocknr = fb->blocks[i];
		fb->blocks[i] = block;
	}

skip:
	brelse(bh_old);

	ci->old_index_block = ci->index_block;
	ci->index_block = new;

	return bh_new;

bcopy_file_block:
	while (i--)
		put_block(sbi, fb->blocks[i]);
bcopy_block:
	brelse(bh_old);
	brelse(bh_new);
	put_block(sbi, new);
bcopy_err:
	return ERR_PTR(err);
}

static struct buffer_head *copy_start(struct inode *inode)
{
	int err;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	uint32_t istore;

	invalidate_inode_buffers(inode);

	istore = get_free_istore(sbi);
	if (!istore) {
		err = -ENOSPC;
		goto copy_err;
	}

	err = put_inode(sb, ci->isrc);
	if (err)
		goto copy_put;

	bh = cow_copy_index(inode);
	if (IS_ERR(bh)) {
		err = PTR_ERR(bh);
		goto copy_bcopy;
	}

	ci->idest = istore;
	mark_inode_dirty_sync(inode);

	err = sync_inode_metadata(inode, false);
	if (err)
		goto copy_sync;

	return bh;

copy_sync:
	brelse(bh);
	ci->idest = 0;
	WARN_ON(ouichefs_put_index(sb, ci->index_block, inode->i_mode));
	ci->index_block = ci->old_index_block;
copy_bcopy:
	WARN_ON(get_inode(sb, ci->isrc));
copy_put:
	put_istore(sbi, istore);
copy_err:
	return ERR_PTR(err);
}

static void copy_commit(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	pr_debug("(ino %lu, istore %u -> %u)\n", inode->i_ino, ci->isrc,
		 ci->idest);

	get_ino(sbi, inode->i_ino);

	ci->isrc = ci->idest;
	ci->old_index_block = ci->index_block;
}

static void copy_discard(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	WARN_ON(get_inode(sb, ci->isrc));

	put_istore(sbi, ci->idest);
	WARN_ON(ouichefs_put_index(sb, ci->index_block, inode->i_mode));

	ci->idest = 0;
	ci->index_block = ci->old_index_block;
}

/*
 * Get inode from disk.
 */
struct inode *ouichefs_iget(struct super_block *sb, uint32_t ino,
			    uint32_t istore, int force_update)
{
	struct inode *inode = NULL;
	struct ouichefs_inode *disk_inode;
	struct ouichefs_inode_info *ci = NULL;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	int err, new;

	/* Fail if istore is out of range */
	if (istore >= sbi->nr_inodes)
		return ERR_PTR(-EINVAL);

	/* Get a locked inode from Linux */
	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ci = OUICHEFS_INODE(inode);

	/* If inode is in cache, return it */
	new = inode->i_state & I_NEW;
	if (!force_update && !new)
		return inode;

	/* Read inode from disk and initialize */
	bh = ouichefs_bread_inode(&disk_inode, sb, istore);
	if (!bh) {
		err = -EIO;
		goto failed;
	}

	inode->i_sb = sb;
	inode->i_op = &ouichefs_inode_ops;
	inode->i_ino = ino;
	ouichefs_ifill(inode, disk_inode);
	ci->isrc = istore;
	ci->idest = disk_inode->i_refcnt == 1 ? istore : 0;
	ci->index_block = disk_inode->index_block;

	brelse(bh);

	/* Unlock the inode to make it usable */
	if (new)
		unlock_new_inode(inode);

	return inode;

failed:
	brelse(bh);
	iget_failed(inode);
	return ERR_PTR(err);
}

void ouichefs_ifill(struct inode *inode, struct ouichefs_inode *disk_inode)
{
	inode->i_mode = disk_inode->i_mode;
	i_uid_write(inode, disk_inode->i_uid);
	i_gid_write(inode, disk_inode->i_gid);
	inode->i_size = disk_inode->i_size;
	inode->i_ctime.tv_sec = (time64_t)disk_inode->i_ctime;
	inode->i_ctime.tv_nsec = (long)disk_inode->i_nctime;
	inode->i_atime.tv_sec = (time64_t)disk_inode->i_atime;
	inode->i_atime.tv_nsec = (long)disk_inode->i_natime;
	inode->i_mtime.tv_sec = (time64_t)disk_inode->i_mtime;
	inode->i_mtime.tv_nsec = (long)disk_inode->i_nmtime;
	inode->i_blocks = disk_inode->i_blocks;
	set_nlink(inode, disk_inode->i_nlink);

	if (S_ISDIR(inode->i_mode)) {
		inode->i_fop = &ouichefs_dir_ops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_fop = &ouichefs_file_ops;
		inode->i_mapping->a_ops = &ouichefs_aops;
	}
}

int ouichefs_imut(struct super_block *sb, struct dentry *dentry)
{
	int err = 0;
	int depth = 0;
	struct dentry *d = dentry;
	struct inode *inode, *prev = NULL;
	struct ouichefs_inode_info *ci;
	struct ouichefs_dir_block *db;
	struct buffer_head *bh = NULL;

	/* Walk up until we find a mutable inode */
	while (d != NULL) {
		inode = d_inode(d);
		ci = OUICHEFS_INODE(inode);

		/* Is the inode already writable? */
		if (ci->idest)
			break;

		/* Make the inode writable */
		brelse(bh);
		bh = copy_start(inode);
		if (IS_ERR(bh)) {
			err = PTR_ERR(bh);
			bh = NULL;
			break;
		}

		/* Update a directory entry after a copy of a child */
		if (prev) {
			db = (struct ouichefs_dir_block *)bh->b_data;
			err = cow_update_dir_block(db, prev);
			if (err)
				break;
		}

		mark_buffer_dirty(bh);
		depth++;
		prev = inode;
		d = d->d_parent;
	}

	brelse(bh);

	/* Update a directory if an entry was updated */
	if (!err && prev) {
		bh = ouichefs_bread_dir(&db, sb, ci->index_block);
		if (bh)
			err = cow_update_dir_block(db, prev);
		else
			err = -EIO;

		mark_buffer_dirty(bh);
		brelse(bh);
	}

	/* Discard or commit pending changes */
	d = dentry;
	if (err) {
		while (depth--) {
			copy_discard(d_inode(d));
			d = d->d_parent;
		}
	} else {
		while (depth--) {
			copy_commit(d_inode(d));
			d = d->d_parent;
		}
	}

	return err;
}

/*
 * Look for dentry in dir.
 * Fill dentry with NULL if not in dir, with the corresponding inode if found.
 * Returns NULL on success.
 */
static struct dentry *ouichefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct ouichefs_inode_info *ci_dir = OUICHEFS_INODE(dir);
	struct inode *inode = NULL;
	struct buffer_head *bh = NULL;
	const struct ouichefs_dir_block *dblock = NULL;
	const struct ouichefs_file *f = NULL;
	int i;

	/* Check filename length */
	if (dentry->d_name.len > OUICHEFS_FILENAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	/* Read the directory index block on disk */
	bh = sb_bread(sb, ci_dir->index_block);
	if (!bh)
		return ERR_PTR(-EIO);
	dblock = (struct ouichefs_dir_block *)bh->b_data;

	/* Search for the file in directory */
	for (i = 0; i < OUICHEFS_MAX_DIR_FILES; i++) {
		f = &dblock->files[i];
		if (!f->ino)
			break;
		if (!strncmp(f->filename, dentry->d_name.name,
			     OUICHEFS_FILENAME_LEN)) {
			inode = ouichefs_iget(sb, f->ino, f->istore, false);
			break;
		}
	}
	brelse(bh);

	if (IS_ERR(inode))
		return ERR_CAST(inode);

	/* Fill the dentry with the inode */
	d_add(dentry, inode);

	return NULL;
}

/*
 * Create a new inode in dir.
 */
static struct inode *ouichefs_new_inode(struct inode *dir, mode_t mode)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci;
	struct buffer_head *bh;
	struct super_block *sb = dir->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t ino, istore, bno;
	int ret;

	/* Check mode before doing anything to avoid undoing everything */
	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		pr_err("File type not supported (only directory and regular files supported)\n");
		return ERR_PTR(-EINVAL);
	}

	istore = get_free_istore(sbi);
	if (!istore)
		return ERR_PTR(-ENOSPC);

	/* Load new istore entry from disk */
	ino = get_free_ino(sbi);
	inode = ouichefs_iget(sb, ino, istore, false);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto put_istore;
	}

	/* Get a free block for this new inode's index */
	bno = get_free_block(sbi);
	if (!bno) {
		ret = -ENOSPC;
		goto put_inode;
	}

	/* Initialize ouichefs specific fields */
	ci = OUICHEFS_INODE(inode);
	ci->idest = istore;
	ci->index_block = bno;

	/* Initialize new index block */
	bh = sb_bread(sb, bno);
	if (!bh) {
		ret = -EIO;
		goto put_block;
	}
	memset(bh->b_data, 0, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Initialize inode */
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_blocks = 1;
	if (S_ISDIR(mode)) {
		inode->i_size = OUICHEFS_BLOCK_SIZE;
		inode->i_fop = &ouichefs_dir_ops;
		set_nlink(inode, 2); /* . and .. */
	} else if (S_ISREG(mode)) {
		inode->i_size = 0;
		inode->i_fop = &ouichefs_file_ops;
		inode->i_mapping->a_ops = &ouichefs_aops;
		set_nlink(inode, 1);
	}
	inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
	mark_inode_dirty_sync(inode);

	ret = sync_inode_metadata(inode, false);
	if (ret)
		goto put_block;

	return inode;

put_block:
	put_block(sbi, bno);
put_inode:
	iput(inode);
put_istore:
	put_ino(sbi, ino);
	put_istore(sbi, istore);

	return ERR_PTR(ret);
}

/*
 * Create a file or directory in this way:
 *   - check filename length and if the parent directory is not full
 *   - create the new inode (allocate inode and blocks)
 *   - cleanup index block of the new inode
 *   - add new file/directory in parent index
 */
static int ouichefs_create(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci, *ci_dir = OUICHEFS_INODE(dir);
	struct super_block *sb = dir->i_sb;
	struct ouichefs_dir_block *dblock;
	struct buffer_head *bh;
	int err = 0, i;

	/* Check filename length */
	if (strlen(dentry->d_name.name) > OUICHEFS_FILENAME_LEN)
		return -ENAMETOOLONG;

	/* Make the parent directory modifiable */
	err = ouichefs_imut(sb, dentry->d_parent);
	if (err)
		return err;

	/* Read parent directory index */
	bh = ouichefs_bread_dir(&dblock, sb, ci_dir->index_block);
	if (!bh)
		return -EIO;

	/* Check if parent directory is full */
	if (dblock->files[OUICHEFS_MAX_DIR_FILES - 1].ino != 0) {
		err = -EMLINK;
		goto end;
	}

	/* Get a new free inode */
	inode = ouichefs_new_inode(dir, mode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto end;
	}

	/* Find first free slot in parent index and register new inode */
	for (i = 0; i < OUICHEFS_MAX_DIR_FILES; i++)
		if (dblock->files[i].ino == 0)
			break;

	ci = OUICHEFS_INODE(inode);
	dblock->files[i].ino = inode->i_ino;
	dblock->files[i].istore = ci->idest;
	strscpy(dblock->files[i].filename, dentry->d_name.name,
		OUICHEFS_FILENAME_LEN);
	pr_debug(
		"(name '%s', ino %lu, istore %u, index %u) files[%i] = (ino %u, istore %u)\n",
		dentry->d_parent->d_name.name, dir->i_ino, ci_dir->isrc,
		ci_dir->index_block, i, dblock->files[i].ino,
		dblock->files[i].istore);
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Update stats and mark dir and new inode dirty */
	dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (S_ISDIR(mode))
		inode_inc_link_count(dir);
	mark_inode_dirty(dir);

	/* setup dentry */
	d_instantiate(dentry, inode);

	return 0;

end:
	brelse(bh);
	return err;
}

/*
 * Remove a link for a file. If link count is 0, destroy file in this way:
 *   - remove the file from its parent directory.
 *   - cleanup blocks containing data
 *   - cleanup file index block
 *   - cleanup inode
 */
static int ouichefs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh = NULL;
	struct ouichefs_dir_block *db = NULL;
	int err, i, f_id = -1, nr_subs = 0;

	/* Make the parent directory modifiable */
	err = ouichefs_imut(sb, dentry->d_parent);
	if (err)
		return err;

	/* Read parent directory index */
	bh = sb_bread(sb, OUICHEFS_INODE(dir)->index_block);
	if (!bh)
		return -EIO;

	/* Search for inode in parent index and get number of subfiles */
	db = (struct ouichefs_dir_block *)bh->b_data;
	for (i = 0; i < OUICHEFS_MAX_DIR_FILES; i++) {
		if (db->files[i].ino == inode->i_ino)
			f_id = i;
		else if (db->files[i].ino == 0)
			break;
	}
	nr_subs = i;

	/* Couldn't find ino */
	if (f_id < 0) {
		brelse(bh);
		return -ENOENT;
	}

	/* Remove file from parent directory */
	if (f_id != OUICHEFS_MAX_DIR_FILES - 1)
		memmove(db->files + f_id, db->files + f_id + 1,
			(nr_subs - f_id - 1) * sizeof(struct ouichefs_file));
	memset(&db->files[nr_subs - 1], 0, sizeof(struct ouichefs_file));
	pr_debug("(name '%s', ino %lu, istore %u, index %u) files[%i] unlink\n",
		 dentry->d_parent->d_name.name, dir->i_ino,
		 OUICHEFS_INODE(dir)->isrc, OUICHEFS_INODE(dir)->index_block,
		 f_id);
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Update inode stats */
	dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (S_ISDIR(inode->i_mode))
		inode_dec_link_count(dir);
	mark_inode_dirty(dir);

	WARN_ON(put_inode(sb, OUICHEFS_INODE(inode)->isrc));

	return 0;
}

static int ouichefs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			   struct dentry *old_dentry, struct inode *new_dir,
			   struct dentry *new_dentry, unsigned int flags)
{
	struct super_block *sb = old_dir->i_sb;
	struct ouichefs_inode_info *ci_old = OUICHEFS_INODE(old_dir);
	struct ouichefs_inode_info *ci_new = OUICHEFS_INODE(new_dir);
	struct inode *src = d_inode(old_dentry);
	struct buffer_head *bh_old = NULL, *bh_new = NULL;
	struct ouichefs_dir_block *db_old, *db_new;
	int i, old_pos, new_pos, ret = 0, total_files;

	/* fail with these unsupported flags */
	if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT)) {
		ret = -EINVAL;
		goto rename_end;
	}

	/* Check if filename is not too long */
	if (strlen(new_dentry->d_name.name) > OUICHEFS_FILENAME_LEN) {
		ret = -ENAMETOOLONG;
		goto rename_end;
	}

	/* Modify the old directory */
	ret = ouichefs_imut(sb, old_dentry->d_parent);
	if (ret)
		goto rename_end;

	/* Modify the new directory */
	ret = ouichefs_imut(sb, new_dentry->d_parent);
	if (ret)
		goto rename_end;

	bh_new = sb_bread(sb, ci_new->index_block);
	if (!bh_new) {
		ret = -EIO;
		goto rename_end;
	}

	/* Scan new directory block */
	db_new = (struct ouichefs_dir_block *)bh_new->b_data;
	old_pos = -1;
	for (i = 0; i < OUICHEFS_MAX_DIR_FILES && db_new->files[i].istore;
	     i++) {
		/* If the old file is in this dir save the old file position */
		if (db_new->files[i].ino == src->i_ino)
			old_pos = i;

		/* New file already exists */
		if (!strncmp(db_new->files[i].filename, new_dentry->d_name.name,
			     OUICHEFS_FILENAME_LEN)) {
			ret = -EEXIST;
			goto rename_end;
		}
	}
	new_pos = i;

	/* If old_dir == new_dir, just update the name */
	if (old_dir == new_dir) {
		if (old_pos < 0) {
			ret = -ENOENT;
			goto rename_end;
		}

		new_pos = old_pos;
		goto rename_update_new_name;
	}

	/* If new directory is full, fail */
	if (new_pos == OUICHEFS_MAX_DIR_FILES) {
		ret = -EMLINK;
		goto rename_end;
	}

	bh_old = sb_bread(sb, ci_old->index_block);
	if (!bh_old) {
		ret = -EIO;
		goto rename_end;
	}

	/* Find old directory entry */
	db_old = (struct ouichefs_dir_block *)bh_old->b_data;
	old_pos = -1;
	for (i = 0; i < OUICHEFS_MAX_DIR_FILES && db_old->files[i].istore;
	     i++) {
		if (db_old->files[i].ino == src->i_ino)
			old_pos = i;
	}
	total_files = i;

	if (old_pos < 0) {
		ret = -ENOENT;
		goto rename_end;
	}

	/* Remove file from old parent directory */
	if (old_pos != OUICHEFS_MAX_DIR_FILES - 1)
		memmove(db_old->files + old_pos, db_old->files + old_pos + 1,
			(total_files - old_pos - 1) *
				sizeof(struct ouichefs_file));
	pr_debug("(name '%s', ino %lu, istore %u, index %u) files[%i] unlink\n",
		 old_dentry->d_parent->d_name.name, old_dir->i_ino,
		 ci_old->isrc, ci_old->index_block, old_pos);
	memset(&db_old->files[total_files - 1], 0,
	       sizeof(struct ouichefs_file));
	mark_buffer_dirty(bh_old);

	/* Update old parent inode metadata */
	old_dir->i_ctime = old_dir->i_mtime = current_time(old_dir);

	/* Link counts were changed */
	if (S_ISDIR(src->i_mode)) {
		inode_dec_link_count(old_dir);
		inode_inc_link_count(new_dir);
	}

	/* Old inode was modified */
	mark_inode_dirty(old_dir);

	/* Insert in new parent directory */
	db_new->files[new_pos].ino = src->i_ino;
	db_new->files[new_pos].istore = OUICHEFS_INODE(src)->isrc;
rename_update_new_name:
	strscpy(db_new->files[new_pos].filename, new_dentry->d_name.name,
		OUICHEFS_FILENAME_LEN);
	pr_debug(
		"(name '%s', ino %lu, istore %u, index %u) files[%i] = (ino %lu, istore %u)\n",
		new_dentry->d_parent->d_name.name, new_dir->i_ino, ci_new->isrc,
		ci_new->index_block, new_pos, src->i_ino,
		OUICHEFS_INODE(src)->isrc);
	mark_buffer_dirty(bh_new);

	/* Update new parent inode metadata */
	new_dir->i_ctime = new_dir->i_mtime = current_time(new_dir);
	mark_inode_dirty(new_dir);

rename_end:
	brelse(bh_old);
	brelse(bh_new);
	return ret;
}

static int ouichefs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode)
{
	return ouichefs_create(NULL, dir, dentry, mode | S_IFDIR, 0);
}

static int ouichefs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh;
	struct ouichefs_dir_block *dblock;

	/* If the directory is not empty, fail */
	if (inode->i_nlink > 2)
		return -ENOTEMPTY;
	bh = ouichefs_bread_dir(&dblock, sb,
				OUICHEFS_INODE(inode)->index_block);
	if (!bh)
		return -EIO;
	if (dblock->files[0].istore != 0) {
		brelse(bh);
		return -ENOTEMPTY;
	}
	brelse(bh);

	/* Remove directory with unlink */
	return ouichefs_unlink(dir, dentry);
}

static const struct inode_operations ouichefs_inode_ops = {
	.lookup = ouichefs_lookup,
	.create = ouichefs_create,
	.unlink = ouichefs_unlink,
	.mkdir = ouichefs_mkdir,
	.rmdir = ouichefs_rmdir,
	.rename = ouichefs_rename,
};
