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
 *   along with emu3fs.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/kernel.h>

#include "emu3_fs.h"

#define EMU3_COMMON_MODE (S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR)
#define EMU3_DIR_MODE (S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH)
#define EMU3_FILE_MODE (S_IFREG)

extern struct kmem_cache *emu3_inode_cachep;

struct inode *emu3_alloc_inode(struct super_block *sb)
{
	struct emu3_inode *e3i;

	e3i = kmem_cache_alloc(emu3_inode_cachep, GFP_KERNEL);
	if (!e3i)
		return NULL;
	return &e3i->vfs_inode;
}

static void emu3_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(emu3_inode_cachep, EMU3_I(inode));
}

void emu3_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, emu3_i_callback);
}

void emu3_init_once(void *foo)
{
	struct emu3_inode *e3i = foo;

	inode_init_once(&e3i->vfs_inode);
}

static struct emu3_dentry *emu3_find_dentry_by_id(struct super_block *sb,
						  unsigned long id,
						  struct buffer_head **b)
{
	sector_t blknum = EMU3_I_ID_GET_BLKNUM(id);
	unsigned int offset = EMU3_I_ID_GET_OFFSET(id);
	struct emu3_dentry *e3d;

	*b = sb_bread(sb, blknum);

	e3d = (struct emu3_dentry *)(*b)->b_data;
	e3d += offset;

	if (EMU3_DENTRY_IS_FILE(e3d) || EMU3_DENTRY_IS_DIR(e3d))
		return e3d;

	brelse(*b);

	return NULL;
}

int emu3_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_dentry *e3d;
	struct emu3_inode *e3i;
	struct buffer_head *bh;
	int err = 0;

	if (inode->i_ino == EMU3_ROOT_DIR_I_ID)
		return 0;

	mutex_lock(&info->lock);

	e3d = emu3_find_dentry_by_id(inode->i_sb, inode->i_ino, &bh);
	if (!e3d) {
		mutex_unlock(&info->lock);
		return PTR_ERR(e3d);
	}

	emu3_update_cluster_list(inode);

	emu3_get_file_geom(inode, &e3d->fattrs.clusters, &e3d->fattrs.blocks,
			   &e3d->fattrs.bytes);

	e3i = EMU3_I(inode);
	e3d->fattrs.start_cluster = e3i->start_cluster;

	mark_buffer_dirty(bh);
	if (wbc->sync_mode == WB_SYNC_ALL) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh))
			err = -EIO;
	}
	brelse(bh);

	mutex_unlock(&info->lock);

	return err;
}

void emu3_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);
	invalidate_inode_buffers(inode);
	clear_inode(inode);
}

unsigned int emu3_dir_block_count(struct emu3_dentry *e3d)
{
	unsigned int i = 0;
	unsigned short *block = e3d->dattrs.block_list;

	while (*block && i < EMU3_BLOCKS_PER_DIR) {
		block++;
		i++;
	}

	return i;
}

unsigned int emu3_file_block_count(unsigned long id, struct emu3_sb_info *sb,
				   struct emu3_dentry *e3d,
				   int *start, int *bsize, int *fsize)
{
	unsigned int start_cluster = cpu_to_le16(e3d->fattrs.start_cluster) - 1;
	unsigned int clusters = cpu_to_le16(e3d->fattrs.clusters) - 1;
	unsigned int blocks = cpu_to_le16(e3d->fattrs.blocks);

	if (blocks > sb->blocks_per_cluster) {
		printk(KERN_ERR "%s: wrong EOF in file with id 0x%016lx",
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
	struct inode *inode;
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_dentry *e3d = NULL;
	struct emu3_inode *e3i;
	int file_block_size;
	int file_block_start;
	int file_size;
	struct buffer_head *b = NULL;
	const struct inode_operations *iops;
	const struct file_operations *fops;
	unsigned int links;
	umode_t mode;

	inode = iget_locked(sb, ino);

	if (IS_ERR(inode))
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW))
		return inode;

	if (ino == EMU3_ROOT_DIR_I_ID) {
		file_block_start = info->start_root_block;
		file_block_size = info->root_blocks;
		file_size = info->root_blocks * EMU3_BSIZE;
		iops = &emu3_inode_operations_dir;
		fops = &emu3_file_operations_dir;
		links = 2;
		mode = EMU3_DIR_MODE;
	} else {
		e3d = emu3_find_dentry_by_id(sb, ino, &b);

		if (!e3d)
			return ERR_PTR(-EIO);

		if (EMU3_IS_I_DIR(info, ino)) {
			//Directory
			file_block_size = emu3_dir_block_count(e3d);
			file_size = file_block_size * EMU3_BSIZE;
			iops = &emu3_inode_operations_dir;
			fops = &emu3_file_operations_dir;
			links = 2;
			mode = EMU3_DIR_MODE;
		} else {
			//File
			emu3_file_block_count(ino, info, e3d, &file_block_start,
					      &file_block_size, &file_size);
			iops = &emu3_inode_operations_file;
			fops = &emu3_file_operations_file;
			links = 1;
			mode = EMU3_FILE_MODE;

			e3i = EMU3_I(inode);
			e3i->start_cluster = e3d->fattrs.start_cluster;
			inode->i_mapping->a_ops = &emu3_aops;
		}
		brelse(b);
	}

	inode->i_mode = mode | EMU3_COMMON_MODE;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	set_nlink(inode, links);
	inode->i_op = iops;
	inode->i_fop = fops;
	inode->i_blocks = file_block_size;
	inode->i_size = file_size;
	inode->i_atime = current_time(inode);
	inode->i_mtime = current_time(inode);
	inode->i_ctime = current_time(inode);

	unlock_new_inode(inode);

	return inode;
}
