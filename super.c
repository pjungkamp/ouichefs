// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/writeback.h>
#include <linux/pagemap.h>

#include "ouichefs.h"
#include "bitmap.h"

static struct kmem_cache *ouichefs_inode_cache;

int ouichefs_init_inode_cache(void)
{
	ouichefs_inode_cache = kmem_cache_create(
		"ouichefs_cache", sizeof(struct ouichefs_inode_info), 0, 0,
		NULL);
	if (!ouichefs_inode_cache)
		return -ENOMEM;
	return 0;
}

void ouichefs_destroy_inode_cache(void)
{
	kmem_cache_destroy(ouichefs_inode_cache);
}

static struct inode *ouichefs_alloc_inode(struct super_block *sb)
{
	struct ouichefs_inode_info *ci;

	/* ci = kzalloc(sizeof(struct ouichefs_inode_info), GFP_KERNEL); */
	ci = kmem_cache_alloc(ouichefs_inode_cache, GFP_KERNEL);
	if (!ci)
		return NULL;

	inode_init_once(&ci->vfs_inode);
	return &ci->vfs_inode;
}

static void ouichefs_destroy_inode(struct inode *inode)
{
	struct ouichefs_inode_info *ci;

	ci = OUICHEFS_INODE(inode);
	kmem_cache_free(ouichefs_inode_cache, ci);
}

static int ouichefs_write_inode(struct inode *inode,
				struct writeback_control *wbc)
{
	struct ouichefs_inode *disk_inode;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;

	if (!ci->idest) {
		pr_err("can't write shared inode %lu\n", inode->i_ino);
		return 0;
	}

	if (ci->idest >= sbi->nr_inodes)
		return 0;

	bh = ouichefs_bread_inode(&disk_inode, sb, ci->idest);
	if (!bh)
		return -EIO;

	pr_debug("(ino %lu, istore %u)\n", inode->i_ino, ci->idest);

	/* update the inode on disk */
	disk_inode->i_no = inode->i_ino;
	disk_inode->i_mode = inode->i_mode;
	disk_inode->i_uid = i_uid_read(inode);
	disk_inode->i_gid = i_gid_read(inode);
	disk_inode->i_size = inode->i_size;
	disk_inode->i_ctime = inode->i_ctime.tv_sec;
	disk_inode->i_nctime = inode->i_ctime.tv_nsec;
	disk_inode->i_atime = inode->i_atime.tv_sec;
	disk_inode->i_natime = inode->i_atime.tv_nsec;
	disk_inode->i_mtime = inode->i_mtime.tv_sec;
	disk_inode->i_nmtime = inode->i_mtime.tv_nsec;
	disk_inode->i_blocks = inode->i_blocks;
	disk_inode->i_nlink = inode->i_nlink;
	disk_inode->i_refcnt = 1;
	disk_inode->index_block = ci->index_block;

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int sync_sb_info(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_sb_info *disk_sb;
	struct buffer_head *bh;

	/* Flush superblock */
	bh = sb_bread(sb, OUICHEFS_SB_BLOCK_NR);
	if (!bh)
		return -EIO;
	disk_sb = (struct ouichefs_sb_info *)bh->b_data;

	disk_sb->nr_blocks = sbi->nr_blocks;
	disk_sb->nr_inodes = sbi->nr_inodes;
	disk_sb->nr_istore_blocks = sbi->nr_istore_blocks;
	disk_sb->nr_ifree_blocks = sbi->nr_ifree_blocks;
	disk_sb->nr_iref_blocks = sbi->nr_iref_blocks;
	disk_sb->nr_bfree_blocks = sbi->nr_bfree_blocks;
	disk_sb->nr_free_inodes = sbi->nr_free_inodes;
	disk_sb->nr_free_blocks = sbi->nr_free_blocks;
	disk_sb->nr_snapshots = sbi->nr_snapshots;

	memcpy(bh->b_data + OUICHEFS_SNAPSHOTS_OFFSET, sbi->snapshots,
	       sbi->nr_snapshots * sizeof(sbi->snapshots[0]));

	mark_buffer_dirty(bh);
	if (wait)
		sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int sync_ifree(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	int i, idx;

	/* Flush free inodes bitmask */
	for (i = 0; i < sbi->nr_ifree_blocks; i++) {
		idx = OUICHEFS_SBI_IFREE_BLOCK_OFFSET(sbi) + i;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data,
		       (void *)sbi->ifree_bitmap + i * OUICHEFS_BLOCK_SIZE,
		       OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static int sync_iref(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	int i, idx;

	/* Flush free ino refcnt store */
	for (i = 0; i < sbi->nr_iref_blocks; i++) {
		idx = OUICHEFS_SBI_IREF_BLOCK_OFFSET(sbi) + i;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data, &sbi->iref[i * OUICHEFS_IREF_PER_BLOCK],
		       OUICHEFS_IREF_PER_BLOCK);
		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static int sync_bfree(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	int i, idx;

	/* Flush free blocks bitmask */
	for (i = 0; i < sbi->nr_bfree_blocks; i++) {
		idx = OUICHEFS_SBI_BFREE_BLOCK_OFFSET(sbi) + i;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data,
		       (void *)sbi->bfree_bitmap + i * OUICHEFS_BLOCK_SIZE,
		       OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static void ouichefs_put_super(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (sbi) {
		kfree(sbi->ifree_bitmap);
		kfree(sbi->iref);
		kfree(sbi->bfree_bitmap);
		kfree(sbi->snapshots);
		kfree(sbi);
	}
}

static int ouichefs_sync_fs(struct super_block *sb, int wait)
{
	int ret = 0;

	ret = sync_sb_info(sb, wait);
	if (ret)
		return ret;
	ret = sync_ifree(sb, wait);
	if (ret)
		return ret;
	ret = sync_iref(sb, wait);
	if (ret)
		return ret;
	ret = sync_bfree(sb, wait);
	if (ret)
		return ret;

	return 0;
}

static int ouichefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
	struct super_block *sb = dentry->d_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	stat->f_type = OUICHEFS_MAGIC;
	stat->f_bsize = OUICHEFS_BLOCK_SIZE;
	stat->f_blocks = sbi->nr_blocks;
	stat->f_bfree = sbi->nr_free_blocks;
	stat->f_bavail = sbi->nr_free_blocks;
	stat->f_files = sbi->nr_inodes;
	stat->f_ffree = sbi->nr_free_inodes;
	stat->f_namelen = OUICHEFS_FILENAME_LEN;

	return 0;
}

static struct super_operations ouichefs_super_ops = {
	.put_super = ouichefs_put_super,
	.alloc_inode = ouichefs_alloc_inode,
	.destroy_inode = ouichefs_destroy_inode,
	.write_inode = ouichefs_write_inode,
	.sync_fs = ouichefs_sync_fs,
	.statfs = ouichefs_statfs,
};

/* Fill the struct superblock from partition superblock */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *bh = NULL;
	struct ouichefs_sb_info *csb = NULL;
	struct ouichefs_sb_info *sbi = NULL;
	struct inode *root_inode = NULL;
	int ret = 0, i;

	/* Init sb */
	sb->s_magic = OUICHEFS_MAGIC;
	sb_set_blocksize(sb, OUICHEFS_BLOCK_SIZE);
	sb->s_maxbytes = OUICHEFS_MAX_FILESIZE;
	sb->s_op = &ouichefs_super_ops;
	sb->s_time_gran = 1;

	/* Read sb from disk */
	bh = sb_bread(sb, OUICHEFS_SB_BLOCK_NR);
	if (!bh)
		return -EIO;
	csb = (struct ouichefs_sb_info *)bh->b_data;

	/* Check magic number */
	if (csb->magic != sb->s_magic) {
		pr_err("Wrong magic number\n");
		ret = -EPERM;
		goto release;
	}

	/* Alloc sb_info */
	sbi = kzalloc(sizeof(struct ouichefs_sb_info), GFP_KERNEL);
	if (!sbi) {
		ret = -ENOMEM;
		goto release;
	}
	sb->s_fs_info = sbi;

	sbi->nr_blocks = csb->nr_blocks;
	sbi->nr_inodes = csb->nr_inodes;
	sbi->nr_istore_blocks = csb->nr_istore_blocks;
	sbi->nr_ifree_blocks = csb->nr_ifree_blocks;
	sbi->nr_iref_blocks = csb->nr_iref_blocks;
	sbi->nr_bfree_blocks = csb->nr_bfree_blocks;
	sbi->nr_free_inodes = csb->nr_free_inodes;
	sbi->nr_free_blocks = csb->nr_free_blocks;
	sbi->nr_snapshots = csb->nr_snapshots;

	/* Alloc and copy snapshots array */
	sbi->snapshots = kcalloc(OUICHEFS_MAX_SNAPSHOTS,
				 sizeof(struct ouichefs_snapshot), GFP_KERNEL);
	if (!sbi->snapshots) {
		ret = -ENOMEM;
		goto free_sbi;
}
	memcpy(sbi->snapshots, bh->b_data + OUICHEFS_SNAPSHOTS_OFFSET,
	       sbi->nr_snapshots * sizeof(struct ouichefs_snapshot));

	brelse(bh);

	/* Alloc and copy ifree_bitmap */
	sbi->ifree_bitmap =
		kzalloc(sbi->nr_ifree_blocks * OUICHEFS_BLOCK_SIZE, GFP_KERNEL);
	if (!sbi->ifree_bitmap) {
		ret = -ENOMEM;
		bh = NULL;
		goto free_snapshots;
	}
	for (i = 0; i < sbi->nr_ifree_blocks; i++) {
		int idx = OUICHEFS_SBI_IFREE_BLOCK_OFFSET(sbi) + i;

		bh = sb_bread(sb, idx);
		if (!bh) {
			ret = -EIO;
			goto free_ifree;
		}

		memcpy((void *)sbi->ifree_bitmap + i * OUICHEFS_BLOCK_SIZE,
		       bh->b_data, OUICHEFS_BLOCK_SIZE);

		brelse(bh);
	}

	/* Alloc and copy iref */
	sbi->iref = kzalloc(sbi->nr_iref_blocks * OUICHEFS_IREF_PER_BLOCK *
				    sizeof(ouichefs_iref_t),
			    GFP_KERNEL);
	if (!sbi->iref) {
		ret = -ENOMEM;
		bh = NULL;
		goto free_ifree;
	}
	for (i = 0; i < sbi->nr_iref_blocks; i++) {
		int idx = OUICHEFS_SBI_IREF_BLOCK_OFFSET(sbi) + i;

		bh = sb_bread(sb, idx);
		if (!bh) {
			ret = -EIO;
			goto free_ifree;
		}

		memcpy(sbi->iref + i * OUICHEFS_IREF_PER_BLOCK, bh->b_data,
		       OUICHEFS_IREF_PER_BLOCK * sizeof(ouichefs_iref_t));

		brelse(bh);
	}

	/* Alloc and copy bfree_bitmap */
	sbi->bfree_bitmap =
		kzalloc(sbi->nr_bfree_blocks * OUICHEFS_BLOCK_SIZE, GFP_KERNEL);
	if (!sbi->bfree_bitmap) {
		ret = -ENOMEM;
		bh = NULL;
		goto free_iref;
	}
	for (i = 0; i < sbi->nr_bfree_blocks; i++) {
		int idx = OUICHEFS_SBI_BFREE_BLOCK_OFFSET(sbi) + i;

		bh = sb_bread(sb, idx);
		if (!bh) {
			ret = -EIO;
			goto free_bfree;
		}

		memcpy((void *)sbi->bfree_bitmap + i * OUICHEFS_BLOCK_SIZE,
		       bh->b_data, OUICHEFS_BLOCK_SIZE);

		brelse(bh);
	}

	/* Create root inode */
	root_inode = ouichefs_iget(sb, 1, 1, true);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		bh = NULL;
		goto free_bfree;
	}
	inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto iput;
	}

	return 0;

iput:
	iput(root_inode);
free_bfree:
	kfree(sbi->bfree_bitmap);
free_iref:
	kfree(sbi->iref);
free_ifree:
	kfree(sbi->ifree_bitmap);
free_snapshots:
	kfree(sbi->snapshots);
free_sbi:
	kfree(sbi);
release:
	brelse(bh);

	return ret;
}

struct buffer_head *ouichefs_bread_inode(struct ouichefs_inode **inode,
					 struct super_block *sb,
					 uint32_t istore)
{
	struct buffer_head *bh;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t block = istore / OUICHEFS_INODES_PER_BLOCK;
	uint32_t offset = istore % OUICHEFS_INODES_PER_BLOCK;

	bh = sb_bread(sb, OUICHEFS_SBI_ISTORE_BLOCK_OFFSET(sbi) + block);
	if (!bh)
		return NULL;

	if (inode)
		*inode = ((struct ouichefs_inode *)bh->b_data) + offset;

	return bh;
}

int ouichefs_put_inode(struct super_block *sb, uint32_t istore,
		       struct ouichefs_inode *inode)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct inode *cached = ilookup(sb, inode->i_no);

	if (cached) {
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(cached);

		/* Make previously shared inode writable */
		if (!ci->idest && ci->isrc == istore && inode->i_refcnt == 2)
			ci->idest = istore;

		iput(cached);
	}

	pr_debug("(ino %u, istore %u, index %u) put\n", inode->i_no, istore,
		 inode->index_block);

	inode->i_refcnt--;
	if (inode->i_refcnt)
		goto skip;

	pr_debug("(ino %u, istore %u, index %u) discard\n", inode->i_no, istore,
		 inode->index_block);

	put_ino(sbi, inode->i_no);
	put_istore(sbi, istore);
	ouichefs_put_index(sb, inode->index_block, inode->i_mode);

skip:
	return 0;
}

int ouichefs_get_inode(struct super_block *sb, uint32_t istore,
		       struct ouichefs_inode *inode)
{
	int err;
	struct inode *cached = ilookup(sb, inode->i_no);

	if (cached) {
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(cached);

		if (ci->idest == istore) {
			err = sync_inode_metadata(cached, true);
			if (err) {
				iput(cached);
				return err;
			}

			err = filemap_fdatawait(cached->i_mapping);
			if (err) {
				iput(cached);
				return err;
			}

			ci->idest = 0;
		}

		iput(cached);
	}

	pr_debug("(ino %u, istore %u, index %u) get\n", inode->i_no, istore,
		 inode->index_block);

	inode->i_refcnt++;
	return 0;
}

struct buffer_head *ouichefs_bread_file(struct ouichefs_file_block **fb,
					struct super_block *sb, uint32_t bno)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, bno);
	if (!bh)
		return NULL;

	*fb = (struct ouichefs_file_block *)bh->b_data;

	return bh;
}

struct buffer_head *ouichefs_bread_dir(struct ouichefs_dir_block **db,
				       struct super_block *sb, uint32_t bno)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, bno);
	if (!bh)
		return ERR_PTR(-EIO);

	*db = (struct ouichefs_dir_block *)bh->b_data;

	return bh;
}

int ouichefs_put_index(struct super_block *sb, uint32_t index_block,
		       mode_t mode)
{
	int ret = 0;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_file_block *fb;
	struct buffer_head *bh = NULL;

	if (!S_ISREG(mode))
		goto skip;

	bh = ouichefs_bread_file(&fb, sb, index_block);
	if (!bh) {
		ret = -EIO;
		goto skip;
	}

	for (int i = 0; i < OUICHEFS_MAX_FILE_BLOCKS; i++) {
		if (!fb->blocks[i])
			break;

		put_block(sbi, fb->blocks[i]);
	}

skip:
	brelse(bh);
	put_block(sbi, index_block);
	return ret;
}
