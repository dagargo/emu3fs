/*  
 *	dir.c
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

static int emu3_readdir(struct file *f, void *dirent, filldir_t filldir)
{
    int i;
    int block_num;
    int entries_per_block;
    struct dentry *de = f->f_dentry;
   	struct emu3_sb_info *info = EMU3_SB(de->d_inode->i_sb);
   	struct buffer_head *b;
   	struct emu3_dentry * e3d;
   	struct emu3_inode * e3i;
   	
    //TODO: check error.
    if (de->d_inode->i_ino != ROOT_DIR_INODE_ID)
    	return -EBADF;

    if(f->f_pos > 0 )
    	return 0; //Returning an error here (-EBADF) makes ls giving a WRONG DESCRIPTOR FILE.
    
    if (filldir(dirent, ".", 1, f->f_pos++, de->d_inode->i_ino, DT_DIR) < 0)
    	return 0;
    if (filldir(dirent, "..", 2, f->f_pos++, de->d_parent->d_inode->i_ino, DT_DIR) < 0)
    	return 0;

	info->used_inodes = 0;
	block_num = info->start_root_dir_block;
	e3i = EMU3_I(de->d_inode);
	for (i = 0; i < e3i->blocks; i++) {
		b = sb_bread(de->d_inode->i_sb, block_num);
	
		e3d = (struct emu3_dentry *)b->b_data;
	
		entries_per_block = 0;
		while (entries_per_block < MAX_ENTRIES_PER_BLOCK && IS_EMU3_FILE(e3d)) {
			if (filldir(dirent, e3d->name, MAX_LENGTH_FILENAME, f->f_pos++, EMU3_I_ID(e3d), DT_REG) < 0) {
    			return 0;
    		}
			info->used_inodes++;
			info->next_available_cluster = e3d->start_cluster + e3d->clusters;
			e3d++;
			entries_per_block++;
		}
	
		brelse(b);

		if (entries_per_block < MAX_ENTRIES_PER_BLOCK) {
			break;
		}

		block_num++;
	}
	
    return 0;
}

static struct dentry *emu3_lookup(struct inode *dir, struct dentry *dentry,
						struct nameidata *nd)
{
	int i;
	int block_num;
	int entries_per_block;
	struct inode *inode;
	struct buffer_head *b;
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);
   	struct emu3_dentry * e3d;
   	struct emu3_inode * e3i;

	if (dentry->d_name.len > MAX_LENGTH_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	block_num = info->start_root_dir_block;
	e3i = EMU3_I(dir);
	for (i = 0; i < e3i->blocks; i++) {
		b = sb_bread(dir->i_sb, block_num);
	
		e3d = (struct emu3_dentry *)b->b_data;
	
		entries_per_block = 0;
		while (entries_per_block < MAX_ENTRIES_PER_BLOCK && IS_EMU3_FILE(e3d)) {
			if(strncmp(dentry->d_name.name, e3d->name, MAX_LENGTH_FILENAME) == 0) {
				inode = emu3_iget(dir->i_sb, EMU3_I_ID(e3d));
				d_add(dentry, inode);
				return NULL;
			}
			e3d++;
			entries_per_block++;
		}
	
		brelse(b);

		if (entries_per_block < MAX_ENTRIES_PER_BLOCK)
			break;

		block_num++;
	}

	return NULL;
}

static int emu3_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry) {
	return -EINVAL;
}

const struct file_operations emu3_file_operations_dir = {
	.read		= generic_read_dir,
	.readdir	= emu3_readdir,
	.fsync		= generic_file_fsync,
	.llseek		= generic_file_llseek,
};

const struct inode_operations emu3_inode_operations_dir = {
	.create	= NULL,
	.lookup	= emu3_lookup,
	.link   = NULL,
	.unlink	= NULL,
	.rename	= emu3_rename,
};
