/*
 *   inode.c
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

#include <linux/module.h>
#include <linux/kernel.h>

#include "emu3_fs.h"

extern struct kmem_cache *emu3_inode_cachep;

inline void emu3_inode_set_data(struct inode *inode, struct emu3_dentry *e3d)
{
	struct emu3_inode *e3i = EMU3_I(inode);
	memcpy(&e3i->data, &e3d->data, sizeof(struct emu3_dentry_data));
}

inline void emu3_set_i_map(struct emu3_sb_info *info,
			   struct inode *inode, unsigned int dnum)
{
	info->i_maps[inode->i_ino - EMU3_I_ID_MAP_OFFSET] = dnum;
}

inline unsigned int emu3_get_i_map(struct emu3_sb_info *info,
				   struct inode *inode)
{
	return info->i_maps[inode->i_ino - EMU3_I_ID_MAP_OFFSET];
}

inline void emu3_clear_i_map(struct emu3_sb_info *info, struct inode *inode)
{
	info->i_maps[inode->i_ino - EMU3_I_ID_MAP_OFFSET] = 0;
}

unsigned long emu3_get_or_add_i_map(struct emu3_sb_info *info,
				    unsigned int dnum)
{
	int i, pos;
	bool found;
	unsigned int *empty;
	unsigned int *v;

	empty = NULL;
	found = 0;
	v = info->i_maps;
	for (i = 0; i < EMU3_TOTAL_ENTRIES(info); i++, v++) {
		if ((*v) == dnum) {
			found = 1;
			break;
		}

		if (!(*v) && !empty) {
			empty = v;
			pos = i;
		}
	}

	if (!found)
		*empty = dnum;

	return (found ? i : pos) + EMU3_I_ID_MAP_OFFSET;
}

struct emu3_dentry *emu3_find_dentry_by_inode(struct inode *inode,
					      struct buffer_head **b)
{
	struct emu3_dentry *e3d;
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	unsigned int dnum = emu3_get_i_map(info, inode);
	unsigned int blknum = EMU3_DNUM_BLKNUM(dnum);
	unsigned int offset = EMU3_DNUM_OFFSET(dnum);

	*b = sb_bread(inode->i_sb, blknum);

	e3d = (struct emu3_dentry *)(*b)->b_data;
	e3d += offset;

	return e3d;
}

static unsigned int emu3_dir_block_count(struct emu3_dentry *e3d,
					 struct emu3_sb_info *info)
{
	unsigned int i = 0;
	short *block = e3d->data.dattrs.block_list;

	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++, block++) {
		if (EMU3_IS_DIR_BLOCK_FREE(*block))
			return i;
	}

	return i;
}

static unsigned int emu3_file_block_count(unsigned long id,
					  struct emu3_sb_info *sb,
					  struct emu3_dentry *e3d, int *start,
					  int *bsize, int *fsize)
{
	unsigned int start_cluster =
	    cpu_to_le16(e3d->data.fattrs.start_cluster) - 1;
	unsigned int clusters = cpu_to_le16(e3d->data.fattrs.clusters) - 1;
	unsigned int blocks = cpu_to_le16(e3d->data.fattrs.blocks);

	if (blocks > sb->blocks_per_cluster) {
		printk(KERN_ERR "%s: Bad data in inode %ld\n", EMU3_MODULE_NAME,
		       id);
		return -1;
	}
	*bsize = (clusters * sb->blocks_per_cluster) + blocks;
	*fsize =
	    (((*bsize) - 1) * EMU3_BSIZE) + cpu_to_le16(e3d->data.fattrs.bytes);
	*start =
	    (start_cluster * sb->blocks_per_cluster) + sb->start_data_block;
	return 0;
}

struct inode *emu3_get_inode(struct super_block *sb, unsigned long ino)
{
	int file_block_size;
	int file_block_start;
	int file_size;
	umode_t mode;
	unsigned int links;
	struct inode *inode;
	struct emu3_dentry *e3d;
	struct buffer_head *b;
	const struct inode_operations *iops;
	const struct file_operations *fops;
	struct emu3_sb_info *info = EMU3_SB(sb);

	inode = iget_locked(sb, ino);

	if (IS_ERR(inode))
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW))
		return inode;

	if (EMU3_IS_I_ROOT_DIR(inode)) {
		file_block_size = info->root_blocks;
		file_size = info->root_blocks * EMU3_BSIZE;
		iops = &emu3_inode_operations_dir;
		fops = &emu3_file_operations_dir;
		links = 2;
		mode = EMU3_DIR_MODE;
	} else {
		e3d = emu3_find_dentry_by_inode(inode, &b);

		if (!e3d)
			return ERR_PTR(-EIO);

		if (EMU3_DENTRY_IS_FILE(e3d)) {
			emu3_file_block_count(ino, info, e3d, &file_block_start,
					      &file_block_size, &file_size);
			iops = &emu3_inode_operations_file;
			fops = &emu3_file_operations_file;
			links = 1;
			mode = EMU3_FILE_MODE;
			inode->i_mapping->a_ops = &emu3_aops;
		} else if (EMU3_DENTRY_IS_DIR(e3d)) {
			file_block_size = emu3_dir_block_count(e3d, info);
			file_size = file_block_size * EMU3_BSIZE;
			iops = &emu3_inode_operations_dir;
			fops = &emu3_file_operations_dir;
			links = 2;
			mode = EMU3_DIR_MODE;
		} else {
			printk(KERN_ERR
			       "%s: entry is neither a file nor a directory\n",
			       EMU3_MODULE_NAME);
			brelse(b);
			return ERR_PTR(-EIO);
		}
		emu3_inode_set_data(inode, e3d);
		brelse(b);
	}

	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	set_nlink(inode, links);
	inode->i_op = iops;
	inode->i_fop = fops;
	inode->i_blocks = file_block_size;
	inode->i_size = file_size;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);

	unlock_new_inode(inode);

	return inode;
}
