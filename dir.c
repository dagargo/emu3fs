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

int name_comparator(void * v, struct emu3_dentry * e3d) {
	struct dentry *dentry = v;
	int size = strnlen(dentry->d_name.name, LENGTH_FILENAME);
	return strncmp(e3d->name, dentry->d_name.name, size);
}

char * emu3_filename_length(const char * filename, int * size) {
	char * index = &filename[LENGTH_FILENAME - 1];

	for (*size = LENGTH_FILENAME; *size > 0; (*size)--) {
		if (*index != ' ') {
			return index;
		}
		index--;
	}
	return NULL;
}

static inline struct emu3_dentry * emu3_find_dentry_by_name(struct super_block *sb, struct dentry *dentry, struct buffer_head **b)
{
	return emu3_find_dentry(sb, b, (void *) dentry, name_comparator);
}

static int emu3_readdir(struct file *f, void *dirent, filldir_t filldir)
{
    int i, j;
    struct dentry *de = f->f_dentry;
   	struct emu3_sb_info *info = EMU3_SB(de->d_inode->i_sb);
   	struct buffer_head *b;
   	struct emu3_dentry * e3d;
   	struct emu3_inode * e3i;
   	
    if (de->d_inode->i_ino != ROOT_DIR_INODE_ID)
    	return -EBADF;

    if (f->f_pos > 0)
    	return 0;
    
    if (filldir(dirent, ".", 1, f->f_pos++, de->d_inode->i_ino, DT_DIR) < 0)
    	return 0;
    if (filldir(dirent, "..", 2, f->f_pos++, de->d_parent->d_inode->i_ino, DT_DIR) < 0)
    	return 0;

	info->used_inodes = 0;
	e3i = EMU3_I(de->d_inode);
	for (i = 0; i < e3i->blocks; i++) {
		b = sb_bread(de->d_inode->i_sb, info->start_root_dir_block + i);
		e3d = (struct emu3_dentry *)b->b_data;

		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (IS_EMU3_FILE(e3d)) {
				if (e3d->type != FTYPE_DEL) { //Mark as deleted files are not shown
					int size;
					emu3_filename_length(e3d->name, &size);
					if (filldir(dirent, e3d->name, size, f->f_pos++, EMU3_I_ID(e3d), DT_REG) < 0) {
						return 0;
					}
				}
				info->used_inodes++;
				info->next_available_cluster = e3d->start_cluster + e3d->clusters;
				info->last_used_inode = e3d->id;
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

	if (dentry->d_name.len > LENGTH_FILENAME) {
		return ERR_PTR(-ENAMETOOLONG);
	}
	
	mutex_lock(&info->lock);
	e3d = emu3_find_dentry_by_name(dir->i_sb, dentry, &b);

	if (e3d) {
		inode = emu3_get_inode(dir->i_sb, EMU3_I_ID(e3d));
		brelse(b);
		if (IS_ERR(inode)) {
			mutex_unlock(&info->lock);
			return ERR_CAST(inode);
		}
	}

	mutex_unlock(&info->lock);
	
	d_add(dentry, inode);
	//d_invalidate(dentry);

	return NULL;
}

static int emu3_create(struct inode *dir, struct dentry *dentry, int mode,
                                                struct nameidata *nd)
{
	int err;
	struct inode *inode;
	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	unsigned int ino;

	inode = new_inode(sb);
	if (!inode) {
		return -ENOSPC;
	}
	
	mutex_lock(&info->lock);
	
	err = emu3_add_entry(dir, dentry->d_name.name, dentry->d_name.len, &ino);

	if (err)  {
	    mutex_unlock(&info->lock);
	    return err;
	}
	
	inode_init_owner(inode, dir, mode);
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;
	inode->i_blocks = 0; //TODO: check;
	inode->i_op = &emu3_inode_operations_file;
	inode->i_fop = &emu3_file_operations_file;
	inode->i_mapping->a_ops = &emu3_aops;
	inode->i_ino = ino + 1;
	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	mutex_unlock(&info->lock);
	d_instantiate(dentry, inode);
	
	return 0;
}

int emu3_add_entry(struct inode *dir, const unsigned char *name, int namelen, unsigned int * id) {
	struct buffer_head *b;
   	struct emu3_dentry * e3d;
   	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
   	unsigned int start_cluster;

	if (!namelen) {
		return -ENOENT;
	}
	
	if (namelen > LENGTH_FILENAME) {
		return -ENAMETOOLONG;
	}
	
	e3d = emu3_find_empty_dentry(sb, &b, id, &start_cluster);

	if (!e3d) {
		return -ENOSPC;
	}
	
	//TODO: fix timestamps
	memcpy(e3d->name, name, namelen);
	memset(&e3d->name[namelen], ' ', LENGTH_FILENAME - namelen);
	e3d->unknown = 0;
	e3d->id = *id;
	e3d->start_cluster = cpu_to_le16(start_cluster);
	e3d->clusters = cpu_to_le16(1);
	e3d->blocks = cpu_to_le16(1);
	e3d->bytes = cpu_to_le16(0);
	e3d->type = FTYPE_STD;
	memset(e3d->props, 0, 5);
    dir->i_mtime = CURRENT_TIME_SEC;
    mark_buffer_dirty_inode(b, dir);
    brelse(b);

	info->used_inodes++;
	info->next_available_cluster = start_cluster;
	info->last_used_inode = *id;

	return 0;
}

struct emu3_dentry * emu3_find_empty_dentry(struct super_block *sb, 
											struct buffer_head **b,
											unsigned int * id,
											unsigned int * start_cluster)
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_dentry * e3d;
	int i, j, k;
	char ids[EMU3_MAX_REGULAR_FILE];
	
	for (i = 0; i < EMU3_MAX_REGULAR_FILE; i++) {
		ids[i] = 0;
	}

	*start_cluster = 1; //The first data cluster is always the 1st one

	for (i = 0; i < info->root_dir_blocks; i++) {
		*b = sb_bread(sb, info->start_root_dir_block + i);
	
		e3d = (struct emu3_dentry *)(*b)->b_data;
		
		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (IS_EMU3_FILE(e3d)) {
				if (e3d->type != FTYPE_DEL)  {
				//Here we can't use the EMU3_I_ID macro
					ids[e3d->id] = 1;
				}
				*start_cluster = e3d->start_cluster + e3d->clusters;
			}
			else {
				for (k = 0; k < EMU3_MAX_REGULAR_FILE; k++) {
					if (ids[k] == 0) {
						*id = k;
						break;
					}
				}
				if (k == EMU3_MAX_REGULAR_FILE || *start_cluster > info->clusters)	{
					brelse(*b);
					return NULL;
				}
				else {
					return e3d;
				}
			}
			e3d++;
		}
		brelse(*b);
	}
	
	return NULL;
}

static int emu3_unlink(struct inode *dir, struct dentry *dentry) {
	struct buffer_head *b;
   	struct emu3_dentry * e3d;
	struct inode *inode = dentry->d_inode;
    struct emu3_sb_info *info = EMU3_SB(inode->i_sb);

	if (dentry->d_name.len > LENGTH_FILENAME) {
		return -ENAMETOOLONG;
	}

	e3d = emu3_find_dentry_by_name(dir->i_sb, dentry, &b);

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
	.create	= emu3_create,
	.lookup	= emu3_lookup,
	.link   = NULL,
	.unlink	= emu3_unlink,
	.rename	= NULL
};
