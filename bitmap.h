/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#ifndef _OUICHEFS_BITMAP_H
#define _OUICHEFS_BITMAP_H

#include <linux/bitmap.h>
#include "ouichefs.h"

/*
 * Return the first free bit (set to 1) in a given in-memory bitmap spanning
 * over multiple blocks and clear it.
 * Return 0 if no free bit found (we assume that the first bit is never free
 * because of the superblock and the root inode, thus allowing us to use 0 as an
 * error value).
 */
static inline uint32_t get_first_free_bit(unsigned long *freemap,
					  unsigned long size)
{
	uint32_t idx;

	idx = find_first_bit(freemap, size);
	if (idx == size)
		return 0;

	bitmap_clear(freemap, idx, 1);

	return idx;
}

/*
 * Return an unused inode number and mark it used.
 * Return 0 if no free inode was found.
 */
static inline uint32_t get_free_istore(struct ouichefs_sb_info *sbi)
{
	uint32_t istore;

	if (!sbi->nr_free_inodes)
		return 0;

	istore = get_first_free_bit(sbi->ifree_bitmap, sbi->nr_inodes);
	if (!istore)
		return 0;

	sbi->nr_free_inodes--;
	pr_debug("allocated inode %u\n", istore);
	return istore;
}

/*
 * Return an unused block number and mark it used.
 * Return 0 if no free block was found.
 */
static inline uint32_t get_free_block(struct ouichefs_sb_info *sbi)
{
	uint32_t block;

	if (!sbi->nr_free_blocks)
		return 0;

	block = get_first_free_bit(sbi->bfree_bitmap, sbi->nr_blocks);
	if (!block)
		return 0;

	sbi->nr_free_blocks--;
	pr_debug("allocated block %u\n", block);

	return block;
}

/*
 * Mark the i-th bit in freemap as free (i.e. 1)
 */
static inline int put_free_bit(unsigned long *freemap, unsigned long size,
			       uint32_t i)
{
	/* i is greater than freemap size */
	if (i > size)
		return -1;

	bitmap_set(freemap, i, 1);

	return 0;
}

/*
 * Mark an inode as unused.
 */
static inline void put_istore(struct ouichefs_sb_info *sbi, uint32_t istore)
{
	if (put_free_bit(sbi->ifree_bitmap, sbi->nr_inodes, istore))
		return;

	sbi->nr_free_inodes++;
	pr_debug("freed istore %u\n", istore);
}

/*
 * Mark a block as unused.
 */
static inline void put_block(struct ouichefs_sb_info *sbi, uint32_t bno)
{
	if (put_free_bit(sbi->bfree_bitmap, sbi->nr_blocks, bno))
		return;

	sbi->nr_free_blocks++;
	pr_debug("freed block %u\n", bno);
}

/*
 * Find free ino in iref table.
 */
static inline uint32_t get_free_ino(struct ouichefs_sb_info *sbi)
{
	for (uint32_t ino = 1; ino < sbi->nr_inodes; ino++) {
		if (sbi->iref[ino] == 0) {
			sbi->iref[ino]++;
			pr_debug("allocated ino %u\n", ino);
			return ino;
		}
	}

	return 0;
}

/*
 * Increase ino use count.
 */
static inline void get_ino(struct ouichefs_sb_info *sbi, uint32_t ino)
{
	sbi->iref[ino]++;
}

/*
 * Decrease ino use count.
 */
static inline void put_ino(struct ouichefs_sb_info *sbi, uint32_t ino)
{
	if (--sbi->iref[ino])
		return;

	pr_debug("freed ino %u\n", ino);
}

#endif /* _OUICHEFS_BITMAP_H */
