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
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/vfs.h>

#include "emu3_fs.h"

static struct kmem_cache * emu3_inode_cachep;

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

	printk("Lookupping...\n");

	if (dentry->d_name.len > MAX_LENGTH_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	block_num = info->start_root_dir_block;
	e3i = EMU3_I(dir);
	for (i = 0; i < e3i->blocks; i++) {
		b = sb_bread(dir->i_sb, block_num);
	
		e3d = (struct emu3_dentry *)b->b_data;
	
		entries_per_block = 0;
		while (entries_per_block < MAX_ENTRIES_PER_BLOCK && IS_EMU3_FILE(e3d)) {
			if(strncmp(dentry->d_name.name, e3d->name, dentry->d_name.len) == 0) {
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

//TODO:complete
static int emu3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	buf->f_type = 0x454d5533; //TODO (EMU3)
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = 0x6000000; //Wrong!!! /x200
	buf->f_bfree = 0;
	buf->f_files = 100;
	buf->f_ffree = 0;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	buf->f_namelen = 16;
	return 0;
}

static int emu3_readdir(struct file *f, void *dirent, filldir_t filldir)
{
    int i;
    int filename_length;
    int block_num;
    int entries_per_block;
    struct dentry *de = f->f_dentry;
   	struct emu3_sb_info *info = EMU3_SB(de->d_inode->i_sb);
   	struct buffer_head *b;
   	struct emu3_dentry * e3d;
   	struct emu3_inode * e3i;
   	
	printk("Reddiring...\n");
    
    //TODO: check error.
    if (de->d_inode->i_ino != ROOT_DIR_INODE_ID)
    	return -EBADF;

    if(f->f_pos > 0 )
    	return 0; //Returning an error here (-EBADF) makes ls giving a WRONG DESCRIPTOR FILE.
    
    if (filldir(dirent, ".", 1, f->f_pos++, de->d_inode->i_ino, DT_DIR) < 0)
    	return 0;
    if (filldir(dirent, "..", 2, f->f_pos++, de->d_parent->d_inode->i_ino, DT_DIR) < 0)
    	return 0;

	block_num = info->start_root_dir_block;
	e3i = EMU3_I(de->d_inode);
	for (i = 0; i < e3i->blocks; i++) {
		b = sb_bread(de->d_inode->i_sb, block_num);
	
		e3d = (struct emu3_dentry *)b->b_data;
	
		entries_per_block = 0;
		while (entries_per_block < MAX_ENTRIES_PER_BLOCK && IS_EMU3_FILE(e3d)) {
			filename_length = strnlen(e3d->name, MAX_LENGTH_FILENAME);
			if (filldir(dirent, e3d->name, filename_length, f->f_pos++, EMU3_I_ID(e3d), DT_REG) < 0)
    			return 0;
			e3d++;
			entries_per_block++;
		}
	
		brelse(b);

		if (entries_per_block < MAX_ENTRIES_PER_BLOCK)
			break;

		block_num++;
	}
    return 0;
}

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

static const struct file_operations emu3_file_operations_file = {
	.read		= NULL,
	.readdir	= NULL,
	.fsync		= NULL,
	.llseek		= NULL,
};

static const struct inode_operations emu3_inode_operations_file = {
	.create			= NULL,
	.lookup			= NULL,
	.link			= NULL,
	.unlink			= NULL,
	.rename			= NULL,
};

static const struct file_operations emu3_file_operations_dir = {
	.read		= generic_read_dir,
	.readdir	= emu3_readdir,
	.fsync		= generic_file_fsync,
	.llseek		= generic_file_llseek,
};

static const struct inode_operations emu3_inode_operations_dir = {
	.create			= NULL,
	.lookup			= emu3_lookup,
	.link			= NULL,
	.unlink			= NULL,
	.rename			= NULL,
};

unsigned int emu3_file_bcount(struct emu3_sb_info * sb, struct emu3_dentry * e3d, int * start, int * size) {
	unsigned int start_cluster = cpu_to_le16(e3d->start_cluster) - 1;
	unsigned int clusters = cpu_to_le16(e3d->clusters) - 1;
	unsigned int blocks = cpu_to_le16(e3d->blocks);
	if (blocks > sb->blocks_per_cluster) {
		//TODO: check message && ERROR
		printk(KERN_ERR "Error in %s. EOF wrong in file id %d.\n", EMU3_MODULE_NAME, EMU3_I_ID(e3d));
		return -1;
	}
	*size = ((clusters * sb->blocks_per_cluster) + blocks);
	*start = (start_cluster * sb->blocks_per_cluster) + sb->start_data_block;
	return 0;
}

static struct inode * emu3_iget(struct super_block *sb, unsigned long id)
{
	struct inode * inode;
	struct buffer_head *b;
	struct emu3_sb_info *info;
	struct emu3_dentry * e3d;
	int file_count;
	int block_count;
	int file_block_size;
	int file_block_start;
	int err;
	unsigned int block_num;
	int file_found;
	int entries_per_block;

	info = EMU3_SB(sb);

	if (!info) {
		return NULL;
	}
	
	inode = iget_locked(sb, id);

	if (IS_ERR(inode)) {
		return ERR_PTR(-ENOMEM);
	}
	if (inode->i_state & I_NEW) {
//		//TODO: Why the hell this is needed?
//		if (inode->i_private == NULL) printk("Why NULL %d????????????????????????????????????????????\n", inode->i_ino);
//		printk("Initiating emu3 inode 2 (%d)...\n", id);
//		inode->i_private = kzalloc(sizeof(struct emu3_inode), GFP_KERNEL);
	}
	else {
		return inode;
	}
	
	inode->i_ino = id;
	inode->i_mode = ((id == ROOT_DIR_INODE_ID)?S_IFDIR | S_IXUSR:S_IFREG) | S_IRUSR;
	inode->i_uid = 0;
	inode->i_gid = 0;	
	inode->i_version = 1;
	inode->i_nlink = 2;
	inode->i_op = (id == ROOT_DIR_INODE_ID)?&emu3_inode_operations_dir:&emu3_inode_operations_file;
	inode->i_fop = (id == ROOT_DIR_INODE_ID)?&emu3_file_operations_dir:&emu3_file_operations_file;
	
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
			emu3_file_bcount(info, e3d, &file_block_start, &file_block_size);
			//printk("%.3d: '%.16s (%d bytes)'.\n", EMU3_I_ID(e3d), e3d->name, file_block_size * EMU3_BSIZE);
			if (EMU3_I_ID(e3d) == id) { //Regular file found
				//printk("Found inode %d!\n", id);
				err = emu3_file_bcount(info, e3d, &file_block_start, &file_block_size);
				file_found = 1;
				break;
			}
			e3d++;
			file_count++;
			entries_per_block++;
		}
	
		brelse(b);
		
		if (err)
			break;
		
		if (file_found != 0)
			break;

		if (entries_per_block < MAX_ENTRIES_PER_BLOCK)
			break;
		
		block_num++;
	}
	
	if ((!file_found && id != ROOT_DIR_INODE_ID) || err) {
		iget_failed(inode);
		return ERR_PTR(-EIO);
	} 

	inode->i_blocks = (id == ROOT_DIR_INODE_ID)?block_count:file_block_size;
	inode->i_size = inode->i_blocks * EMU3_BSIZE;
	EMU3_I(inode)->start_block = (id == ROOT_DIR_INODE_ID)?info->start_root_dir_block:file_block_start;
	EMU3_I(inode)->blocks = inode->i_blocks;
		
	unlock_new_inode(inode);
	
	return inode;
}

static int emu3_fill_super(struct super_block *sb, void *data, int silent)
{
	struct emu3_sb_info *info;
	struct buffer_head *sbh;
	unsigned char * e3sb;
	struct inode * inode;
	int ret = 0;
	unsigned int * parameters;

	info = kzalloc(sizeof(struct emu3_sb_info), GFP_KERNEL);
	if (!info) {
		return -ENOMEM;
	}
	sb->s_fs_info = info;

	sb_set_blocksize(sb, EMU3_BSIZE);
	
	sbh = sb_bread(sb, 0);
	
	if (sbh) {
		e3sb = (unsigned char *)sbh->b_data;
		
		//Check EMU3 string
		if (strncmp(EMU3_FS_SIGNATURE, e3sb, 4) != 0) {
			ret = -EINVAL;
			printk(KERN_ERR "Volume does not look like an EMU3 disk.");
		}
		else {
			parameters = (unsigned int *) e3sb;
			
			info->info_block = cpu_to_le32(parameters[2]);
			info->start_root_dir_block = cpu_to_le32(parameters[4]);
			info->start_data_block = cpu_to_le32(parameters[8]);
			info->blocks_per_cluster = (0x10000 << (e3sb[0x28] - 1)) / EMU3_BSIZE;

			printk("EMU3 disk attributes: %ld, %ld, %ld, %ld\n", info->info_block, info->start_root_dir_block, info->start_data_block, info->blocks_per_cluster);

			sb->s_op = &emu3_super_operations;

			inode = emu3_iget(sb, ROOT_DIR_INODE_ID);

			if (!inode) {
				ret = -EIO;
			}
			else {
		    	sb->s_root = d_alloc_root(inode);
		    	if (!sb->s_root) {
		            iput(inode);
					ret = -EIO;
		    	}
        	}	
		}
	}
	
	//ret = -EIO;
	
	if (ret < 0) {
		kfree(info);
		sb->s_fs_info = NULL;
	}
	
	brelse(sbh);
	return ret;
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


