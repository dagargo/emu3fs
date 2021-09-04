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

inline void emu3_set_emu3_inode_data(struct inode *inode,
				     struct emu3_dentry *e3d)
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

static void emu3_set_inode_size_dir(struct inode *inode)
{
	struct emu3_inode *e3i = EMU3_I(inode);
	unsigned int i;
	short *block = e3i->data.dattrs.block_list;
	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++, block++)
		if (EMU3_IS_DIR_BLOCK_FREE(*block))
			break;
	inode->i_blocks = i;
	inode->i_size = inode->i_blocks * EMU3_BSIZE;
}

void emu3_set_inode_size_file(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	short clusters = cpu_to_le16(e3i->data.fattrs.clusters);
	short blocks = cpu_to_le16(e3i->data.fattrs.blocks);
	short bytes = cpu_to_le16(e3i->data.fattrs.bytes);

	if (blocks > info->blocks_per_cluster) {
		printk(KERN_CRIT "%s: Bad data in inode %ld\n",
		       EMU3_MODULE_NAME, inode->i_ino);
	}
	inode->i_blocks = clusters * info->blocks_per_cluster;

	if (clusters == 1 && blocks == 1 && bytes == 0)
		inode->i_size = 0;
	else {
		if (blocks > 1)
			clusters--;
		if (bytes)
			blocks--;
		inode->i_size =
		    (clusters * info->blocks_per_cluster +
		     blocks) * EMU3_BSIZE + bytes;
	}
}

struct inode *emu3_get_inode(struct super_block *sb, unsigned long ino)
{
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
		inode->i_blocks = info->root_blocks;
		inode->i_size = info->root_blocks * EMU3_BSIZE;
		iops = &emu3_inode_operations_dir;
		fops = &emu3_file_operations_dir;
		links = 2;
		mode = EMU3_ROOT_DIR_MODE;
	} else {
		e3d = emu3_find_dentry_by_inode(inode, &b);

		if (!e3d)
			return ERR_PTR(-EIO);

		emu3_set_emu3_inode_data(inode, e3d);
		brelse(b);

		if (EMU3_DENTRY_IS_FILE(e3d)) {
			emu3_set_inode_size_file(inode);
			iops = &emu3_inode_operations_file;
			fops = &emu3_file_operations_file;
			links = 1;
			mode = EMU3_FILE_MODE;
			inode->i_mapping->a_ops = &emu3_aops;
		} else if (EMU3_DENTRY_IS_DIR(e3d)) {
			emu3_set_inode_size_dir(inode);
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
	}

	if (mode & S_IFDIR)
		inode->i_opflags &= ~IOP_XATTR;
	if (mode & S_IFREG)
		inode->i_opflags |= IOP_XATTR;

	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	set_nlink(inode, links);
	inode->i_op = iops;
	inode->i_fop = fops;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);

	unlock_new_inode(inode);

	return inode;
}
