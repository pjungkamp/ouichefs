/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#ifndef _OUICHEFS_H
#define _OUICHEFS_H

#include <linux/fs.h>

#define OUICHEFS_MAGIC 0x48434957

#define OUICHEFS_BLOCK_SIZE (1 << 12) /* 4 KiB */
#define OUICHEFS_MAX_FILESIZE (1 << 22) /* 4 MiB */
#define OUICHEFS_FILENAME_LEN 24 /* previously 28 */
#define OUICHEFS_MAX_DIR_FILES \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_file))
#define OUICHEFS_MAX_FILE_BLOCKS (OUICHEFS_BLOCK_SIZE / sizeof(uint32_t))
#define OUICHEFS_MAX_SNAPSHOTS \
	((OUICHEFS_BLOCK_SIZE / 2) / sizeof(struct ouichefs_snapshot))
#define OUICHEFS_SNAPSHOTS_OFFSET (1 << 11) /* 2 KiB */

/* block offset helper */

#define OUICHEFS_SB_BLOCK_NR 0

#define OUICHEFS_SBI_ISTORE_BLOCK_OFFSET(sbi) \
	((void)sbi, OUICHEFS_SB_BLOCK_NR + 1)

#define OUICHEFS_SBI_IFREE_BLOCK_OFFSET(sbi) \
	(OUICHEFS_SBI_ISTORE_BLOCK_OFFSET(sbi) + sbi->nr_istore_blocks)

#define OUICHEFS_SBI_IREF_BLOCK_OFFSET(sbi) \
	(OUICHEFS_SBI_IFREE_BLOCK_OFFSET(sbi) + sbi->nr_ifree_blocks)

#define OUICHEFS_SBI_BFREE_BLOCK_OFFSET(sbi) \
	(OUICHEFS_SBI_IREF_BLOCK_OFFSET(sbi) + sbi->nr_iref_blocks)

#define OUICHEFS_SBI_DATA_BLOCK_OFFSET(sbi) \
	(OUICHEFS_SBI_BFREE_BLOCK_OFFSET(sbi) + sbi->nr_bfree_blocks)

#define OUICHEFS_INODES_PER_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode))

#define OUICHEFS_BITS_PER_BLOCK (OUICHEFS_BLOCK_SIZE * 8)

#define OUICHEFS_IREF_PER_BLOCK (OUICHEFS_BLOCK_SIZE / sizeof(ouichefs_iref_t))

/* private date getters */

#define OUICHEFS_SB(sb) (sb->s_fs_info)

#define OUICHEFS_INODE(inode) \
	(container_of(inode, struct ouichefs_inode_info, vfs_inode))

typedef uint8_t ouichefs_iref_t;

/*
 * ouiche_fs partition layout
 *
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode store  |  sb->nr_istore_blocks blocks
 * +---------------+
 * | ifree bitmap  |  sb->nr_ifree_blocks blocks
 * +---------------+
 * | ino refcount  |  sb->nr_iref_blocks blocks
 * +---------------+
 * | bfree bitmap  |  sb->nr_bfree_blocks blocks
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 *
 */

struct ouichefs_inode {
	uint32_t i_no; /* Unique identifier */
	uint32_t i_mode; /* File mode */
	uint32_t i_uid; /* Owner id */
	uint32_t i_gid; /* Group id */
	uint32_t i_size; /* Size in bytes */
	uint32_t i_ctime; /* Inode change time (sec)*/
	uint64_t i_nctime; /* Inode change time (nsec) */
	uint32_t i_atime; /* Access time (sec) */
	uint64_t i_natime; /* Access time (nsec) */
	uint32_t i_mtime; /* Modification time (sec) */
	uint64_t i_nmtime; /* Modification time (nsec) */
	uint32_t i_blocks; /* Block count */
	uint32_t i_nlink; /* Hard links count */
	uint32_t i_refcnt; /* Number of references to this node */
	uint32_t index_block; /* Block with list of blocks for this file */
};

struct ouichefs_inode_info {
	uint32_t isrc; /* Source store index */
	uint32_t idest; /* Writeback store index */
	uint32_t index_block; /* Index block */
	uint32_t old_index_block; /* New index block after cow */
	struct inode vfs_inode;
};

struct ouichefs_snapshot {
	uint32_t s_id; /* Unique id of the snapshot */
	uint32_t s_root; /* Index of the root inode */
	uint64_t s_time; /* Creation time */
};

struct ouichefs_sb_info {
	uint32_t magic; /* Magic number */

	uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
	uint32_t nr_inodes; /* Total number of inodes */

	uint32_t nr_istore_blocks; /* Number of inode store blocks */
	uint32_t nr_ifree_blocks; /* Number of inode free bitmap blocks */
	uint32_t nr_iref_blocks; /* Number of inode free bitmap blocks */
	uint32_t nr_bfree_blocks; /* Number of block free bitmap blocks */

	uint32_t nr_free_inodes; /* Number of free inodes */
	uint32_t nr_free_blocks; /* Number of free blocks */

	uint32_t nr_snapshots; /* Number of snapshots */

	unsigned long *ifree_bitmap; /* In-memory free inodes bitmap */
	ouichefs_iref_t *iref; /* In-memory ino reference counts */
	unsigned long *bfree_bitmap; /* In-memory free blocks bitmap */
	struct ouichefs_snapshot *snapshots; /* In-memory snapshot list */
};

struct ouichefs_file_block {
	uint32_t blocks[OUICHEFS_MAX_FILE_BLOCKS];
};

struct ouichefs_dir_block {
	struct ouichefs_file {
		uint32_t ino;
		uint32_t istore;
		char filename[OUICHEFS_FILENAME_LEN];
	} files[OUICHEFS_MAX_DIR_FILES];
};

/* superblock functions */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent);
struct buffer_head *ouichefs_bread_inode(struct ouichefs_inode **inode,
					 struct super_block *sb,
					 uint32_t istore);
int ouichefs_get_inode(struct super_block *sb, uint32_t istore,
		       struct ouichefs_inode *inode);
int ouichefs_put_inode(struct super_block *sb, uint32_t istore,
		       struct ouichefs_inode *inode);
struct buffer_head *ouichefs_bread_file(struct ouichefs_file_block **fb,
					struct super_block *sb, uint32_t bno);
struct buffer_head *ouichefs_bread_dir(struct ouichefs_dir_block **db,
				       struct super_block *sb, uint32_t bno);
int ouichefs_put_index(struct super_block *sb, uint32_t index_block,
		       mode_t mode);

/* inode cache functions */
int ouichefs_init_inode_cache(void);
void ouichefs_destroy_inode_cache(void);

/* inode functions */
struct inode *ouichefs_iget(struct super_block *sb, uint32_t ino,
			    uint32_t istore, int force_update);
void ouichefs_ifill(struct inode *inode, struct ouichefs_inode *disk_inode);
int ouichefs_imut(struct super_block *sb, struct dentry *dentry);

/* file functions */
void ouichefs_truncate_file_blocks(struct super_block *sb, struct inode *inode,
				   struct ouichefs_file_block *fb);
int ouichefs_copy_file_blocks(struct super_block *sb, struct inode *inode,
			      struct ouichefs_file_block *fb);

/* walk functions */
typedef int (*ouichefs_walk_cb)(struct super_block *sb, uint32_t istore,
				struct ouichefs_inode *inode);
int ouichefs_walk(struct super_block *sb, uint32_t root_index_block,
		  ouichefs_walk_cb visit, ouichefs_walk_cb revert);

/* snapshot functions */
int ouichefs_snapshot_create(struct super_block *sb, uint32_t id);
int ouichefs_snapshot_restore(struct super_block *sb, uint32_t id);
int ouichefs_snapshot_destroy(struct super_block *sb, uint32_t id);

/* file functions */
extern const struct file_operations ouichefs_file_ops;
extern const struct file_operations ouichefs_dir_ops;
extern const struct address_space_operations ouichefs_aops;

/* Getters for superbock and inode */
#define OUICHEFS_SB(sb) (sb->s_fs_info)
#define OUICHEFS_INODE(inode) \
	(container_of(inode, struct ouichefs_inode_info, vfs_inode))

#endif /* _OUICHEFS_H */
