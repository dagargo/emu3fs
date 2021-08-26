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

#define EMU3_BSIZE 0x200
#define EMU3_CLUSTER_ENTRIES_PER_BLOCK  (EMU3_BSIZE >> 1)

#define EMU3_ROOT_DIR_I_ID 1	//Any value is valid as long as is lower than the first inode ID.
#define EMU3_I_ID_OFFSET   8

#define EMU3_SB(sb) ((struct emu3_sb_info *)(sb)->s_fs_info)

#define EMU3_I(inode) ((struct emu3_inode *)container_of((inode), struct emu3_inode, vfs_inode))

#define EMU3_I_ID_OFFSET_SIZE 4
#define EMU3_I_ID_OFFSET_MASK ((1 << EMU3_I_ID_OFFSET_SIZE) - 1)
#define EMU3_I_ID(blknum, offset) (((blknum) << EMU3_I_ID_OFFSET_SIZE) | ((offset) & EMU3_I_ID_OFFSET_MASK))
#define EMU3_I_ID_GET_BLKNUM(id) ((id) >> EMU3_I_ID_OFFSET_SIZE)
#define EMU3_I_ID_GET_OFFSET(id) ((id) & EMU3_I_ID_OFFSET_MASK)

#define EMU3_MAX_REGULAR_FILE 100

#define EMU_LAST_FILE_CLUSTER ((short)0x7fff)

#define EMU3_BLOCKS_PER_DIR 7

#define EMU3_LENGTH_FILENAME 16

#define EMU3_ENTRIES_PER_BLOCK (EMU3_BSIZE / (sizeof(struct emu3_dentry)))

#define EMU3_TOTAL_ENTRIES(info) (((info)->root_blocks + (info)->dir_content_blocks) * EMU3_ENTRIES_PER_BLOCK)

//For devices, this should be 102, 100 regular banks + 2 special rom files with fixed ids at 0x6b and 0x6d.
//We use the maximum physically allowed.
#define EMU3_MAX_FILES_PER_DIR (EMU3_ENTRIES_PER_BLOCK * EMU3_BLOCKS_PER_DIR)

#define EMU3_FTYPE_DEL 0x00	//Deleted file
#define EMU3_FTYPE_STD 0x81
#define EMU3_FTYPE_UPD 0x83	//Used by the first file after a deleted file
#define EMU3_FTYPE_SYS 0x80

#define EMU3_DTYPE_1 0x40
#define EMU3_DTYPE_2 0x80

#define EMU3_IS_I_ROOT_DIR(inode) ((inode)->i_ino == EMU3_ROOT_DIR_I_ID)

#define EMU3_IS_I_REG_DIR(dir, info) (((emu3_get_i_map(info, dir)) >= EMU3_I_ID((info)->start_root_block, 0)) && \
 		                     ((emu3_get_i_map(info, dir)) <  EMU3_I_ID((info)->start_dir_content_block, 0)))

#define EMU3_DENTRY_IS_FILE(e3d) (((e3d)->id >= 0) &&                      \
                                  ((e3d)->id < EMU3_MAX_FILES_PER_DIR) &&  \
				  ((e3d)->fattrs.clusters > 0) &&          \
                                   (				           \
		          	   (e3d)->fattrs.type == EMU3_FTYPE_STD || \
	                           (e3d)->fattrs.type == EMU3_FTYPE_UPD || \
	                           (e3d)->fattrs.type == EMU3_FTYPE_SYS    \
			           )                                       \
	                         )

#define EMU3_DENTRY_IS_DIR(e3d) (((e3d)->id == EMU3_DTYPE_1 || (e3d)->id == EMU3_DTYPE_2) && \
                                 (le16_to_cpu((e3d)->dattrs.block_list[0]) > 0))

#define EMU3_DIR_BLOCK_OK(block, info) ((block) >= info->start_dir_content_block && (block) < info->start_data_block)

#define EMU3_COMMON_MODE (S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR)
#define EMU3_DIR_MODE_ (S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH)
#define EMU3_FILE_MODE_ (S_IFREG)
#define EMU3_ROOT_DIR_MODE (EMU3_COMMON_MODE | EMU3_DIR_MODE_| S_IWGRP | S_IWOTH)
#define EMU3_DIR_MODE (EMU3_COMMON_MODE | EMU3_DIR_MODE_)
#define EMU3_FILE_MODE (EMU3_COMMON_MODE | EMU3_FILE_MODE_)

#define EMU3_FREE_DIR_BLOCK (-1)
#define EMU3_IS_DIR_BLOCK_FREE(block) ((block) == EMU3_FREE_DIR_BLOCK)

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
	short *cluster_list;
	bool *dir_content_block_list;
	unsigned long *i_maps;
	struct mutex lock;
};

struct emu3_file_attrs {
	unsigned short start_cluster;
	unsigned short clusters;
	unsigned short blocks;
	unsigned short bytes;
	unsigned char type;
	unsigned char props[5];
};

struct emu3_dir_attrs {
	short block_list[EMU3_BLOCKS_PER_DIR];
};

struct emu3_dentry {
	char name[EMU3_LENGTH_FILENAME];
	unsigned char unknown;
	unsigned char id;	//This can be 0. No inode id in linux can be 0.
	union {
		struct emu3_file_attrs fattrs;
		struct emu3_dir_attrs dattrs;
	};
};

struct emu3_inode {
	struct inode vfs_inode;
	short start_cluster;
};

extern const struct file_operations emu3_file_operations_dir;

extern const struct inode_operations emu3_inode_operations_dir;

extern const struct file_operations emu3_file_operations_file;

extern const struct inode_operations emu3_inode_operations_file;

extern const struct address_space_operations emu3_aops;

struct inode *emu3_get_inode(struct super_block *, unsigned long);

inline void get_emu3_fulldentry(char *, struct emu3_dentry *);

struct emu3_dentry *emu3_find_empty_dentry(struct super_block *,
					   struct buffer_head **);

inline unsigned int emu3_get_start_block(struct emu3_inode *,
					 struct emu3_sb_info *);

void emu3_get_file_geom(struct inode *, unsigned short *, unsigned short *,
			unsigned short *);

void emu3_write_cluster_list(struct super_block *);

void emu3_read_cluster_list(struct super_block *);

int emu3_expand_cluster_list(struct inode *, sector_t);

int emu3_next_free_cluster(struct emu3_sb_info *);

void emu3_init_cluster_list(struct inode *);

void emu3_update_cluster_list(struct inode *);

int emu3_get_cluster(struct inode *, int);

void emu3_update_cluster_list(struct inode *);

void emu3_clear_cluster_list(struct inode *);

sector_t emu3_get_phys_block(struct inode *, sector_t);

void emu3_init_once(void *);

struct emu3_dentry *emu3_find_dentry_by_inode(struct inode *,
					      struct buffer_head **);

unsigned long emu3_get_or_add_i_map(struct emu3_sb_info *, unsigned long);

unsigned long emu3_get_i_map(struct emu3_sb_info *, struct inode *);

void emu3_clear_i_map(struct emu3_sb_info *, struct inode *);
