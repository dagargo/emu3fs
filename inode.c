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

void emu3_init_once(void *foo)
{
	struct emu3_inode *e3i = foo;

	inode_init_once(&e3i->vfs_inode);
}

inline void emu3_set_i_map(struct emu3_sb_info *info,
			   struct inode *inode, unsigned int d_num)
{
	info->i_maps[inode->i_ino - EMU3_I_ID_OFFSET] = d_num;
}

inline unsigned int emu3_get_i_map(struct emu3_sb_info *info,
				   struct inode *inode)
{
	return info->i_maps[inode->i_ino - EMU3_I_ID_OFFSET];
}

inline void emu3_clear_i_map(struct emu3_sb_info *info, struct inode *inode)
{
	info->i_maps[inode->i_ino - EMU3_I_ID_OFFSET] = 0;
}

unsigned long emu3_get_or_add_i_map(struct emu3_sb_info *info,
				    unsigned int d_num)
{
	int i, pos;
	bool found;
	unsigned int *empty;
	unsigned int *v;

	empty = NULL;
	found = 0;
	v = info->i_maps;
	for (i = 0; i < EMU3_TOTAL_ENTRIES(info); i++, v++) {
		if ((*v) == d_num) {
			found = 1;
			break;
		}

		if (!(*v) && !empty) {
			empty = v;
			pos = i;
		}
	}

	if (!found)
		*empty = d_num;

	return (found ? i : pos) + EMU3_I_ID_OFFSET;
}

struct emu3_dentry *emu3_find_dentry_by_inode(struct inode *inode,
					      struct buffer_head **b)
{
	struct emu3_dentry *e3d;
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	unsigned long mapped = emu3_get_i_map(info, inode);
	unsigned long blknum = EMU3_I_ID_GET_BLKNUM(mapped);
	unsigned long offset = EMU3_I_ID_GET_OFFSET(mapped);

	*b = sb_bread(inode->i_sb, blknum);

	e3d = (struct emu3_dentry *)(*b)->b_data;
	e3d += offset;

	return e3d;
}

static unsigned int emu3_dir_block_count(struct emu3_dentry *e3d,
					 struct emu3_sb_info *info)
{
	unsigned int i = 0;
	short *block = e3d->dattrs.block_list;

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
	unsigned int start_cluster = cpu_to_le16(e3d->fattrs.start_cluster) - 1;
	unsigned int clusters = cpu_to_le16(e3d->fattrs.clusters) - 1;
	unsigned int blocks = cpu_to_le16(e3d->fattrs.blocks);

	if (blocks > sb->blocks_per_cluster) {
		printk(KERN_ERR "%s: wrong EOF in file with id 0x%016lx\n",
		       EMU3_MODULE_NAME, id);
		return -1;
	}
	*bsize = (clusters * sb->blocks_per_cluster) + blocks;
	*fsize = (((*bsize) - 1) * EMU3_BSIZE) + cpu_to_le16(e3d->fattrs.bytes);
	*start =
	    (start_cluster * sb->blocks_per_cluster) + sb->start_data_block;
	return 0;
}

void
emu3_get_file_geom(struct inode *inode,
		   unsigned short *clusters,
		   unsigned short *blocks, unsigned short *bytes)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int bytes_per_cluster = info->blocks_per_cluster * EMU3_BSIZE;
	unsigned int clusters_rem;
	unsigned int size = inode->i_size;

	if (clusters)
		*clusters = (size / bytes_per_cluster) + 1;
	clusters_rem = size % bytes_per_cluster;
	if (blocks)
		*blocks = (clusters_rem / EMU3_BSIZE) + 1;
	if (bytes)
		*bytes = clusters_rem % EMU3_BSIZE;
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
	struct emu3_inode *e3i;
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

			e3i = EMU3_I(inode);
			e3i->start_cluster =
			    le16_to_cpu(e3d->fattrs.start_cluster);
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
