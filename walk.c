// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
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

struct walk_dir_state {
	struct walk_dir_state *parent;
	uint32_t cursor;
	uint32_t entries[OUICHEFS_MAX_DIR_FILES];
};

int state_push(struct super_block *sb, uint32_t index_block, uint32_t cursor,
	       struct walk_dir_state **state)
{
	int err;
	struct buffer_head *bh;
	struct ouichefs_dir_block *db;
	struct walk_dir_state *new_state;

	new_state = kzalloc(sizeof(*new_state), GFP_KERNEL);
	if (!new_state) {
		err = -ENOMEM;
		goto state_append_alloc;
	}

	bh = ouichefs_bread_dir(&db, sb, index_block);
	if (!bh) {
		err = -EIO;
		goto state_append_bread;
	}

	new_state->parent = *state;
	new_state->cursor = cursor;
	for (int i = 0; i < OUICHEFS_MAX_DIR_FILES && db->files[i].ino; i++)
		new_state->entries[i] = db->files[i].istore;

	*state = new_state;

	brelse(bh);

	return 0;

state_append_bread:
	kfree(new_state);
state_append_alloc:
	return err;
}

void state_pop(struct walk_dir_state **state)
{
	struct walk_dir_state *parent = (*state)->parent;

	kfree(*state);
	*state = parent;
}

int ouichefs_walk(struct super_block *sb, uint32_t root_index_block,
		  ouichefs_walk_cb visit, ouichefs_walk_cb revert)
{
	int err;
	uint32_t istore, visited = 0, reverted = 0;
	struct buffer_head *bh = NULL;
	struct ouichefs_inode *inode;
	struct walk_dir_state *state = NULL;

	pr_debug("(index %u)", root_index_block);

	err = state_push(sb, root_index_block, 0, &state);
	if (err)
		goto revert;

	while (state) {
		if (state->cursor == OUICHEFS_MAX_DIR_FILES) {
			state_pop(&state);
			continue;
		}

		istore = state->entries[state->cursor];
		if (!istore) {
			state_pop(&state);
			continue;
		}

		bh = ouichefs_bread_inode(&inode, sb, istore);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto revert;
		}

		pr_debug("  (ino %u, istore %u, index %u, %s)\n", inode->i_no,
			 istore, inode->index_block,
			 S_ISDIR(inode->i_mode) ? "dir" : "file");

		state->cursor++;

		if (S_ISDIR(inode->i_mode)) {
			err = state_push(sb, inode->index_block, 0, &state);
			if (err)
				goto revert;
		}

		err = visit(sb, istore, inode);
		if (err)
			goto revert;
		visited++;

		mark_buffer_dirty(bh);
		brelse(bh);
	}

	return 0;

revert:
	brelse(bh);

	while (state) {
		if (!state->cursor) {
			state_pop(&state);
			continue;
		}

		do {
			state->cursor--;
			istore = state->entries[state->cursor];
		} while (state->cursor && !istore);

		if (!istore) {
			state_pop(&state);
			continue;
		}

		bh = ouichefs_bread_inode(&inode, sb, istore);
		if (!bh)
			break;

		if (S_ISDIR(inode->i_mode))
			state_push(sb, inode->index_block,
				   OUICHEFS_MAX_DIR_FILES, &state);

		revert(sb, istore, inode);
		reverted++;

		mark_buffer_dirty(bh);
		brelse(bh);
	}

	if (visited != reverted)
		pr_crit("error left the filesystem in an inconsistent state (%d visited, %d reverted)\n",
			visited, reverted);

	return err;
}
