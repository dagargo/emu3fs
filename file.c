/*
 *   file.c
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
#include <linux/mpage.h>
#include "emu3_fs.h"

//Base 0 search
static int emu3_expand_cluster_list(struct inode *inode, sector_t block)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int cluster = ((int)block) / info->blocks_per_cluster;
	short next = EMU3_I_START_CLUSTER(inode);
	int new, i = 0;

	while (le16_to_cpu(info->cluster_list[next]) != EMU_LAST_FILE_CLUSTER) {
		next = le16_to_cpu(info->cluster_list[next]);
		i++;
	}
	while (i < cluster) {
		new = emu3_next_free_cluster(info);
		if (new < 0)
			return -ENOSPC;
		info->cluster_list[next] = cpu_to_le16(new);
		next = new;
		i++;
	}
	info->cluster_list[next] = cpu_to_le16(EMU_LAST_FILE_CLUSTER);
	return 0;
}

static int
emu3_get_block(struct inode *inode, sector_t block,
	       struct buffer_head *bh_result, int create)
{
	sector_t phys;
	struct super_block *sb = inode->i_sb;
	struct emu3_inode *e3i = EMU3_I(inode);
	struct emu3_sb_info *info = EMU3_SB(sb);
	int err;

	phys = emu3_get_phys_block(inode, block);
	if (phys != -1) {
		map_bh(bh_result, sb, phys);
		return 0;
	}

	if (!create)
		return 0;

	mutex_lock(&info->lock);
	err = emu3_expand_cluster_list(inode, block);
	mutex_unlock(&info->lock);

	if (err)
		return err;
	else {
		phys = emu3_get_phys_block(inode, block);
		map_bh(bh_result, sb, phys);
		inode->i_blocks += info->blocks_per_cluster;
		e3i->data.fattrs.clusters++;
	}

	return 0;
}

static int emu3_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, emu3_get_block);
}

static int emu3_writepages(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, emu3_get_block);
}

static int
emu3_write_begin(struct file *file, struct address_space *mapping,
		 loff_t pos, unsigned len, struct page **pagep, void **fsdata)
{
	return block_write_begin(mapping, pos, len, pagep, emu3_get_block);
}

static sector_t emu3_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, emu3_get_block);
}

static int emu3_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	blkcnt_t blocks;
	int err;

	err = setattr_prepare(&nop_mnt_idmap, dentry, attr);
	if (err)
		return err;

	if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size != i_size_read(inode)) {
		err = inode_newsize_ok(inode, attr->ia_size);
		if (err)
			return err;

		truncate_setsize(inode, attr->ia_size);
		mutex_lock(&info->lock);
		emu3_set_fattrs(info, &e3i->data.fattrs, attr->ia_size);
		emu3_prune_cluster_list(inode);
		blocks = e3i->data.fattrs.clusters * info->blocks_per_cluster;
		mutex_unlock(&info->lock);

		inode->i_blocks = blocks;
	}
	setattr_copy(&nop_mnt_idmap, inode, attr);
	mark_inode_dirty(inode);

	return 0;
}

const struct address_space_operations emu3_aops = {
	.read_folio = emu3_read_folio,
	.writepages = emu3_writepages,
	.write_begin = emu3_write_begin,
	.write_end = generic_write_end,
	.bmap = emu3_bmap,
};

const struct file_operations emu3_file_operations_file = {
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
	.mmap = generic_file_mmap,
	.splice_read = filemap_splice_read,
	.fsync = generic_file_fsync
};

const struct inode_operations emu3_inode_operations_file = {
	.listxattr = emu3_listxattr,
	.setattr = emu3_setattr,
};
