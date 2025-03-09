// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2025 Philipp Jungkamp <philipp.jungkamp@rwth-aachen.de>
 * Copyright (C) 2025 Jonas Dohmen <jonas.dohmen@rwth-aachen.de>
 * Copyright (C) 2025 Antonios Salios <antonios.salios@rwth-aachen.de>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/dcache.h>
#include <linux/rwsem.h>
#include <linux/semaphore.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/writeback.h>

#include "ouichefs.h"
#include "bitmap.h"

static int copy_root(struct super_block *sb, uint32_t istore, int backup)
{
	int ret = 0;
	uint32_t root_index, copy_index;
	struct inode *iroot = d_inode(sb->s_root);
	struct ouichefs_inode *root, *copy;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh_root = NULL, *bh_copy = NULL;

	up_read(&sb->s_umount);

	sync_inodes_sb(sb);
	shrink_dcache_sb(sb);
	evict_inodes(sb);

	bh_root = ouichefs_bread_inode(&root, sb, 1);
	bh_copy = ouichefs_bread_inode(&copy, sb, istore);
	if (!bh_root || !bh_copy) {
		ret = -EIO;
		goto ret;
	}

	if (backup) {
		copy->index_block = get_free_block(sbi);
		if (!copy->index_block) {
			ret = -ENOSPC;
			goto ret;
		}
	}

	root_index = root->index_block;
	copy_index = copy->index_block;

	if (backup) {
		pr_debug("backup (istore 1 -> %u)\n", istore);
		memcpy(copy, root, sizeof(*root));
		copy->index_block = copy_index;
		mark_buffer_dirty(bh_copy);
	} else {
		pr_debug("restore (istore %u -> 1)\n", istore);
		memcpy(root, copy, sizeof(*root));
		root->index_block = root_index;
		mark_buffer_dirty(bh_root);
		ouichefs_ifill(iroot, root);
	}

	brelse(bh_root);
	brelse(bh_copy);
	bh_root = sb_bread(sb, root_index);
	bh_copy = sb_bread(sb, copy_index);
	if (!bh_root || !bh_copy) {
		if (backup)
			put_block(sbi, copy->index_block);

		ret = -EIO;
		goto ret;
	}

	if (backup) {
		pr_debug("backup (index %u -> %u)\n", root_index, copy_index);
		memcpy(bh_copy->b_data, bh_root->b_data, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh_copy);
	} else {
		pr_debug("restore (index %u -> %u)\n", copy_index, root_index);
		memcpy(bh_root->b_data, bh_copy->b_data, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh_root);
	}

ret:
	brelse(bh_copy);
	brelse(bh_root);
	down_read(&sb->s_umount);

	return ret;
}

static uint32_t read_root_index_block(struct super_block *sb, uint32_t istore)
{
	uint32_t index_block;
	struct ouichefs_inode *root;
	struct buffer_head *bh = NULL;

	bh = ouichefs_bread_inode(&root, sb, istore);
	if (!bh)
		return 0;

	index_block = root->index_block;
	brelse(bh);
	return index_block;
}

static struct ouichefs_snapshot *snapshot_find(struct super_block *sb,
					       uint32_t id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	int i;

	for (i = 0; i < sbi->nr_snapshots; i++) {
		if (sbi->snapshots[i].s_id == id)
			break;
	}

	if (i == sbi->nr_snapshots)
		return ERR_PTR(-ESRCH);

	return &sbi->snapshots[i];
}

static struct ouichefs_snapshot *snapshot_add(struct super_block *sb,
					      uint32_t id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	int err, i;

	if (sbi->nr_snapshots >= OUICHEFS_MAX_SNAPSHOTS) {
		err = -ENOSPC;
		goto err_snapshot_add;
	}

	for (i = 0; i < sbi->nr_snapshots; i++) {
		if (sbi->snapshots[i].s_id >= id)
			break;
	}

	if (i < sbi->nr_snapshots) {
		if (sbi->snapshots[i].s_id == id) {
			err = -EEXIST;
			goto err_snapshot_add;
		}

		memmove(&sbi->snapshots[i + 1], &sbi->snapshots[i],
			(sbi->nr_snapshots - i) *
				sizeof(struct ouichefs_snapshot));
	}

	sbi->nr_snapshots++;

	return &sbi->snapshots[i];

err_snapshot_add:
	return ERR_PTR(err);
}

static void snapshot_del(struct super_block *sb, struct ouichefs_snapshot *snap)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	memmove(snap, snap + 1,
		(&sbi->snapshots[sbi->nr_snapshots - 1] - snap) *
			sizeof(struct ouichefs_snapshot));

	sbi->nr_snapshots--;
}

int ouichefs_snapshot_create(struct super_block *sb, uint32_t id)
{
	int err;
	uint32_t istore;
	struct ouichefs_snapshot *snap;
	struct inode *root = sb->s_root->d_inode;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	snap = snapshot_add(sb, id);
	if (IS_ERR(snap)) {
		err = PTR_ERR(snap);
		goto create_err;
	}

	istore = get_free_istore(sbi);
	if (!istore) {
		err = PTR_ERR(snap);
		goto create_istore;
	}

	err = copy_root(sb, istore, true);
	if (err)
		goto create_copy;

	err = ouichefs_walk(sb, OUICHEFS_INODE(root)->index_block,
			    ouichefs_get_inode, ouichefs_put_inode);
	if (err)
		goto create_copy;

	*snap = (struct ouichefs_snapshot){
		.s_id = id,
		.s_root = istore,
		.s_time = current_time(root).tv_sec,
	};

	return 0;

create_copy:
	put_istore(sbi, istore);
create_istore:
	snapshot_del(sb, snap);
create_err:
	return err;
}

int ouichefs_snapshot_restore(struct super_block *sb, uint32_t id)
{
	int err;
	struct ouichefs_snapshot *snap;
	struct dentry *droot = sb->s_root;
	struct inode *iroot = droot->d_inode;

	snap = snapshot_find(sb, id);
	if (IS_ERR(snap)) {
		err = PTR_ERR(snap);
		goto restore_err;
	}

	err = ouichefs_walk(sb, OUICHEFS_INODE(iroot)->index_block,
			    ouichefs_put_inode, ouichefs_get_inode);
	if (err)
		goto restore_old;

	err = copy_root(sb, snap->s_root, false);
	if (err)
		goto restore_new;

	mark_inode_dirty(iroot);

	err = ouichefs_walk(sb, OUICHEFS_INODE(iroot)->index_block,
			    ouichefs_get_inode, ouichefs_put_inode);
	if (err)
		goto restore_new;

	return 0;

restore_new:
	err = ouichefs_walk(sb, OUICHEFS_INODE(iroot)->index_block,
			    ouichefs_get_inode, ouichefs_put_inode);
restore_old:
	snapshot_del(sb, snap);
restore_err:
	return err;
}

int ouichefs_snapshot_destroy(struct super_block *sb, uint32_t id)
{
	int err;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot *snap;
	uint32_t root_index_block;

	snap = snapshot_find(sb, id);
	if (IS_ERR(snap)) {
		pr_err("could not find a snapshot with id %u\n", id);
		err = PTR_ERR(snap);
		goto destroy_err;
	}

	root_index_block = read_root_index_block(sb, snap->s_root);
	if (!root_index_block) {
		pr_err("could load snapshot index for root %u\n", snap->s_root);
		err = -EIO;
		goto destroy_err;
	}

	err = ouichefs_walk(sb, root_index_block, ouichefs_put_inode,
			    ouichefs_get_inode);
	if (err) {
		pr_err("could not walk snapshot filesystem\n");
		goto destroy_err;
	}

	put_istore(sbi, snap->s_root);
	put_block(sbi, root_index_block);
	snapshot_del(sb, snap);

	return 0;

destroy_err:
	return err;
}
