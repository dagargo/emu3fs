/*  
 *	file.c
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

#include "emu3_fs.h"

const struct file_operations emu3_file_operations_file = {
	.llseek 	 = generic_file_llseek,
	.read		 = do_sync_read,
	.aio_read	 = generic_file_aio_read,
	.write		 = do_sync_write,
	.aio_write	 = generic_file_aio_write,
	.mmap		 = generic_file_mmap,
	.splice_read = generic_file_splice_read,
};

inline unsigned int emu3_get_start_block(struct emu3_inode * e3i, struct emu3_sb_info *info) {
	return ((e3i->start_cluster - 1) * info->blocks_per_cluster) + info->start_data_block;
}

static int emu3_get_block(struct inode *inode, sector_t block,
			struct buffer_head *bh_result, int create)
{
	unsigned long phys;
	struct super_block *sb = inode->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_inode * e3i = EMU3_I(inode);
	unsigned int ino = TO_EMU3_ID(inode->i_ino);
	unsigned short clusters;
	
	phys = emu3_get_start_block(e3i, info) + block;
	if (!create) {
		if (block < inode->i_blocks) {
			map_bh(bh_result, sb, phys);
		}
		return 0;
	}

	if (block < inode->i_blocks) {
		map_bh(bh_result, sb, phys);
		return 0;
	}

	//block >= e3i->total_blocks

	if (phys >= info->blocks) {
		return -ENOSPC;
	}
	
	if (ino != info->last_inode) {
		return -ENOSPC;
	}
	
	emu3_get_file_geom(info, inode, &clusters, NULL, NULL);
	
	if (e3i->start_cluster + clusters > info->clusters + 1) {
		//End of device reached
		return -ENOSPC;
	}

	mutex_lock(&info->lock);
	info->next_available_cluster = e3i->start_cluster + clusters;
	mutex_unlock(&info->lock);

	map_bh(bh_result, sb, phys);
	mark_inode_dirty(inode);
	
	return 0;
}

static int emu3_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, emu3_get_block);
}

static int emu3_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, emu3_get_block, wbc);
}

static int emu3_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	return block_write_begin(mapping, pos, len, flags, pagep,
				emu3_get_block);
}

static sector_t emu3_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, emu3_get_block);
}

const struct address_space_operations emu3_aops = {
	.readpage	 = emu3_readpage,
	.writepage	 = emu3_writepage,
	.write_begin = emu3_write_begin,
	.write_end	 = generic_write_end,
	.bmap		 = emu3_bmap,
};

const struct inode_operations emu3_inode_operations_file;
