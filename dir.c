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

static int name_comparator(void * v, struct emu3_dentry * e3d) {
	unsigned char * name = (unsigned char *) v;
	unsigned char fullname[LENGTH_SHOWED_FILENAME];
	get_emu3_fulldentry(fullname, e3d);
	if(strcmp(name, fullname) == 0) {
		return 0;
	}
	return -1;
}

static inline struct emu3_dentry * emu3_find_dentry_by_name(struct super_block *sb, const unsigned char * name, struct buffer_head **b)
{
	return emu3_find_dentry(sb, b, (void *) name, name_comparator);
}

static int emu3_readdir(struct file *f, void *dirent, filldir_t filldir)
{
    int i, j;
    struct dentry *de = f->f_dentry;
   	struct emu3_sb_info *info = EMU3_SB(de->d_inode->i_sb);
   	struct buffer_head *b;
   	struct emu3_dentry * e3d;
   	struct emu3_inode * e3i;
   	
    //TODO: check error.
    if (de->d_inode->i_ino != ROOT_DIR_INODE_ID)
    	return -EBADF;

    if (f->f_pos > 0)
    	return 0; //Returning an error here (-EBADF) makes ls giving a WRONG DESCRIPTOR FILE.
    
    if (filldir(dirent, ".", 1, f->f_pos++, de->d_inode->i_ino, DT_DIR) < 0)
    	return 0;
    if (filldir(dirent, "..", 2, f->f_pos++, de->d_parent->d_inode->i_ino, DT_DIR) < 0)
    	return 0;

	//What if reading by id the 102 files? 00-99, 107 and 109

	info->used_inodes = 0;
	e3i = EMU3_I(de->d_inode);
	for (i = 0; i < e3i->blocks; i++) {
		b = sb_bread(de->d_inode->i_sb, info->start_root_dir_block + i);
		e3d = (struct emu3_dentry *)b->b_data;

		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (IS_EMU3_FILE(e3d)) {
				char fullname[LENGTH_SHOWED_FILENAME];
				if (e3d->type != FTYPE_DEL) { //Mark as deleted files are not shown
					get_emu3_fulldentry(fullname, e3d);
					if (filldir(dirent, fullname, LENGTH_SHOWED_FILENAME, f->f_pos++, EMU3_I_ID(e3d), DT_REG) < 0) {
						return 0;
					}
				}
				info->used_inodes++;
				//Should this be moved to a run on dirty sb function?
				info->next_available_cluster = e3d->start_cluster + e3d->clusters;
			}
			e3d++;
		}
	
		brelse(b);
	}
	
   return 0;
}

static struct dentry *emu3_lookup(struct inode *dir, struct dentry *dentry,
						struct nameidata *nd)
{
	struct inode *inode = NULL;
	struct buffer_head *b;
   	struct emu3_dentry * e3d;
   	struct emu3_sb_info * info = EMU3_SB(dir->i_sb);

	if (dentry->d_name.len > LENGTH_SHOWED_FILENAME) {
		return ERR_PTR(-ENAMETOOLONG);
	}
	
	mutex_lock(&info->lock);
	e3d = emu3_find_dentry_by_name(dir->i_sb, dentry->d_name.name, &b);

	if (e3d) {
		brelse(b);
		inode = emu3_get_inode(dir->i_sb, EMU3_I_ID(e3d));
		if (IS_ERR(inode)) {
			mutex_unlock(&info->lock);
			return ERR_CAST(inode);
		}
	}

	d_add(dentry, inode);
	mutex_unlock(&info->lock);

	return NULL;
}

static int emu3_unlink(struct inode *dir, struct dentry *dentry) {
	struct buffer_head *b;
   	struct emu3_dentry * e3d;
	struct inode *inode = dentry->d_inode;
    struct emu3_sb_info *info = EMU3_SB(inode->i_sb);

	if (dentry->d_name.len > LENGTH_SHOWED_FILENAME) {
		return -ENAMETOOLONG;
	}

	e3d = emu3_find_dentry_by_name(dir->i_sb, dentry->d_name.name, &b);

	if (e3d == NULL) {
		return -ENOENT;
	}
	
	mutex_lock(&info->lock);
	e3d->type = FTYPE_DEL;
	mark_buffer_dirty_inode(b, dir);
	dir->i_ctime = dir->i_mtime = CURRENT_TIME_SEC;
    mark_inode_dirty(dir);
    inode->i_ctime = dir->i_ctime;
    inode_dec_link_count(inode);
	brelse(b);
	mutex_unlock(&info->lock);

	return 0;
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
	.unlink	= emu3_unlink,
	.rename	= NULL
};
