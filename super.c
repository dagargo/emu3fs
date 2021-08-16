/*
 *   super.c
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include "emu3_fs.h"

struct kmem_cache *emu3_inode_cachep;

static int init_inodecache(void)
{
	emu3_inode_cachep = kmem_cache_create("emu3_inode_cache",
					      sizeof(struct emu3_inode),
					      0, (SLAB_RECLAIM_ACCOUNT |
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

static int emu3_get_free_clusters(struct emu3_sb_info *info)
{
	int free_clusters = 0;
	int i;

	for (i = 1; i <= info->clusters; i++)
		if (info->cluster_list[i] == 0)
			free_clusters++;
	return free_clusters;
}

static int emu3_get_free_inodes(struct super_block *sb)
{
	int i, j;
	int free_ids = 0;
	struct emu3_dentry *e3d;
	struct buffer_head *b;
	struct emu3_sb_info *info = EMU3_SB(sb);

	for (i = 0; i < info->dir_content_blocks; i++) {
		b = sb_bread(sb, info->start_dir_content_block + i);
		//TODO: add check?

		e3d = (struct emu3_dentry *)b->b_data;
		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++)
			if (!EMU3_DENTRY_IS_FILE(e3d))
				free_ids++;

		brelse(b);
	}

	return free_ids;
}

static int emu3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

	//For the free space and free inodes we do not consider files.
	buf->f_type = EMU3_FS_TYPE;
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = info->clusters * info->blocks_per_cluster;
	buf->f_bfree = emu3_get_free_clusters(info) * info->blocks_per_cluster;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = emu3_get_free_inodes(sb);
	buf->f_ffree =
	    EMU3_ENTRIES_PER_BLOCK * info->dir_content_blocks - buf->f_files;
	buf->f_fsid.val[0] = (u32) id;
	buf->f_fsid.val[1] = (u32) (id >> 32);
	buf->f_namelen = EMU3_LENGTH_FILENAME;
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
		kfree(info->dir_content_block_list);
		kfree(info);
		sb->s_fs_info = NULL;
	}
}

//Base 0 search
int emu3_expand_cluster_list(struct inode *inode, sector_t block)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int cluster = ((int)block) / info->blocks_per_cluster;
	int next = e3i->start_cluster;
	int i = 0;

	while (info->cluster_list[next] != le16_to_cpu(EMU_LAST_FILE_CLUSTER)) {
		next = info->cluster_list[next];
		i++;
	}
	while (i < cluster) {
		int new = emu3_next_free_cluster(info);
		if (new < 0)
			return -ENOSPC;
		info->cluster_list[next] = new;
		next = new;
		i++;
	}
	info->cluster_list[next] = le16_to_cpu(EMU_LAST_FILE_CLUSTER);
	return 0;
}

//Base 0 search
int emu3_get_cluster(struct inode *inode, int n)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int next = e3i->start_cluster;
	int i = 0;

	while (i < n) {
		if (info->cluster_list[next] ==
		    le16_to_cpu(EMU_LAST_FILE_CLUSTER))
			return -1;
		next = info->cluster_list[next];
		i++;
	}
	return next;
}

void emu3_init_cluster_list(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);

	info->cluster_list[e3i->start_cluster] =
	    le16_to_cpu(EMU_LAST_FILE_CLUSTER);
}

void emu3_clear_cluster_list(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int next = e3i->start_cluster;

	while (info->cluster_list[next] != le16_to_cpu(EMU_LAST_FILE_CLUSTER)) {
		int prev = next;
		next = info->cluster_list[next];
		info->cluster_list[prev] = 0;
	}
	info->cluster_list[next] = 0;
}

short int emu3_get_inode_clusters(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int bytes_per_cluster = info->blocks_per_cluster * EMU3_BSIZE;
	short int clusters;

	if (inode->i_size == 0)
		clusters = 1;
	else {
		clusters = inode->i_size / bytes_per_cluster;
		if (inode->i_size % bytes_per_cluster > 0)
			clusters++;
	}
	return clusters;
}

//Prunes the cluster list to the real inode size
void emu3_update_cluster_list(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	short int clusters, last_cluster, next_cluster;
	int pruning;

	clusters = emu3_get_inode_clusters(inode);
	last_cluster = emu3_get_cluster(inode, clusters - 1);
	pruning = 0;
	next_cluster = info->cluster_list[last_cluster];
	while (next_cluster != le16_to_cpu(EMU_LAST_FILE_CLUSTER)) {
		if (pruning)
			info->cluster_list[last_cluster] = 0;
		else
			info->cluster_list[last_cluster] =
			    le16_to_cpu(EMU_LAST_FILE_CLUSTER);
		last_cluster = next_cluster;
		next_cluster = info->cluster_list[last_cluster];
		pruning = 1;
	}
	if (pruning)
		info->cluster_list[last_cluster] = 0;
}

int emu3_next_free_cluster(struct emu3_sb_info *info)
{
	int i;

	for (i = 1; i <= info->clusters; i++)
		if (info->cluster_list[i] == 0)
			return i;
	return -ENOSPC;
}

sector_t emu3_get_phys_block(struct inode *inode, sector_t block)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int cluster = ((int)block) / info->blocks_per_cluster;	//cluster amount
	int offset = ((int)block) % info->blocks_per_cluster;

	cluster = emu3_get_cluster(inode, cluster);
	if (cluster == -1)
		return -1;
	return info->start_data_block +
	    ((cluster - 1) * info->blocks_per_cluster) + offset;
}

static const struct super_operations emu3_super_operations = {
	.alloc_inode = emu3_alloc_inode,
	.destroy_inode = emu3_destroy_inode,
	.write_inode = emu3_write_inode,
	.evict_inode = emu3_evict_inode,
	.put_super = emu3_put_super,
	.statfs = emu3_statfs
};

void emu3_write_cluster_list(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;
	int i;

	for (i = 0; i < info->cluster_list_blocks; i++) {
		bh = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(bh->b_data,
		       &info->cluster_list[EMU3_CLUSTER_ENTRIES_PER_BLOCK * i],
		       EMU3_BSIZE);
		mark_buffer_dirty(bh);
		brelse(bh);
	}
}

void emu3_read_cluster_list(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;
	int i;

	for (i = 0; i < info->cluster_list_blocks; i++) {
		bh = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(&info->cluster_list[EMU3_CLUSTER_ENTRIES_PER_BLOCK * i],
		       bh->b_data, EMU3_BSIZE);
		brelse(bh);
	}
}

static int emu3_fill_super(struct super_block *sb, void *data, int silent)
{
	struct emu3_sb_info *info;
	struct buffer_head *sbh;
	struct buffer_head *b;
	unsigned char *e3sb;
	struct inode *inode;
	int i, j, k, size, index, err = 0;
	short *block;
	struct emu3_dentry *e3d;
	unsigned int *parameters;

	if (sb_set_blocksize(sb, EMU3_BSIZE) != EMU3_BSIZE) {
		printk(KERN_ERR
		       "%s: 512B block size not allowed on this device",
		       EMU3_MODULE_NAME);
		return -EINVAL;
	}

	info = kzalloc(sizeof(struct emu3_sb_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	sb->s_fs_info = info;

	sbh = sb_bread(sb, 0);

	if (!sbh) {
		err = -EIO;
		goto out1;
	}

	e3sb = (unsigned char *)sbh->b_data;

	//Check EMU3 string
	if (strncmp(EMU3_FS_SIGNATURE, e3sb, 4) != 0) {
		printk(KERN_ERR "%s: volume is not an EMU3 disk",
		       EMU3_MODULE_NAME);
		err = -EINVAL;
		goto out2;
	}

	parameters = (unsigned int *)e3sb;

	info->blocks = le32_to_cpu(parameters[1]);	//TODO: add 1 ??? Do we really use this?
	info->start_root_block = le32_to_cpu(parameters[2]);
	info->root_blocks = le32_to_cpu(parameters[3]);
	info->start_dir_content_block = le32_to_cpu(parameters[4]);
	info->dir_content_blocks = le32_to_cpu(parameters[5]);
	info->start_cluster_list_block = le32_to_cpu(parameters[6]);
	info->cluster_list_blocks = le32_to_cpu(parameters[7]);
	info->start_data_block = le32_to_cpu(parameters[8]);
	info->blocks_per_cluster = (0x10000 << (e3sb[0x28] - 1)) / EMU3_BSIZE;
	info->clusters = le32_to_cpu(parameters[9]) / (e3sb[0x28] >= 5 ? 2 : 1);

	//Now it's time to read the cluster list...
	size = EMU3_BSIZE * info->cluster_list_blocks;
	info->cluster_list = kzalloc(size, GFP_KERNEL);

	if (!info->cluster_list) {
		err = -ENOMEM;
		goto out2;
	}

	emu3_read_cluster_list(sb);

	printk(KERN_INFO "%s: %d blocks, %d clusters, %d blocks/cluster",
	       EMU3_MODULE_NAME, info->blocks, info->clusters,
	       info->blocks_per_cluster);
	printk(KERN_INFO "%s: cluster list start block @ %d + %d blocks",
	       EMU3_MODULE_NAME, info->start_cluster_list_block,
	       info->cluster_list_blocks);
	printk(KERN_INFO "%s: root start block @ %d + %d blocks",
	       EMU3_MODULE_NAME, info->start_root_block, info->root_blocks);
	printk(KERN_INFO "%s: dir content start block @ %d + %d blocks",
	       EMU3_MODULE_NAME, info->start_dir_content_block,
	       info->dir_content_blocks);
	printk(KERN_INFO "%s: data start block @ %d + %d clusters",
	       EMU3_MODULE_NAME, info->start_data_block, info->clusters);

	size = sizeof(bool) * info->dir_content_blocks;
	info->dir_content_block_list = kzalloc(size, GFP_KERNEL);
	if (!info->dir_content_block_list) {
		err = -ENOMEM;
		goto out3;
	}
	memset(info->dir_content_block_list, 0, size);

	sb->s_op = &emu3_super_operations;

	inode = emu3_get_inode(sb, EMU3_ROOT_DIR_I_ID);
	if (!inode) {
		err = -EIO;
		goto out4;
	}

	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		iput(inode);
		err = -ENOMEM;
		goto out4;
	}

	for (i = 0; i < info->root_blocks; i++) {
		b = sb_bread(sb, info->start_root_block + i);

		e3d = (struct emu3_dentry *)b->b_data;

		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++) {
			if (!EMU3_DENTRY_IS_DIR(e3d))
				continue;

			block = e3d->dattrs.block_list;
			for (k = 0; k < EMU3_BLOCKS_PER_DIR; k++, block++) {
				if (EMU3_IS_DIR_BLOCK_FREE(*block))
					continue;

				index =
				    le16_to_cpu(*block) -
				    info->start_dir_content_block;

				if (index < 0
				    || index >= info->dir_content_blocks) {
					printk(KERN_ERR
					       "%s: block %d marked as used by dir %.16s\n",
					       EMU3_MODULE_NAME, *block,
					       e3d->name);
					continue;
				}

				info->dir_content_block_list[index] = 1;
			}
		}

		brelse(b);
	}

	if (!err) {
		mutex_init(&info->lock);
		brelse(sbh);
		return 0;
	}

 out4:
	kfree(info->dir_content_block_list);
 out3:
	kfree(info->cluster_list);
 out2:
	brelse(sbh);
 out1:
	kfree(info);
	sb->s_fs_info = NULL;
	return err;
}

static struct dentry *emu3_fs_mount(struct file_system_type *fs_type,
				    int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, emu3_fill_super);
}

static struct file_system_type emu3_fs_type = {
	.owner = THIS_MODULE,
	.name = "emu3",
	.mount = emu3_fs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init emu3_init(void)
{
	int err;

	printk(KERN_INFO "%s: init", EMU3_MODULE_NAME);
	err = init_inodecache();
	if (err)
		return err;
	err = register_filesystem(&emu3_fs_type);
	if (err)
		destroy_inodecache();
	return err;
}

static void __exit emu3_exit(void)
{
	unregister_filesystem(&emu3_fs_type);
	destroy_inodecache();
	printk(KERN_INFO "%s: exit", EMU3_MODULE_NAME);
}

module_init(emu3_init);
module_exit(emu3_exit);

MODULE_LICENSE("GPL");

MODULE_AUTHOR("David García Goñi <dagargo@gmail.com>");
MODULE_DESCRIPTION("E-Mu EIII filesystem for Linux");
