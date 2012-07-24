/*  
 *	inode.c
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include "emu3_fs.h"

static struct kmem_cache * emu3_inode_cachep;

static struct inode *emu3_alloc_inode(struct super_block *sb)
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
	INIT_LIST_HEAD(&inode->i_dentry);
	kmem_cache_free(emu3_inode_cachep, EMU3_I(inode));
}

static void emu3_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, emu3_i_callback);
}

static void emu3_init_once(void *foo)
{
	struct emu3_inode * e3i = foo;
	inode_init_once(&e3i->vfs_inode);
}

static int init_inodecache(void)
{
	emu3_inode_cachep = kmem_cache_create("emu3_inode_cache",
					     sizeof(struct emu3_inode),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     emu3_init_once);
	if (emu3_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(emu3_inode_cachep);
}

inline void get_emu3_fulldentry(char * fullname, struct emu3_dentry * e3d) {
	sprintf(fullname, FILENAME_TEMPLATE, EMU3_I_ID(e3d), e3d->name);
}

static int id_comparator(void * v, struct emu3_dentry * e3d) {
	int id = *((int*)v);
	if (EMU3_I_ID(e3d) == id) {
		return 0;
	}
	return -1;
}

struct emu3_dentry * emu3_find_dentry(struct super_block *sb, 
											struct buffer_head **b,
											void * v,
											int (*comparator)(void *, struct emu3_dentry *))
{
	struct emu3_sb_info *info;
	struct emu3_dentry * e3d;
	int i, j;
	int entries_per_block;

	info = EMU3_SB(sb);

	if (!info) {
		return NULL;
	}

	for (i = 0; i < info->root_dir_blocks; i++) {
		*b = sb_bread(sb, info->start_root_dir_block + i);
	
		e3d = (struct emu3_dentry *)(*b)->b_data;
		
		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (IS_EMU3_FILE(e3d)) {
				entries_per_block++;
				if (comparator(v, e3d) == 0) {
					return e3d;
				}
			}
			e3d++;
		}
	
		brelse(*b);
	}
	
	return NULL;
}

static inline struct emu3_dentry * emu3_find_dentry_by_id(struct super_block *sb, unsigned long id, struct buffer_head **b)
{
	return emu3_find_dentry(sb, b, &id, id_comparator);
}

static int emu3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	buf->f_type = EMU3_FS_TYPE;
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = info->clusters * info->blocks_per_cluster;
	buf->f_bfree = (info->clusters - info->next_available_cluster) * info->blocks_per_cluster;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = info->used_inodes;
	buf->f_ffree = EMU3_MAX_FILES - info->used_inodes;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	buf->f_namelen = LENGTH_SHOWED_FILENAME;
	return 0;
}

static void emu3_put_super(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);

	mutex_destroy(&info->lock);

	if (info) {
		kfree(info);
		sb->s_fs_info = NULL;
	}
}

static void emu3_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);
	invalidate_inode_buffers(inode);
	end_writeback(inode);
}

static const struct super_operations emu3_super_operations = {
	.alloc_inode	= emu3_alloc_inode,
	.destroy_inode	= emu3_destroy_inode,
	.write_inode	= NULL, //TODO: emu3_write_inode,
	.evict_inode	= emu3_evict_inode,
	.put_super      = emu3_put_super,
	.statfs		    = emu3_statfs
};

unsigned int emu3_file_block_count(struct emu3_sb_info * sb, 
									struct emu3_dentry * e3d,
									int * start,
									int * bsize,
									int * fsize) {
	unsigned int start_cluster = cpu_to_le16(e3d->start_cluster) - 1;
	unsigned int clusters = cpu_to_le16(e3d->clusters) - 1;
	unsigned int blocks = cpu_to_le16(e3d->blocks);
	if (blocks > sb->blocks_per_cluster) {
		//TODO: check message && ERROR
		printk(KERN_ERR "%s EOF wrong in file id %d.\n", EMU3_ERROR_MSG, EMU3_I_ID(e3d));
		return -1;
	}
	*bsize = (clusters * sb->blocks_per_cluster) + blocks;
	*fsize = (((*bsize) - 1) * EMU3_BSIZE) + cpu_to_le16(e3d->bytes);
	*start = (start_cluster * sb->blocks_per_cluster) + sb->start_data_block;
	return 0;
}

struct inode * emu3_get_inode(struct super_block *sb, unsigned long id)
{
	struct inode * inode;
	struct emu3_sb_info *info;
	struct emu3_dentry * e3d;
	int file_block_size;
	int file_block_start;
	int file_size;
	struct buffer_head *b = NULL;

	info = EMU3_SB(sb);

	if (!info) {
		return NULL;
	}
	
	if (id == ROOT_DIR_INODE_ID) {
		file_block_start = info->start_root_dir_block;
		file_block_size = info->root_dir_blocks;
		file_size = info->root_dir_blocks * EMU3_BSIZE;		
	}
	else {
		e3d = emu3_find_dentry_by_id(sb, id, &b);
	
		if (!e3d) {
			return ERR_PTR(-EIO);
		}
	
		emu3_file_block_count(info, e3d, &file_block_start, &file_block_size, &file_size);
	}
	
	inode = iget_locked(sb, id);

	if (IS_ERR(inode)) {
		return ERR_PTR(-ENOMEM);
	}
	if (!(inode->i_state & I_NEW)) {
		return inode;
	}

	inode->i_ino = id;
	inode->i_mode = ((id == ROOT_DIR_INODE_ID)?S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH:S_IFREG) | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_version = 1;
	set_nlink (inode, (id == ROOT_DIR_INODE_ID) ? 2 : 1);
	inode->i_op = (id == ROOT_DIR_INODE_ID)?&emu3_inode_operations_dir:&emu3_inode_operations_file;
	inode->i_fop = (id == ROOT_DIR_INODE_ID)?&emu3_file_operations_dir:&emu3_file_operations_file;
	inode->i_blocks = file_block_size;
	inode->i_size = file_size;
	inode->i_atime = CURRENT_TIME;
	inode->i_mtime = CURRENT_TIME;
	inode->i_ctime = CURRENT_TIME;
	EMU3_I(inode)->start_block = file_block_start;
	EMU3_I(inode)->blocks = inode->i_blocks;
	if (id != ROOT_DIR_INODE_ID) {
		inode->i_mapping->a_ops = &emu3_aops;	
		brelse(b);
	}
		
	unlock_new_inode(inode);
	
	return inode;
}

static int emu3_fill_super(struct super_block *sb, void *data, int silent)
{
	struct emu3_sb_info *info;
	struct buffer_head *sbh;
	unsigned char * e3sb;
	struct inode * inode;
	int err = 0;
	unsigned int * parameters;

	if (sb_set_blocksize(sb, EMU3_BSIZE) != EMU3_BSIZE) {
		printk(KERN_ERR "%s Impossible to mount. Linux does not allow 512B block size on this device.", EMU3_ERROR_MSG);
		return -EINVAL;
	}

	info = kzalloc(sizeof(struct emu3_sb_info), GFP_KERNEL);
	if (!info) {
		return -ENOMEM;
	}
	
	sb->s_fs_info = info;
		
	sbh = sb_bread(sb, 0);
	
	if (sbh) {
		e3sb = (unsigned char *)sbh->b_data;
		
		//Check EMU3 string
		if (strncmp(EMU3_FS_SIGNATURE, e3sb, 4) != 0) {
			err = -EINVAL;
			printk(KERN_ERR "Volume does not look like an EMU3 disk.");
		}
		else {
			parameters = (unsigned int *) e3sb;
			
			info->blocks = cpu_to_le32(parameters[1]);
			info->start_info_block = cpu_to_le32(parameters[2]);
			info->info_blocks = cpu_to_le32(parameters[3]);
			info->start_root_dir_block = cpu_to_le32(parameters[4]);
			info->root_dir_blocks = cpu_to_le32(parameters[5]);
			info->start_data_block = cpu_to_le32(parameters[8]);
			info->blocks_per_cluster = (0x10000 << (e3sb[0x28] - 1)) / EMU3_BSIZE;
			info->clusters = cpu_to_le32(parameters[9]);
			//TODO: check clusters ok. Seems so...

			printk("%s: %d blocks, %d clusters, b/c %d.\n", EMU3_MODULE_NAME, info->blocks, info->clusters, info->blocks_per_cluster);
			printk("%s: info init sector @ %d + %d sectors.\n", EMU3_MODULE_NAME, info->start_info_block, info->info_blocks);
			printk("%s: root init sector @ %d + %d sectors.\n", EMU3_MODULE_NAME, info->start_root_dir_block, info->root_dir_blocks);
			printk("%s: data init sector @ %d + %d clusters.\n", EMU3_MODULE_NAME, info->start_data_block, info->clusters);
						
			sb->s_op = &emu3_super_operations;

			inode = emu3_get_inode(sb, ROOT_DIR_INODE_ID);

			if (!inode) {
				err = -EIO;
			}
			else {
		    	sb->s_root = d_alloc_root(inode);
		    	if (!sb->s_root) {
		            iput(inode);
					err = -EIO;
		    	}
        	}	
		}
	}
	
	if (!err) {
		mutex_init(&info->lock);
	}
	else {
		kfree(info);
		sb->s_fs_info = NULL;
	}
	
	brelse(sbh);
	return err;
}

static struct dentry *emu3_fs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, emu3_fill_super);
}

static struct file_system_type emu3_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "emu3",
	.mount		= emu3_fs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init(void)
{
	int err;
	printk(KERN_INFO "Init %s.\n", EMU3_MODULE_NAME);
	err = init_inodecache();
	if (err) {
		return err;
	}
	err = register_filesystem(&emu3_fs_type);
	if (err) {
		destroy_inodecache();
	}
	return err;
}

static void __exit exit(void)
{
	unregister_filesystem(&emu3_fs_type);
	destroy_inodecache();
	printk(KERN_INFO "Exit %s.\n", EMU3_MODULE_NAME);
}

module_init(init);
module_exit(exit);

MODULE_LICENSE("GPL");

MODULE_AUTHOR("David García Goñi <dagargo at gmail dot com>");
MODULE_DESCRIPTION("E-mu E3 family filesystem for Linux");
