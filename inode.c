/*
 *   inode.c
 *   Copyright (C) 2018 David García Goñi <dagargo at gmail dot com>
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
#include <linux/writeback.h>

#include "emu3_fs.h"

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

static int id_comparator(void *v, struct emu3_dentry *e3d)
{
	int id = *((int *)v);

	if (e3d->id == id)
		return 0;
	return -1;
}

struct emu3_dentry *emu3_find_dentry(struct super_block *sb,
				     struct buffer_head **bh,
				     void *v, int (*comparator) (void *,
								 struct
								 emu3_dentry *))
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_dentry *e3d;
	int i, j;

	if (!info)
		return NULL;

	for (i = 0; i < info->root_dir_blocks; i++) {
		*bh = sb_bread(sb, info->start_root_dir_block + i);

		e3d = (struct emu3_dentry *)(*bh)->b_data;

		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (IS_EMU3_FILE(e3d) && e3d->type != FTYPE_DEL)
				if (comparator(v, e3d) == 0)
					return e3d;
			e3d++;
		}

		brelse(*bh);
	}

	return NULL;
}

static inline struct emu3_dentry *emu3_find_dentry_by_id(struct super_block *sb,
							 unsigned long id,
							 struct buffer_head **b)
{
	return emu3_find_dentry(sb, b, &id, id_comparator);
}

int emu3_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	unsigned int ino = TO_EMU3_ID(inode->i_ino);
	struct emu3_dentry *e3d;
	struct emu3_inode *e3i;
	struct buffer_head *bh;
	int err = 0;

	if (ino == ROOT_DIR_INODE_ID)
		return 0;

	mutex_lock(&info->lock);

	e3d = emu3_find_dentry_by_id(inode->i_sb, ino, &bh);
	if (!e3d) {
		mutex_unlock(&info->lock);
		return PTR_ERR(e3d);
	}

	emu3_update_cluster_list(inode);

	emu3_get_file_geom(inode, &e3d->clusters, &e3d->blocks, &e3d->bytes);

	e3i = EMU3_I(inode);
	e3d->start_cluster = e3i->start_cluster;

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

unsigned int
emu3_file_block_count(struct emu3_sb_info *sb,
		      struct emu3_dentry *e3d,
		      int *start, int *bsize, int *fsize)
{
	unsigned int start_cluster = cpu_to_le16(e3d->start_cluster) - 1;
	unsigned int clusters = cpu_to_le16(e3d->clusters) - 1;
	unsigned int blocks = cpu_to_le16(e3d->blocks);

	if (blocks > sb->blocks_per_cluster) {
		printk(KERN_ERR "%s: wrong EOF in file with id %d",
		       EMU3_MODULE_NAME, EMU3_I_ID(e3d));
		return -1;
	}
	*bsize = (clusters * sb->blocks_per_cluster) + blocks;
	*fsize = (((*bsize) - 1) * EMU3_BSIZE) + cpu_to_le16(e3d->bytes);
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

struct inode *emu3_get_inode(struct super_block *sb, unsigned long id)
{
	struct inode *inode;
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_dentry *e3d = NULL;
	struct emu3_inode *e3i;
	int file_block_size;
	int file_block_start;
	int file_size;
	struct buffer_head *b = NULL;

	if (id == ROOT_DIR_INODE_ID) {
		file_block_start = info->start_root_dir_block;
		file_block_size = info->root_dir_blocks;
		file_size = info->root_dir_blocks * EMU3_BSIZE;
	} else {
		e3d = emu3_find_dentry_by_id(sb, TO_EMU3_ID(id), &b);

		if (!e3d)
			return ERR_PTR(-EIO);

		emu3_file_block_count(info, e3d, &file_block_start,
				      &file_block_size, &file_size);
	}

	inode = iget_locked(sb, id);

	if (IS_ERR(inode))
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_ino = id;	//TODO: needed?
	inode->i_mode =
	    ((id ==
	      ROOT_DIR_INODE_ID) ? S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH :
	     S_IFREG) | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP |
	    S_IWOTH;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_version = 1;
	set_nlink(inode, (id == ROOT_DIR_INODE_ID) ? 2 : 1);
	inode->i_op =
	    (id ==
	     ROOT_DIR_INODE_ID) ? &emu3_inode_operations_dir :
	    &emu3_inode_operations_file;
	inode->i_fop =
	    (id ==
	     ROOT_DIR_INODE_ID) ? &emu3_file_operations_dir :
	    &emu3_file_operations_file;
	inode->i_blocks = file_block_size;
	inode->i_size = file_size;
	inode->i_atime = current_time(inode);
	inode->i_mtime = current_time(inode);
	inode->i_ctime = current_time(inode);
	if (id != ROOT_DIR_INODE_ID) {
		e3i = EMU3_I(inode);
		e3i->start_cluster = e3d->start_cluster;
		inode->i_mapping->a_ops = &emu3_aops;
		brelse(b);
	}

	unlock_new_inode(inode);

	return inode;
}
