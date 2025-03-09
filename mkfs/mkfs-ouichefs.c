#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <endian.h>
#include <string.h>

#define OUICHEFS_MAGIC 0x48434957

#define OUICHEFS_SB_BLOCK_NR 0
#define OUICHEFS_BLOCK_SIZE (1 << 12) /* 4 KiB */
#define OUICHEFS_MAX_FILESIZE (1 << 22) /* 4 MiB */
#define OUICHEFS_FILENAME_LEN 28
#define OUICHEFS_MAX_FILE_BLOCKS 1024
#define OUICHEFS_MAX_DIR_FILES 128
#define OUICHEFS_MAX_SNAPSHOTS 128

/* Block offset calculation helper */

#define OUICHEFS_SB_ISTORE_BLOCK_OFFSET(sb) (OUICHEFS_SB_BLOCK_NR + 1)

#define OUICHEFS_SB_IFREE_BLOCK_OFFSET(sb) \
	(OUICHEFS_SB_ISTORE_BLOCK_OFFSET(sb) + le32toh(sb->nr_istore_blocks))

#define OUICHEFS_SB_IREF_BLOCK_OFFSET(sb) \
	(OUICHEFS_SB_IFREE_BLOCK_OFFSET(sb) + le32toh(sb->nr_ifree_blocks))

#define OUICHEFS_SB_BFREE_BLOCK_OFFSET(sb) \
	(OUICHEFS_SB_IREF_BLOCK_OFFSET(sb) + le32toh(sb->nr_iref_blocks))

#define OUICHEFS_SB_DATA_BLOCK_OFFSET(sb) \
	(OUICHEFS_SB_BFREE_BLOCK_OFFSET(sb) + le32toh(sb->nr_bfree_blocks))

#define OUICHEFS_INODES_PER_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode))

#define OUICHEFS_BITS_PER_BLOCK (OUICHEFS_BLOCK_SIZE * 8)

#define OUICHEFS_IREF_PER_BLOCK (OUICHEFS_BLOCK_SIZE / sizeof(ouichefs_iref_t))

typedef uint8_t ouichefs_iref_t;

static char zero_block[OUICHEFS_BLOCK_SIZE];

struct ouichefs_inode {
	uint32_t i_no; /* Unique identifier */
	mode_t i_mode; /* File mode */
	uint32_t i_uid; /* Owner id */
	uint32_t i_gid; /* Group id */
	uint32_t i_size; /* Size in bytes */
	uint32_t i_ctime; /* Inode change time (sec)*/
	uint64_t i_nctime; /* Inode change time (nsec) */
	uint32_t i_atime; /* Access time (sec) */
	uint64_t i_natime; /* Access time (nsec) */
	uint32_t i_mtime; /* Modification time (sec) */
	uint64_t i_nmtime; /* Modification time (nsec) */
	uint32_t i_blocks; /* Block count (subdir count for directories) */
	uint32_t i_nlink; /* Hard links count */
	uint32_t i_refcnt; /* Reference count for snapshot count tracking */
	uint32_t index_block; /* Block with list of blocks for this file */
};

struct ouichefs_snapshot {
	uint32_t s_id; /* Unique id of the snapshot */
	uint32_t s_root; /* Index of the root inode */
	uint64_t s_time; /* Creation time */
};

struct ouichefs_superblock {
	uint32_t magic; /* Magic number */

	uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
	uint32_t nr_inodes; /* Total number of inodes */

	uint32_t nr_istore_blocks; /* Number of inode store blocks */
	uint32_t nr_ifree_blocks; /* Number of free inodes bitmask blocks */
	uint32_t nr_iref_blocks; /* Number of ino refcount blocks */
	uint32_t nr_bfree_blocks; /* Number of free blocks bitmask blocks */

	uint32_t nr_free_inodes; /* Number of free inodes */
	uint32_t nr_free_blocks; /* Number of free blocks */

	uint32_t nr_snapshots; /* Number of snapshots */

	char padding[2008]; /* Padding to match block size */

	struct ouichefs_snapshot
		snapshots[OUICHEFS_MAX_SNAPSHOTS]; /* List of snapshots */
};

struct ouichefs_file_block {
	uint32_t blocks[OUICHEFS_MAX_FILE_BLOCKS];
};

struct ouichefs_dir_block {
	struct ouichefs_file {
		uint32_t inode;
		char filename[OUICHEFS_FILENAME_LEN];
	} files[OUICHEFS_MAX_DIR_FILES];
};

static inline void usage(char *appname)
{
	fprintf(stderr,
		"Usage:\n"
		"%s disk\n",
		appname);
}

/* Returns ceil(a/b) */
static inline uint32_t idiv_ceil(uint32_t a, uint32_t b)
{
	uint32_t ret = a / b;
	if (a % b != 0)
		return ret + 1;
	return ret;
}

static struct ouichefs_superblock *write_superblock(int fd, struct stat *fstats)
{
	int ret;
	struct ouichefs_superblock *sb;
	uint32_t nr_inodes;
	uint32_t nr_blocks;
	uint32_t nr_ifree_blocks;
	uint32_t nr_iref_blocks;
	uint32_t nr_bfree_blocks;
	uint32_t nr_istore_blocks;
	uint32_t mod;

	sb = calloc(1, sizeof(struct ouichefs_superblock));
	if (!sb)
		return NULL;

	nr_blocks = fstats->st_size / OUICHEFS_BLOCK_SIZE;
	nr_inodes = nr_blocks;
	mod = nr_inodes % OUICHEFS_INODES_PER_BLOCK;
	if (mod != 0)
		nr_inodes += mod;
	nr_istore_blocks = idiv_ceil(nr_inodes, OUICHEFS_INODES_PER_BLOCK);
	nr_ifree_blocks = idiv_ceil(nr_inodes, OUICHEFS_BITS_PER_BLOCK);
	nr_iref_blocks = idiv_ceil(nr_inodes, OUICHEFS_IREF_PER_BLOCK);
	nr_bfree_blocks = idiv_ceil(nr_blocks, OUICHEFS_BITS_PER_BLOCK);

	sb->magic = htole32(OUICHEFS_MAGIC);
	sb->nr_blocks = htole32(nr_blocks);
	sb->nr_inodes = htole32(nr_inodes);
	sb->nr_istore_blocks = htole32(nr_istore_blocks);
	sb->nr_ifree_blocks = htole32(nr_ifree_blocks);
	sb->nr_iref_blocks = htole32(nr_iref_blocks);
	sb->nr_bfree_blocks = htole32(nr_bfree_blocks);
	sb->nr_free_inodes = htole32(nr_inodes - 1);
	sb->nr_free_blocks =
		htole32(nr_blocks - OUICHEFS_SB_DATA_BLOCK_OFFSET(sb) - 1);

	ret = write(fd, sb, sizeof(struct ouichefs_superblock));
	if (ret != sizeof(struct ouichefs_superblock)) {
		free(sb);
		return NULL;
	}

	printf("Superblock: (%ld)\n"
	       "\tmagic=%#x\n"
	       "\tnr_blocks=%u\n"
	       "\tnr_inodes=%u (blocks: istore=%u, ifree=%u, iref=%u, bfree=%u)\n"
	       "\tnr_free_inodes=%u\n"
	       "\tnr_free_blocks=%u\n",
	       sizeof(struct ouichefs_superblock), le32toh(sb->magic),
	       le32toh(sb->nr_blocks), le32toh(sb->nr_inodes),
	       le32toh(sb->nr_istore_blocks), le32toh(sb->nr_ifree_blocks),
	       le32toh(sb->nr_iref_blocks), le32toh(sb->nr_bfree_blocks),
	       le32toh(sb->nr_free_inodes), le32toh(sb->nr_free_blocks));

	return sb;
}

static int write_inode_store(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	struct ouichefs_inode *inode;
	char *block;

	/* Allocate a zeroed block for inode store */
	block = calloc(1, OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;

	/* Root inode (inode 1) */
	inode = (struct ouichefs_inode *)block + 1;
	inode->i_mode =
		htole32(S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
			S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH);
	inode->i_no = 1;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_size = htole32(OUICHEFS_BLOCK_SIZE);
	inode->i_ctime = inode->i_atime = inode->i_mtime = htole32(0);
	inode->i_nctime = inode->i_natime = inode->i_nmtime = htole64(0);
	inode->i_blocks = htole32(1);
	inode->i_nlink = htole32(2);
	inode->i_refcnt = 1;
	inode->index_block = htole32(OUICHEFS_SB_DATA_BLOCK_OFFSET(sb));

	ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* Set other inode store blocks to zero */
	for (i = 1; i < le32toh(sb->nr_istore_blocks); i++) {
		ret = write(fd, zero_block, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Inode store: wrote %d blocks\n"
	       "\tinode size = %ld B\n",
	       i, sizeof(struct ouichefs_inode));

end:
	free(block);
	return ret;
}

static int write_ifree_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	char *block;
	uint64_t *ifree;

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	ifree = (uint64_t *)block;

	/* Set all bits to 1 */
	memset(ifree, 0xff, OUICHEFS_BLOCK_SIZE);

	/* First ifree block, containing first used inode */
	ifree[0] = htole64(0xfffffffffffffffc);
	ret = write(fd, ifree, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* All ifree blocks except the one containing 2 first inodes */
	ifree[0] = 0xffffffffffffffff;
	for (i = 1; i < le32toh(sb->nr_ifree_blocks); i++) {
		ret = write(fd, ifree, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Ifree blocks: wrote %d blocks\n", i);

end:
	free(block);

	return ret;
}

static int write_iref_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	uint8_t *block;
	ouichefs_iref_t *iref;

	block = calloc(1, OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;

	iref = block;

	/* Reference count for ino 1 */
	iref[0] = 0xFF;
	iref[1] = 1;

	ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	for (i = 1; i < le32toh(sb->nr_iref_blocks); i++) {
		ret = write(fd, zero_block, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Iref blocks: wrote %d blocks\n", i);

end:
	free(block);

	return ret;
}

static int write_bfree_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	char *block;
	uint64_t *bfree, mask, line;
	uint32_t nr_used = OUICHEFS_SB_DATA_BLOCK_OFFSET(sb) + 1;

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	bfree = (uint64_t *)block;

	/*
	 * Mark metadata blocks as used
	 */
	memset(bfree, 0xff, OUICHEFS_BLOCK_SIZE);
	i = 0;
	while (nr_used) {
		line = 0xffffffffffffffff;
		for (mask = 0x1; mask != 0x0; mask <<= 1) {
			line &= ~mask;
			nr_used--;
			if (!nr_used)
				break;
		}
		bfree[i] = htole64(line);
		i++;
	}
	ret = write(fd, bfree, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* other blocks */
	memset(bfree, 0xff, OUICHEFS_BLOCK_SIZE);
	for (i = 1; i < le32toh(sb->nr_bfree_blocks); i++) {
		ret = write(fd, bfree, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Bfree blocks: wrote %d blocks\n", i);
end:
	free(block);

	return ret;
}

static int write_root_index_block(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;

	ret = write(fd, zero_block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}
	ret = 0;

	printf("Root index block: wrote 1 block\n");

end:
	return ret;
}

static int write_data_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	/* struct ouichefs_dir_block root_block; */
	/* struct ouichefs_file_block foo_block; */
	/* char *foo; */
	/* uint32_t first_block = le32toh(sb->nr_istore_blocks) + */
	/* 	le32toh(sb->nr_ifree_blocks) + le32toh(sb->nr_bfree_blocks) + 3; */

	/* foo = calloc(1, OUICHEFS_BLOCK_SIZE); */
	/* if (!foo) */
	/* 	return -1; */

	/* end: */
	/* 	free(foo); */

	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_SUCCESS, fd;
	long int min_size;
	struct stat stat_buf;
	struct ouichefs_superblock *sb = NULL;

	if (argc != 2 || argv[1][0] == '-') {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	/* Open disk image */
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		perror("open():");
		return EXIT_FAILURE;
	}

	/* Get image size */
	ret = fstat(fd, &stat_buf);
	if (ret != 0) {
		perror("fstat():");
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Check if image is large enough */
	min_size = 100 * OUICHEFS_BLOCK_SIZE;
	if (stat_buf.st_size < min_size) {
		fprintf(stderr,
			"File is not large enough (size=%ld, min size=%ld)\n",
			stat_buf.st_size, min_size);
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Write superblock (block 0) */
	sb = write_superblock(fd, &stat_buf);
	if (!sb) {
		perror("write_superblock():");
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Write inode store blocks (from block 1) */
	ret = write_inode_store(fd, sb);
	if (ret != 0) {
		perror("write_inode_store():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write inode free bitmap blocks */
	ret = write_ifree_blocks(fd, sb);
	if (ret != 0) {
		perror("write_ifree_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write ino refcount blocks */
	ret = write_iref_blocks(fd, sb);
	if (ret != 0) {
		perror("write_iref_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write block free bitmap blocks */
	ret = write_bfree_blocks(fd, sb);
	if (ret != 0) {
		perror("write_bfree_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write the root index block */
	ret = write_root_index_block(fd, sb);
	if (ret != 0) {
		perror("write_root_index_block()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write data blocks */
	ret = write_data_blocks(fd, sb);
	if (ret != 0) {
		perror("write_data_blocks():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

free_sb:
	free(sb);
fclose:
	close(fd);

	return ret;
}
