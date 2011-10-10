/*  
 *	emu3_fs.c
 *	Copyright (C) 2011 David García Goñi <dagargo at gmail dot com>
 *
 *  Code based on bfs for Linux by Tigran Aivazian.
 *  lkm_emu.c
 *  E-mu E3 filesystem module for RO operations.
 *  http://pages.cpsc.ucalgary.ca/~crwth/programming/VFS/VFS.php
 *  http://tldp.org/LDP/lki/lki-3.html
 *  /usr/src/linux-headers-2.6.38-8-generic/include/linux/fs.h
 *  http://tldp.org/HOWTO/Module-HOWTO/x197.html
 *  Zip image: 0x6000000 blocks
 *  http://www.tldp.org/LDP/lkmpg/2.6/html/index.html
 *  http://thecoffeedesk.com/geocities/rkfs.html
 *  
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

static int emu3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	buf->f_type = EMU3_FS_TYPE;
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = info->clusters * EMU3_BSIZE;
	buf->f_bfree = (info->clusters - info->next_cluster) * EMU3_BSIZE; //TODO: info->next_cluster
	buf->f_bavail = buf->f_bfree;
	buf->f_files = EMU3_MAX_FILES;
	buf->f_ffree = EMU3_MAX_FILES - info->used_inodes; //TODO: info->used_inodes
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	buf->f_namelen = MAX_LENGTH_FILENAME;
	return 0;
}

static void emu3_put_super(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);

	if (info) {
		kfree(info);
		sb->s_fs_info = NULL;
	}
}

static const struct super_operations emu3_super_operations = {
	.alloc_inode	= emu3_alloc_inode,
	.destroy_inode	= emu3_destroy_inode,
	.write_inode	= NULL,
	.evict_inode	= NULL,
	.put_super      = emu3_put_super,
	.statfs		    = emu3_statfs,
};

unsigned int emu3_file_bcount(struct emu3_sb_info * sb, struct emu3_dentry * e3d, int * start, int * size) {
	unsigned int start_cluster = cpu_to_le16(e3d->start_cluster) - 1;
	unsigned int clusters = cpu_to_le16(e3d->clusters) - 1;
	unsigned int blocks = cpu_to_le16(e3d->blocks);
	if (blocks > sb->blocks_per_cluster) {
		//TODO: check message && ERROR
		printk(KERN_ERR "%s EOF wrong in file id %d.\n", EMU3_ERROR_MSG, EMU3_I_ID(e3d));
		return -1;
	}
	*size = ((clusters * sb->blocks_per_cluster) + blocks);
	*start = (start_cluster * sb->blocks_per_cluster) + sb->start_data_block;
	return 0;
}

struct inode * emu3_iget(struct super_block *sb, unsigned long id)
{
	struct inode * inode;
	struct buffer_head *b;
	struct emu3_sb_info *info;
	struct emu3_dentry * e3d;
	int block_count;
	int file_block_size;
	int file_block_start;
	int err;
	unsigned int block_num;
	int file_found;
	int entries_per_block;
	int file_count;

	info = EMU3_SB(sb);

	if (!info) {
		return NULL;
	}
	
	inode = iget_locked(sb, id);

	if (IS_ERR(inode)) {
		return ERR_PTR(-ENOMEM);
	}
	if (!(inode->i_state & I_NEW)) {
		return inode;
	}
	
	file_found = 0;
	file_count = 0;
	block_count = 0;
	block_num = info->start_root_dir_block;
	err = 0;
	while (file_count < EMU3_MAX_FILES) {
		block_count++;

		b = sb_bread(sb, block_num);
	
		e3d = (struct emu3_dentry *)b->b_data;
		entries_per_block = 0;
		while (entries_per_block < MAX_ENTRIES_PER_BLOCK && IS_EMU3_FILE(e3d)) {
			//emu3_file_bcount(info, e3d, &file_block_start, &file_block_size);
			//printk("%.3d: '%.16s (%d bytes)'.\n", EMU3_I_ID(e3d), e3d->name, file_block_size * EMU3_BSIZE);
			file_count++;
			entries_per_block++;
			if (EMU3_I_ID(e3d) == id) { //Regular file found
				//printk("Found inode %d!\n", id);
				err = emu3_file_bcount(info, e3d, &file_block_start, &file_block_size);
				file_found = 1;
				//TODO: check next < max
				break;
			}
			e3d++;
		}
	
		brelse(b);
		
		if (err)
			break;
		
		if (file_found)
			break;

		if (entries_per_block < MAX_ENTRIES_PER_BLOCK)
			break;
		
		block_num++;
	}
	
	if ((!file_found && id != ROOT_DIR_INODE_ID) || err) {
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}
	
	inode->i_ino = id;
	inode->i_mode = ((id == ROOT_DIR_INODE_ID)?S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH:S_IFREG) | S_IRUSR | S_IRGRP | S_IROTH;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_version = 1;
	inode->i_nlink = (id == ROOT_DIR_INODE_ID) ? 2 : 1;
	inode->i_op = (id == ROOT_DIR_INODE_ID)?&emu3_inode_operations_dir:&emu3_inode_operations_file;
	inode->i_fop = (id == ROOT_DIR_INODE_ID)?&emu3_file_operations_dir:&emu3_file_operations_file;
	inode->i_blocks = (id == ROOT_DIR_INODE_ID)?block_count:file_block_size;
	inode->i_size = inode->i_blocks * EMU3_BSIZE;
	EMU3_I(inode)->start_block = (id == ROOT_DIR_INODE_ID)?info->start_root_dir_block:file_block_start;
	EMU3_I(inode)->blocks = inode->i_blocks;
	if (id != ROOT_DIR_INODE_ID)
		inode->i_mapping->a_ops = &emu3_aops;
		
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
			info->info_block = cpu_to_le32(parameters[2]);
			info->start_root_dir_block = cpu_to_le32(parameters[4]);
			info->start_data_block = cpu_to_le32(parameters[8]);
			info->blocks_per_cluster = (0x10000 << (e3sb[0x28] - 1)) / EMU3_BSIZE;
			info->clusters = cpu_to_le32(parameters[9]);
			//TODO: check clusters ok.

			printk("EMU3 disk attributes: blocks: %ld; clusters: %ld; info: %ld; root: %ld; data: %ld; b/c: %ld.\n", info->blocks, info->clusters, info->info_block, info->start_root_dir_block, info->start_data_block, info->blocks_per_cluster);

			sb->s_op = &emu3_super_operations;

			inode = emu3_iget(sb, ROOT_DIR_INODE_ID);

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
	
	if (err < 0) {
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
