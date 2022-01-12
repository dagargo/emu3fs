/*
 *   emu3_fs.h
 *   Copyright (C) 2018 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of emu3fs.
 *
 *   emu3fs is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   emu3fs is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with emu3fs. If not, see <http://www.gnu.org/licenses/>.
*/

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/vfs.h>
#include <linux/writeback.h>

#define EMU3_MODULE_NAME "emu3fs"

#define EMU3_FS_SIGNATURE "EMU3"
#define EMU3_FS_TYPE 0x454d5533

#define EMU3_BSIZE_BITS 9
#define EMU3_BSIZE (1 << EMU3_BSIZE_BITS)
#define EMU3_CLUSTER_ENTRIES_PER_BLOCK  (EMU3_BSIZE >> 1)

#define EMU3_I_ID_ROOT_DIR 1	//Any value is valid as long as is lower than the first inode ID.
#define EMU3_I_ID_MAP_OFFSET (EMU3_I_ID_ROOT_DIR + 1)	//As inodes are mapped to emu3 dentries in an array, we need to add an offset greater than EMU3_ROOT_DIR_I_ID.

#define EMU3_SB(sb) ((struct emu3_sb_info *)(sb)->s_fs_info)

#define EMU3_I(inode) ((struct emu3_inode *)container_of((inode), struct emu3_inode, vfs_inode))
#define EMU3_I_START_CLUSTER(inode) (le16_to_cpu(EMU3_I(inode)->data.fattrs.start_cluster))

#define EMU3_DNUM_OFFSET_SIZE 4
#define EMU3_DNUM_OFFSET_MASK ((1 << EMU3_DNUM_OFFSET_SIZE) - 1)
#define EMU3_DNUM(blknum, offset) ((unsigned int)((blknum) << EMU3_DNUM_OFFSET_SIZE) | ((offset) & EMU3_DNUM_OFFSET_MASK))
#define EMU3_DNUM_BLKNUM(dnum) ((dnum) >> EMU3_DNUM_OFFSET_SIZE)
#define EMU3_DNUM_OFFSET(dnum) ((dnum) & EMU3_DNUM_OFFSET_MASK)

#define EMU_LAST_FILE_CLUSTER ((short)0x7fff)

#define EMU3_BLOCKS_PER_DIR 7

#define EMU3_LENGTH_FILENAME 16

#define EMU3_ENTRIES_PER_BLOCK (EMU3_BSIZE / (sizeof(struct emu3_dentry)))

#define EMU3_TOTAL_ENTRIES(info) (((info)->root_blocks + (info)->dir_content_blocks) * EMU3_ENTRIES_PER_BLOCK)

//For devices, this should be 102, 100 regular banks + 2 special rom files with fixed ids at 0x6b and 0x6d.
//We use the maximum physically allowed.
#define EMU3_MAX_FILES_PER_DIR (EMU3_ENTRIES_PER_BLOCK * EMU3_BLOCKS_PER_DIR)
#define EMU3_MAX_REGULAR_FILE 100	//Not used

#define EMU3_FTYPE_DEL 0x00	//Deleted file
#define EMU3_FTYPE_STD 0x81
#define EMU3_FTYPE_UPD 0x83	//Used by the first file after a deleted file
#define EMU3_FTYPE_SYS 0x80

#define EMU3_DTYPE_1 0x40
#define EMU3_DTYPE_2 0x80

#define EMU3_IS_I_ROOT_DIR(inode) ((inode)->i_ino == EMU3_I_ID_ROOT_DIR)

#define EMU3_IS_I_REG_DIR(dir, info) (((emu3_get_i_map(info, dir)) >= EMU3_DNUM((info)->start_root_block, 0)) && \
 		                     ((emu3_get_i_map(info, dir)) <  EMU3_DNUM((info)->start_dir_content_block, 0)))

#define EMU3_DENTRY_IS_FILE(e3d) (((e3d)->data.id >= 0) &&                      \
                                  ((e3d)->data.id < EMU3_MAX_FILES_PER_DIR) &&  \
				  ((e3d)->data.fattrs.clusters > 0) &&          \
                                   (				           \
		          	   (e3d)->data.fattrs.type == EMU3_FTYPE_STD || \
	                           (e3d)->data.fattrs.type == EMU3_FTYPE_UPD || \
	                           (e3d)->data.fattrs.type == EMU3_FTYPE_SYS    \
			           )                                       \
	                         )

#define EMU3_DENTRY_IS_DIR(e3d) (((e3d)->data.id == EMU3_DTYPE_1 || (e3d)->data.id == EMU3_DTYPE_2) && \
                                 (le16_to_cpu((e3d)->data.dattrs.block_list[0]) > 0))

#define EMU3_DIR_BLOCK_OK(block, info) ((block) >= info->start_dir_content_block && (block) < info->start_data_block)

#define EMU3_COMMON_MODE (S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR)
#define EMU3_DIR_MODE_ (S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH)
#define EMU3_FILE_MODE_ (S_IFREG)
#define EMU3_ROOT_DIR_MODE (EMU3_COMMON_MODE | EMU3_DIR_MODE_| S_IWGRP | S_IWOTH)
#define EMU3_DIR_MODE (EMU3_COMMON_MODE | EMU3_DIR_MODE_)
#define EMU3_FILE_MODE (EMU3_COMMON_MODE | EMU3_FILE_MODE_)

#define EMU3_FREE_DIR_BLOCK (-1)
#define EMU3_IS_DIR_BLOCK_FREE(block) ((block) == EMU3_FREE_DIR_BLOCK)

#define EMU3_FILE_PROPS_LEN 5

#define EMU3_ERR_NOT_BLK "%s: block %d not available\n"

struct emu3_sb_info {
	unsigned int blocks;
	unsigned int start_root_block;
	unsigned int root_blocks;
	unsigned int start_dir_content_block;
	unsigned int dir_content_blocks;
	unsigned int start_cluster_list_block;
	unsigned int cluster_list_blocks;
	unsigned int start_data_block;
	unsigned int blocks_per_cluster;
	unsigned int clusters;
	unsigned char cluster_size_shift; //Cluster size always a power of 2
	short *cluster_list;
	bool *dir_content_block_list;
	unsigned int *i_maps;
	struct mutex lock;
};

struct emu3_file_attrs {
	unsigned short start_cluster;
	unsigned short clusters;
	unsigned short blocks;
	unsigned short bytes;
	unsigned char type;
	unsigned char props[EMU3_FILE_PROPS_LEN];
};

struct emu3_dir_attrs {
	short block_list[EMU3_BLOCKS_PER_DIR];
};

struct emu3_dentry_data {
	unsigned char unknown;
	unsigned char id;	//This can be 0. No inode id in linux can be 0.
	union {
		struct emu3_file_attrs fattrs;
		struct emu3_dir_attrs dattrs;
	};
};

struct emu3_dentry {
	char name[EMU3_LENGTH_FILENAME];
	struct emu3_dentry_data data;
};

struct emu3_inode {
	struct inode vfs_inode;
	struct emu3_dentry_data data;
};

extern const struct file_operations emu3_file_operations_dir;

extern const struct inode_operations emu3_inode_operations_dir;

extern const struct file_operations emu3_file_operations_file;

extern const struct inode_operations emu3_inode_operations_file;

extern const struct address_space_operations emu3_aops;

extern const struct xattr_handler *emu3_xattr_handlers[];

struct inode *emu3_get_inode(struct super_block *, unsigned long);

int emu3_next_free_cluster(struct emu3_sb_info *);

void emu3_init_cluster_list(struct inode *);

int emu3_get_cluster(struct inode *, int);

sector_t emu3_get_phys_block(struct inode *, sector_t);

struct emu3_dentry *emu3_find_dentry_by_inode(struct inode *,
					      struct buffer_head **);

unsigned long emu3_get_or_add_i_map(struct emu3_sb_info *, unsigned int);

unsigned int emu3_get_i_map(struct emu3_sb_info *, struct inode *);

void emu3_clear_i_map(struct emu3_sb_info *, struct inode *);

void emu3_set_i_map(struct emu3_sb_info *, struct inode *, unsigned int);

void emu3_set_emu3_inode_data(struct inode *, struct emu3_dentry *);

ssize_t emu3_listxattr(struct dentry *, char *, size_t);

void emu3_free_dir_content_block(struct emu3_sb_info *, int);

int emu3_get_free_dir_content_block(struct emu3_sb_info *);

void emu3_set_fattrs(struct emu3_sb_info *, struct emu3_file_attrs *, loff_t);

void emu3_init_fattrs(struct emu3_sb_info *, struct emu3_file_attrs *, short);

void emu3_set_inode_blocks(struct inode *, struct emu3_file_attrs *);

void emu3_prune_cluster_list(struct inode *);
