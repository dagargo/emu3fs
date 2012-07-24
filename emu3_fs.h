/*  
 *	emu3_fs.h
 *	Copyright (C) 2011 David García Goñi <dagargo at gmail dot com>
 *
 *   This file is part of EMU3 Filesystem Tools.
 *
 *   EMU3 Filesystem Tools is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   EMU3 Filesystem Tool is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with EMU3 Filesystem Tool.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/vfs.h>

#define EMU3_MODULE_NAME "E-mu E3 filesystem module"

#define EMU3_ERROR_MSG "E-mu E3 error: "

#define EMU3_FS_SIGNATURE "EMU3"
#define EMU3_FS_TYPE 0x454d5533

#define EMU3_BSIZE 0x200

//TODO: why can't this be used as a left assignment operator?
#define EMU3_SB(sb) ((struct emu3_sb_info *)sb->s_fs_info)

#define EMU3_I(inode) ((struct emu3_inode *)container_of(inode, struct emu3_inode, vfs_inode))

#define EMU3_I_ID(e3d) ((e3d->id) + 1)

#define EMU3_MAX_REGULAR_FILE 100

#define EMU3_MAX_FILES (EMU3_MAX_REGULAR_FILE + 2) //100 regular banks + 2 special rom files

#define MAX_ENTRIES_PER_BLOCK 16

#define ROOT_DIR_INODE_ID 1024 //Any value is valid as long as is greater than the highest inode id.

#define LENGTH_FILENAME 16

#define FILENAME_TEMPLATE "B%.2d %.16s"

#define LENGTH_SHOWED_FILENAME 21 // 20 + null char

#define FTYPE_DEL 0x00 //Deleted file
#define FTYPE_STD 0x81
#define FTYPE_UPD 0x83 //Used by the first file after a deleted file
#define FTYPE_SYS 0x80

#define IS_EMU3_FILE(e3d) (((e3d)->clusters > 0) && \
	((e3d)->type == FTYPE_DEL || (e3d)->type == FTYPE_STD || (e3d)->type == FTYPE_UPD || (e3d)->type == FTYPE_SYS))

struct emu3_sb_info {
	unsigned int blocks;
	unsigned int start_info_block;
	unsigned int info_blocks;
	unsigned int start_root_dir_block;
	unsigned int root_dir_blocks;
	unsigned int start_data_block;
	unsigned int blocks_per_cluster;
	unsigned int clusters;
	unsigned int used_inodes;
	unsigned int next_available_cluster;
	//TODO: inode map?
	struct mutex lock;
};

struct emu3_dentry {
	char name[16];
	unsigned char unknown;
	unsigned char id;
	unsigned short start_cluster;
	unsigned short clusters;
	unsigned short blocks;
	unsigned short bytes;
	unsigned char type;
	unsigned char props[5];
};

struct emu3_inode {
	unsigned long start_block;
	unsigned long blocks;
	struct inode vfs_inode;
};

extern const struct file_operations emu3_file_operations_dir;

extern const struct inode_operations emu3_inode_operations_dir;

extern const struct file_operations emu3_file_operations_file;

extern const struct inode_operations emu3_inode_operations_file;

extern const struct address_space_operations emu3_aops;

struct inode * emu3_get_inode(struct super_block *, unsigned long);

inline void get_emu3_fulldentry(char *, struct emu3_dentry *);

struct emu3_dentry * emu3_find_dentry(struct super_block *, 
											struct buffer_head **,
											void *,
											int (*)(void *, struct emu3_dentry *));
