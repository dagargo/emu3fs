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
#include <linux/writeback.h>

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

static int id_comparator(void * v, struct emu3_dentry * e3d) {
	int id = *((int*)v);
	if (e3d->id == id) {
		return 0;
	}
	return -1;
}

struct emu3_dentry * emu3_find_dentry(struct super_block *sb,
											struct buffer_head **bh,
											void * v,
											int (*comparator)(void *, struct emu3_dentry *))
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_dentry * e3d;
	int i, j;

	if (!info) {
		return NULL;
	}

	for (i = 0; i < info->root_dir_blocks; i++) {
		*bh = sb_bread(sb, info->start_root_dir_block + i);
	
		e3d = (struct emu3_dentry *)(*bh)->b_data;
		
		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			if (IS_EMU3_FILE(e3d) && e3d->type != FTYPE_DEL) {
				if (comparator(v, e3d) == 0) {
					return e3d;
				}
			}
			e3d++;
		}
	
		brelse(*bh);
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
	int free_clusters = 0;
	int i;
	for (i = 1; i <= info->clusters; i++) {
		if (info->cluster_list[i] == 0) {
			free_clusters++;
		}
	}

	buf->f_type = EMU3_FS_TYPE;
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = info->clusters * info->blocks_per_cluster;
	buf->f_bfree = free_clusters * info->blocks_per_cluster;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = info->used_inodes;
	buf->f_ffree = EMU3_MAX_FILES - info->used_inodes;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	buf->f_namelen = LENGTH_FILENAME;
	return 0;
}

static void emu3_put_super(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);

	mutex_lock(&info->lock);
	emu3_write_cluster_list(sb);
	mutex_unlock(&info->lock);	

	mutex_destroy(&info->lock);

	if (info) {
		kfree(info->cluster_list);
		kfree(info);
		sb->s_fs_info = NULL;
	}
}

void emu3_mark_as_non_empty(struct super_block *sb) {
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;	
	char * data;

	bh = sb_bread(sb, 1);
	data = (char *) bh->b_data;
	data[0x0] = 0x0a;
	mark_buffer_dirty(bh);
	brelse(bh);
	
	bh = sb_bread(sb, info->start_info_block);
	data = (char *) bh->b_data;
	data[0x12] = 0x09;
	data[0x13] = 0x00;
	mark_buffer_dirty(bh);
	brelse(bh);
}

//Base 0 search
int emu3_expand_cluster_list(struct inode * inode, sector_t block) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int cluster = ((int)block) / info->blocks_per_cluster;
	int next = e3i->start_cluster;
	int i = 0;
	while (info->cluster_list[next] != cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
		next = info->cluster_list[next];
		i++;
	}
	while (i < cluster) {
		int new = emu3_next_available_cluster(info);
		if (new < 0) {
			return -ENOSPC;
		}
		info->cluster_list[next] = new;
		next = new;
		i++;
	}
	info->cluster_list[next] = cpu_to_le16(LAST_CLUSTER_OF_FILE);
	return 0;
}


//Base 0 search
int emu3_get_cluster(struct inode * inode, int n) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int next = e3i->start_cluster;
	int i = 0;
	while (i < n) {
		if (info->cluster_list[next] == cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
			return -1;
		}
		next = info->cluster_list[next];
		i++;
	}
	return next;
}

void emu3_init_cluster_list(struct inode * inode) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	info->cluster_list[e3i->start_cluster] = cpu_to_le16(LAST_CLUSTER_OF_FILE);
}

void emu3_clear_cluster_list(struct inode * inode) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int next = e3i->start_cluster;
	while (info->cluster_list[next] != cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
		int prev = next;
		next = info->cluster_list[next];
		info->cluster_list[prev] = 0;
	}
	info->cluster_list[next] = 0;
}

//Prunes the cluster list to the real inode size
void emu3_update_cluster_list(struct inode * inode) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	short int clusters, last_cluster;
	int prunning;
	emu3_get_file_geom(inode, &clusters, NULL, NULL);
	last_cluster = emu3_get_cluster(inode, clusters - 1);
	prunning = 0; 
	while (info->cluster_list[last_cluster] != cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
		int next = info->cluster_list[last_cluster];
		if (prunning) {
			info->cluster_list[last_cluster] = 0;
		}
		else {
			info->cluster_list[last_cluster] = cpu_to_le16(LAST_CLUSTER_OF_FILE);
		}
		last_cluster = next;
		prunning = 1;
	}
	if (prunning) {
		info->cluster_list[last_cluster] = 0;
	}
}

int emu3_next_available_cluster(struct emu3_sb_info * info) {
	int i;
	for (i = 1; i <= info->clusters; i++) {
		if (info->cluster_list[i] == 0) {
			return i;
		}
	}
	return -ENOSPC;
}

unsigned int emu3_get_phys_block(struct inode * inode, sector_t block) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int cluster = ((int)block) / info->blocks_per_cluster; //cluster amount
	int offset = ((int)block) % info->blocks_per_cluster;
	cluster = emu3_get_cluster(inode, cluster);
	if (cluster == -1) {
		return -1;
	}
	return info->start_data_block + ((cluster - 1) * info->blocks_per_cluster) + offset;
}

static int emu3_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	unsigned int ino = TO_EMU3_ID(inode->i_ino);
	struct emu3_dentry *e3d;
	struct emu3_inode *e3i;
	struct buffer_head *bh;
	int err = 0;
	
	if (ino == ROOT_DIR_INODE_ID) {
	    return 0;
	}
		
	mutex_lock(&info->lock);

	printk("%s: writing to inode %d (kernel %ld)...\n", EMU3_MODULE_NAME, ino, inode->i_ino);
	
    e3d = emu3_find_dentry_by_id(inode->i_sb, ino, &bh);
	if (!e3d) {
		mutex_unlock(&info->lock);
		return PTR_ERR(e3d);
	}

	emu3_update_cluster_list(inode);

	emu3_get_file_geom(inode, &e3d->clusters, &e3d->blocks, &e3d->bytes);
	
    e3i = EMU3_I(inode);
    e3d->start_cluster = e3i->start_cluster;
	
	mark_buffer_dirty(bh);
	if (wbc->sync_mode == WB_SYNC_ALL) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			err = -EIO;
		}
	}
	brelse(bh);

	if (ino == 0) {
		emu3_mark_as_non_empty(inode->i_sb);
	}

	mutex_unlock(&info->lock);

	return err;
}

static void emu3_evict_inode(struct inode *inode) {
	truncate_inode_pages(&inode->i_data, 0);
	invalidate_inode_buffers(inode);
	clear_inode(inode);
}

static const struct super_operations emu3_super_operations = {
	.alloc_inode	= emu3_alloc_inode,
	.destroy_inode	= emu3_destroy_inode,
	.write_inode	= emu3_write_inode,
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
		printk(KERN_ERR "%s. EOF wrong in file id %d.\n", EMU3_MODULE_NAME, EMU3_I_ID(e3d));
		return -1;
	}
	*bsize = (clusters * sb->blocks_per_cluster) + blocks;
	*fsize = (((*bsize) - 1) * EMU3_BSIZE) + cpu_to_le16(e3d->bytes);
	*start = (start_cluster * sb->blocks_per_cluster) + sb->start_data_block;
	return 0;
}

void emu3_get_file_geom(struct inode * inode, 
                        unsigned short *clusters, 
                        unsigned short *blocks, 
                        unsigned short *bytes) {
   	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
    int bytes_per_cluster =  info->blocks_per_cluster * EMU3_BSIZE;
    unsigned int clusters_rem;
    unsigned int size = inode->i_size;

	if (clusters) {
	    *clusters = (size / bytes_per_cluster) + 1;
	}    
	clusters_rem = size % bytes_per_cluster;
	if (blocks) {
		*blocks = (clusters_rem / EMU3_BSIZE) + 1;
	}
	if (bytes) {
		*bytes = clusters_rem % EMU3_BSIZE;
	}
}

struct inode * emu3_get_inode(struct super_block *sb, unsigned long id)
{
	struct inode * inode;
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct emu3_dentry * e3d = NULL;
	struct emu3_inode * e3i;
	int file_block_size;
	int file_block_start;
	int file_size;
	struct buffer_head *b = NULL;

	if (id == ROOT_DIR_INODE_ID) {
		file_block_start = info->start_root_dir_block;
		file_block_size = info->root_dir_blocks;
		file_size = info->root_dir_blocks * EMU3_BSIZE;		
	}
	else {
		e3d = emu3_find_dentry_by_id(sb, TO_EMU3_ID(id), &b);
	
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

	inode->i_ino = id;//TODO: needed?
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
	if (id != ROOT_DIR_INODE_ID) {
		e3i = EMU3_I(inode);
		e3i->start_cluster = e3d->start_cluster;
		inode->i_mapping->a_ops = &emu3_aops;	
		brelse(b);
	}
		
	unlock_new_inode(inode);
	
	return inode;
}

void emu3_write_cluster_list(struct super_block *sb) {
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;
	int i;
		
	for (i = 0; i < info->cluster_list_blocks; i++) {
		bh = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(bh->b_data, &info->cluster_list[EMU3_CENTRIES_PER_BLOCK * i], EMU3_BSIZE);
		mark_buffer_dirty(bh);
		brelse(bh);
	}
}

void emu3_read_cluster_list(struct super_block *sb) {
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;
	int i;
		
	for (i = 0; i < info->cluster_list_blocks; i++) {
		bh = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(&info->cluster_list[EMU3_CENTRIES_PER_BLOCK * i], bh->b_data, EMU3_BSIZE);
		brelse(bh);
	}
}

static int emu3_fill_super(struct super_block *sb, void *data, int silent)
{
	struct emu3_sb_info *info;
	struct buffer_head *sbh;
	unsigned char * e3sb;
	struct inode * inode;
	int err = 0;
	unsigned int * parameters;
	struct emu3_dentry * e3d;
	struct buffer_head *bh;
	int i, j;

	if (sb_set_blocksize(sb, EMU3_BSIZE) != EMU3_BSIZE) {
		printk(KERN_ERR "%s: impossible to mount. Linux does not allow 512B block size on this device.", EMU3_MODULE_NAME);
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
			printk(KERN_ERR "%s: volume is not an EMU3 disk.", EMU3_MODULE_NAME);
		}
		else {
			parameters = (unsigned int *) e3sb;
			
			info->blocks = cpu_to_le32(parameters[1]); //TODO: add 1 ??? Do we really use this?
			info->start_info_block = cpu_to_le32(parameters[2]);
			info->info_blocks = cpu_to_le32(parameters[3]);
			info->start_root_dir_block = cpu_to_le32(parameters[4]);
			info->root_dir_blocks = cpu_to_le32(parameters[5]);
			info->start_cluster_list_block = cpu_to_le32(parameters[6]);
			info->cluster_list_blocks = cpu_to_le32(parameters[7]);
			info->start_data_block = cpu_to_le32(parameters[8]);
			info->blocks_per_cluster = (0x10000 << (e3sb[0x28] - 1)) / EMU3_BSIZE;
			info->clusters = cpu_to_le32(parameters[9]);
			
			//We need to calculate some things here.
			info->last_inode = -1;
			info->used_inodes = 0;

			for (i = 0; i < info->root_dir_blocks; i++) {
				bh = sb_bread(sb, info->start_root_dir_block + i);
	
				e3d = (struct emu3_dentry *)bh->b_data;
		
				for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
					if (IS_EMU3_FILE(e3d)) {
						if (e3d->type != FTYPE_DEL)  {
							info->last_inode = e3d->id;
						}
						info->used_inodes++;
					}
					e3d++;
				}
				brelse(bh);
			}
			//Calculations done.
			
			//Now it's time to read the cluster list
			info->cluster_list = kzalloc(EMU3_BSIZE * info->cluster_list_blocks, GFP_KERNEL);
			if (!info->cluster_list) {
				return -ENOMEM;
			}
			emu3_read_cluster_list(sb);
			//Done.

			printk("%s: %d blocks, %d clusters, b/c %d.\n", EMU3_MODULE_NAME, info->blocks, info->clusters, info->blocks_per_cluster);
			printk("%s: info init block @ %d + %d blocks.\n", EMU3_MODULE_NAME, info->start_info_block, info->info_blocks);
			printk("%s: cluster list init block @ %d + %d blocks.\n", EMU3_MODULE_NAME, info->start_cluster_list_block, info->cluster_list_blocks);
			printk("%s: root init block @ %d + %d blocks.\n", EMU3_MODULE_NAME, info->start_root_dir_block, info->root_dir_blocks);
			printk("%s: data init block @ %d + %d clusters.\n", EMU3_MODULE_NAME, info->start_data_block, info->clusters);
						
			sb->s_op = &emu3_super_operations;

			inode = emu3_get_inode(sb, ROOT_DIR_INODE_ID);

			if (!inode) {
				err = -EIO;
			}
			else {
		    	sb->s_root = d_make_root(inode);
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
