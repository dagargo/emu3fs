/*
 *   emu3_fs.h
 *   Copyright (C) 2017 David García Goñi <dagargo at gmail dot com>
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
 *   along with emu3fs.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/vfs.h>

#define EMU3_MODULE_NAME "E-mu E3 fs"

#define EMU3_FS_SIGNATURE "EMU3"
#define EMU3_FS_TYPE 0x454d5533

#define EMU3_BSIZE 0x200
#define EMU3_CENTRIES_PER_BLOCK  (EMU3_BSIZE / 2)

//TODO: why can't this be used as a left assignment operator?
#define EMU3_SB(sb) ((struct emu3_sb_info *)sb->s_fs_info)

#define EMU3_I(inode) ((struct emu3_inode *)container_of(inode, struct emu3_inode, vfs_inode))

//This is needed because no inode id can be zero
#define EMU3_I_ID(e3d) ((e3d->id) + 1)
#define TO_EMU3_ID(id) (id==ROOT_DIR_INODE_ID?id:(id-1))

#define EMU3_MAX_REGULAR_FILE 100

#define LAST_CLUSTER_OF_FILE 0x7fff

//100 regular banks + 2 special rom files with fixed ids at 0x6b and 0x6d
#define EMU3_MAX_FILES 102

#define MAX_ENTRIES_PER_BLOCK 16

#define ROOT_DIR_INODE_ID 1024	//Any value is valid as long as is greater than the highest inode id.

#define LENGTH_FILENAME 16

#define FTYPE_DEL 0x00		//Deleted file
#define FTYPE_STD 0x81
#define FTYPE_UPD 0x83		//Used by the first file after a deleted file
#define FTYPE_SYS 0x80

#define IS_EMU3_FILE(e3d) 		\
	(((e3d)->clusters > 0) &&	\
	(							\
	(e3d)->type == FTYPE_STD || \
	(e3d)->type == FTYPE_UPD || \
	(e3d)->type == FTYPE_SYS)	\
	)

struct emu3_sb_info {
	unsigned int blocks;
	unsigned int start_info_block;
	unsigned int info_blocks;
	unsigned int start_root_dir_block;
	unsigned int root_dir_blocks;
	unsigned int start_cluster_list_block;
	unsigned int cluster_list_blocks;
	unsigned int start_data_block;
	unsigned int blocks_per_cluster;
	unsigned int clusters;
	unsigned int *id_list;
	unsigned short *cluster_list;
	struct mutex lock;
};

struct emu3_dentry {
	char name[LENGTH_FILENAME];
	unsigned char unknown;
	unsigned char id;	//This can be 0. No inode id in linux can be 0.
	unsigned short start_cluster;
	unsigned short clusters;
	unsigned short blocks;
	unsigned short bytes;
	unsigned char type;
	unsigned char props[5];
};

struct emu3_inode {
	unsigned int start_cluster;
	struct inode vfs_inode;
};

extern const struct file_operations emu3_file_operations_dir;

extern const struct inode_operations emu3_inode_operations_dir;

extern const struct file_operations emu3_file_operations_file;

extern const struct inode_operations emu3_inode_operations_file;

extern const struct address_space_operations emu3_aops;

struct inode *emu3_get_inode(struct super_block *, unsigned long);

inline void get_emu3_fulldentry(char *, struct emu3_dentry *);

struct emu3_dentry *emu3_find_dentry(struct super_block *,
				     struct buffer_head **,
				     void *,
				     int (*)(void *, struct emu3_dentry *));

int emu3_add_entry(struct inode *, const unsigned char *, int,
		   unsigned int *, int *);

struct emu3_dentry *emu3_find_empty_dentry(struct super_block *,
					   struct buffer_head **);

const char *emu3_filename_length(const char *, int *);

inline unsigned int emu3_get_start_block(struct emu3_inode *e3i,
					 struct emu3_sb_info *info);

void emu3_get_file_geom(struct inode *,
			unsigned short *, unsigned short *, unsigned short *);

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

struct inode *emu3_alloc_inode(struct super_block *);

void emu3_destroy_inode(struct inode *);

int emu3_write_inode(struct inode *, struct writeback_control *);

void emu3_evict_inode(struct inode *);

int emu3_get_free_id(struct emu3_sb_info *);
