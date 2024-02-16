/*
 *   xattr.c
 *   Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
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

#include <linux/xattr.h>
#include "emu3_fs.h"

#define EMU3_XATTR_BNUM "bank.number"
#define EMU3_XATTR_BNUM_LEN_MAX 8

ssize_t emu3_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	return snprintf(buffer, size, "%s%s", XATTR_USER_PREFIX,
			EMU3_XATTR_BNUM) + 1;
}

static int emu3_xattr_get(const struct xattr_handler *handler,
			  struct dentry *dentry, struct inode *inode,
			  const char *name, void *buffer, size_t size)
{
	int ret;
	struct emu3_inode *e3i;
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);

	if (strcmp(name, EMU3_XATTR_BNUM))
		return -ENODATA;

	mutex_lock(&info->lock);
	e3i = EMU3_I(inode);
	ret = snprintf(buffer, size, "%d", e3i->data.id);
	mutex_unlock(&info->lock);

	return ret;
}

static int emu3_xattr_set(const struct xattr_handler *handler,
			  struct mnt_idmap *idmap, struct dentry *dentry,
			  struct inode *inode, const char *name,
			  const void *buffer, size_t size, int flags)
{
	long bn;
	int ret;
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct emu3_inode *e3i;
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	char value[EMU3_XATTR_BNUM_LEN_MAX];

	if (strcmp(name, EMU3_XATTR_BNUM))
		return -ENODATA;

	if (size >= EMU3_XATTR_BNUM_LEN_MAX) {
		return -ERANGE;
	}

	strncpy(value, buffer, size);
	value[size] = '\0';
	ret = kstrtoul(value, 0, &bn);
	if (ret) {
		return ret;
	}

	if (bn >= EMU3_MAX_FILES_PER_DIR) {
		return -ERANGE;
	}

	mutex_lock(&info->lock);
	e3i = EMU3_I(inode);
	e3i->data.id = bn;
	mark_inode_dirty(inode);
	e3d = emu3_find_dentry_by_inode(inode, &b);
	e3d->data.id = bn;
	mark_buffer_dirty_inode(b, inode);
	brelse(b);
	mutex_unlock(&info->lock);

	return ret;
}

static const struct xattr_handler emu3_xattr_handler = {
	.prefix = XATTR_USER_PREFIX,
	.get = emu3_xattr_get,
	.set = emu3_xattr_set
};

const struct xattr_handler *emu3_xattr_handlers[] = {
	&emu3_xattr_handler,
	NULL
};
