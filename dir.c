/*
 *   dir.c
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

#include "emu3_fs.h"

void emu3_filename_fix(char *in, char *out)
{
	int i;
	char c;

	for (i = 0; i < LENGTH_FILENAME; i++) {
		c = in[i];
		// 32 <= c <= 126
		if (c == '/')
			c = '?';	//Whatever will be nicer
		out[i] = c;
	}
}

const char *emu3_filename_length(const char *filename, int *size)
{
	const char *index = &filename[LENGTH_FILENAME - 1];

	for (*size = LENGTH_FILENAME; *size > 0; (*size)--) {
		if (*index != ' ')
			return index;
		index--;
	}
	return NULL;
}

int name_comparator(void *v, struct emu3_dentry *e3d)
{
	char fixed[LENGTH_FILENAME];
	struct dentry *dentry = v;
	int size;

	emu3_filename_fix(e3d->name, fixed);
	emu3_filename_length(fixed, &size);
	if (dentry->d_name.len != size)
		return -1;
	return strncmp(fixed, dentry->d_name.name, size);
}

static inline struct emu3_dentry *emu3_find_dentry_by_name(struct super_block
							   *sb, struct dentry
							   *dentry, struct buffer_head
							   **b)
{
	return emu3_find_dentry(sb, b, (void *)dentry, name_comparator);
}

static int emu3_iterate(struct file *f, struct dir_context *ctx)
{
	int i, j;
	loff_t k;
	struct inode *dir = file_inode(f);
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);
	struct buffer_head *b;
	struct emu3_dentry *e3d;

	if (dir->i_ino != ROOT_DIR_INODE_ID)
		return -EBADF;

	if (ctx->pos == 0) {
		ctx->pos++;
		if (!dir_emit(ctx, ".", 1, dir->i_ino, DT_DIR))
			return 0;
	}

	if (ctx->pos == 1) {
		ctx->pos++;
		if (!dir_emit
		    (ctx, "..", 2, f->f_path.dentry->d_inode->i_ino, DT_DIR))
			return 0;
	}

	k = 2;
	for (i = 0; i < dir->i_blocks; i++) {
		b = sb_bread(dir->i_sb, info->start_dir_content_block + i);
		e3d = (struct emu3_dentry *)b->b_data;
		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (IS_EMU3_FILE(e3d) && e3d->type != FTYPE_DEL) {	//Mark as deleted files are not shown
				if (ctx->pos == k) {
					char fixed[LENGTH_FILENAME];
					int size;
					emu3_filename_fix(e3d->name, fixed);
					emu3_filename_length(fixed, &size);
					ctx->pos++;
					if (!dir_emit
					    (ctx, fixed, size, EMU3_I_ID(e3d),
					     DT_REG)) {
						brelse(b);
						return 0;
					}
				}
				k++;
			}
			e3d++;
		}
		brelse(b);
	}

	return 0;
}

static struct dentry *emu3_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct inode *inode = NULL;
	struct buffer_head *b;
	struct emu3_dentry *e3d;

	if (dentry->d_name.len > LENGTH_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	e3d = emu3_find_dentry_by_name(dir->i_sb, dentry, &b);

	if (e3d) {
		inode = emu3_get_inode(dir->i_sb, EMU3_I_ID(e3d));
		brelse(b);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}

	d_add(dentry, inode);

	return NULL;
}

static int
emu3_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	unsigned int ino;
	unsigned int start_cluster;
	int err;

	inode = new_inode(sb);
	if (!inode)
		return -ENOSPC;

	mutex_lock(&info->lock);

	err =
	    emu3_add_entry(dir, dentry->d_name.name, dentry->d_name.len, &ino,
			   &start_cluster);

	if (err) {
		mutex_unlock(&info->lock);
		iput(inode);
		return err;
	}

	inode_init_owner(inode, dir, mode);
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	inode->i_blocks = 0;
	inode->i_op = &emu3_inode_operations_file;
	inode->i_fop = &emu3_file_operations_file;
	inode->i_mapping->a_ops = &emu3_aops;
	inode->i_ino = ino + 1;	//can NOT start at 0
	inode->i_size = 0;
	EMU3_I(inode)->start_cluster = start_cluster;
	emu3_init_cluster_list(inode);
	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	mutex_unlock(&info->lock);
	d_instantiate(dentry, inode);

	return 0;
}

int
emu3_add_entry(struct inode *dir, const unsigned char *name, int namelen,
	       unsigned int *ino, int *start_cluster)
{
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	int id;

	if (!namelen)
		return -ENOENT;

	if (namelen > LENGTH_FILENAME)
		return -ENAMETOOLONG;

	e3d = emu3_find_empty_dentry(sb, &b);

	if (!e3d)
		return -ENOSPC;

	*start_cluster = emu3_next_free_cluster(info);

	if (start_cluster < 0)
		return -ENOSPC;

	id = emu3_get_free_id(info);

	if (id < 0)
		return -ENOSPC;

	info->id_list[id] = 1;

	*ino = id;

	//TODO: fix timestamps
	memcpy(e3d->name, name, namelen);
	memset(&e3d->name[namelen], ' ', LENGTH_FILENAME - namelen);
	e3d->unknown = 0;
	e3d->id = id;
	e3d->start_cluster = le16_to_cpu(*start_cluster);
	e3d->clusters = le16_to_cpu(1);
	e3d->blocks = le16_to_cpu(1);
	e3d->bytes = le16_to_cpu(0);
	e3d->type = FTYPE_STD;
	memset(e3d->props, 0, 5);
	dir->i_mtime = current_time(dir);
	mark_buffer_dirty_inode(b, dir);
	brelse(b);

	return 0;
}

struct emu3_dentry *emu3_find_empty_dentry(struct super_block *sb,
					   struct buffer_head **b)
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_dentry *e3d;
	int i, j;

	for (i = 0; i < info->dir_content_blocks; i++) {
		*b = sb_bread(sb, info->start_dir_content_block + i);

		e3d = (struct emu3_dentry *)(*b)->b_data;

		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (!IS_EMU3_FILE(e3d))
				return e3d;
			e3d++;
		}
		brelse(*b);
	}

	return NULL;
}

static int emu3_unlink(struct inode *dir, struct dentry *dentry)
{
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct inode *inode = dentry->d_inode;
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);

	if (dentry->d_name.len > LENGTH_FILENAME)
		return -ENAMETOOLONG;

	e3d = emu3_find_dentry_by_name(dir->i_sb, dentry, &b);

	if (e3d == NULL)
		return -ENOENT;

	mutex_lock(&info->lock);
	e3d->type = FTYPE_DEL;
	mark_buffer_dirty_inode(b, dir);
	dir->i_ctime = dir->i_mtime = current_time(dir);
	mark_inode_dirty(dir);
	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	brelse(b);

	emu3_clear_cluster_list(inode);

	//Iff the file is a regular file then it is mapped
	if (e3d->id < EMU3_MAX_REGULAR_FILE)
		info->id_list[e3d->id] = 0;
	mutex_unlock(&info->lock);

	return 0;
}

static int
emu3_rename(struct inode *old_dir, struct dentry *old_dentry,
	    struct inode *new_dir, struct dentry *new_dentry,
	    unsigned int flags)
{
	struct super_block *sb = old_dentry->d_inode->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;
	struct emu3_dentry *e3d;
	int namelen = new_dentry->d_name.len;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	if (old_dir != new_dir)
		return -EINVAL;

	mutex_lock(&info->lock);

	e3d = emu3_find_dentry_by_name(sb, old_dentry, &bh);
	if (!e3d) {
		mutex_unlock(&info->lock);
		return -ENOENT;
	}

	memcpy(e3d->name, new_dentry->d_name.name, namelen);
	memset(&e3d->name[namelen], ' ', LENGTH_FILENAME - namelen);

	old_dir->i_ctime = old_dir->i_mtime = current_time(old_dir);
	mark_inode_dirty(old_dir);

	mark_buffer_dirty_inode(bh, old_dir);
	brelse(bh);
	mutex_unlock(&info->lock);
	return 0;
}

const struct file_operations emu3_file_operations_dir = {
	.read = generic_read_dir,
	.iterate = emu3_iterate,
	.fsync = generic_file_fsync,
	.llseek = generic_file_llseek,
};

const struct inode_operations emu3_inode_operations_dir = {
	.create = emu3_create,
	.lookup = emu3_lookup,
	.unlink = emu3_unlink,
	.rename = emu3_rename
};
