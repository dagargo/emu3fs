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
 *   along with emu3fs. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include "emu3_fs.h"

static struct kmem_cache *emu3_inode_cachep;

static void emu3_set_file_size(struct inode *inode,
			       struct emu3_file_attrs *fattrs)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int bytes_per_cluster = info->blocks_per_cluster * EMU3_BSIZE;
	unsigned int clusters_rem;
	unsigned int size = inode->i_size;

	fattrs->clusters = (size / bytes_per_cluster) + 1;
	clusters_rem = size % bytes_per_cluster;
	fattrs->blocks = (clusters_rem / EMU3_BSIZE) + 1;
	fattrs->bytes = clusters_rem % EMU3_BSIZE;
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

	kmem_cache_free(emu3_inode_cachep, EMU3_I(inode));
}

static void emu3_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, emu3_i_callback);
}

static int emu3_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_dentry *e3d;
	struct buffer_head *bh;
	int err = 0;

	if (EMU3_IS_I_ROOT_DIR(inode) || EMU3_IS_I_REG_DIR(inode, info))
		return 0;

	mutex_lock(&info->lock);

	e3d = emu3_find_dentry_by_inode(inode, &bh);
	if (!e3d) {
		mutex_unlock(&info->lock);
		return -ENOENT;
	}

	emu3_update_cluster_list(inode);

	emu3_set_file_size(inode, &e3d->data.fattrs);

	mark_buffer_dirty(bh);
	if (wbc->sync_mode == WB_SYNC_ALL) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh))
			err = -EIO;
	}
	brelse(bh);
	mutex_unlock(&info->lock);
	return err;
}

static void emu3_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);
	invalidate_inode_buffers(inode);
	clear_inode(inode);
}

//This happens occasionally, luckily only on single dir images, so we try to fix it.
//In some cases, all the used blocks are bad. See E-mu Classic Series V5.
static bool emu3_fix_first_dir_blocks(struct emu3_dentry *e3d,
				      struct emu3_sb_info *info)
{
	int i;
	short new;
	short old;
	short *blknum;

	blknum = e3d->data.dattrs.block_list;
	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++, blknum++) {
		old = le16_to_cpu(*blknum);
		if (EMU3_IS_DIR_BLOCK_FREE(old))
			break;
		new = info->start_dir_content_block + i;
		if (new != old) {
			printk(KERN_WARNING
			       "%s: Directory block changed from 0x%04x to 0x%04x",
			       EMU3_MODULE_NAME, old, new);
			*blknum = cpu_to_le16(new);
		}
	}

	return 1;
}

static void emu3_init_once(void *foo)
{
	struct emu3_inode *e3i = foo;
	inode_init_once(&e3i->vfs_inode);
}

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
	int free_inos = 0;
	struct emu3_dentry *e3d;
	struct buffer_head *b;
	struct emu3_sb_info *info = EMU3_SB(sb);

	for (i = 0; i < info->root_blocks + info->dir_content_blocks; i++) {
		b = sb_bread(sb, info->start_root_block + i);
		//TODO: add check?

		e3d = (struct emu3_dentry *)b->b_data;
		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++)
			if (i < info->root_blocks) {
				if (!EMU3_DENTRY_IS_DIR(e3d))
					free_inos++;
			} else {
				if (!EMU3_DENTRY_IS_FILE(e3d))
					free_inos++;
			}

		brelse(b);
	}

	return free_inos;
}

static int emu3_get_free_dir_blocks(struct emu3_sb_info *info)
{
	bool *b;
	int i, free_blks = 0;

	b = info->dir_content_block_list;
	for (i = 0; i < info->dir_content_blocks; i++, b++)
		if (!*b)
			free_blks++;

	return free_blks;
}

static int emu3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

	//For the free space and free inodes we do not consider files.
	buf->f_type = EMU3_FS_TYPE;
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = info->root_blocks + info->dir_content_blocks +
	    info->clusters * info->blocks_per_cluster;
	buf->f_bfree =
	    emu3_get_free_clusters(info) * info->blocks_per_cluster +
	    emu3_get_free_dir_blocks(info);
	buf->f_bavail = buf->f_bfree;
	buf->f_files = EMU3_ENTRIES_PER_BLOCK * (info->root_blocks +
						 info->dir_content_blocks);
	buf->f_ffree = emu3_get_free_inodes(sb);
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
		kfree(info->i_maps);
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
	short next = e3i->start_cluster;
	int new, i = 0;

	while (le16_to_cpu(info->cluster_list[next]) != EMU_LAST_FILE_CLUSTER) {
		next = le16_to_cpu(info->cluster_list[next]);
		i++;
	}
	while (i < cluster) {
		new = emu3_next_free_cluster(info);
		if (new < 0)
			return -ENOSPC;
		info->cluster_list[next] = cpu_to_le16(new);
		next = new;
		i++;
	}
	info->cluster_list[next] = cpu_to_le16(EMU_LAST_FILE_CLUSTER);
	return 0;
}

//Base 0 search
int emu3_get_cluster(struct inode *inode, int n)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	short next = e3i->start_cluster;
	int i = 0;

	while (i < n) {
		if (le16_to_cpu(info->cluster_list[next]) ==
		    EMU_LAST_FILE_CLUSTER)
			return -1;
		next = le16_to_cpu(info->cluster_list[next]);
		i++;
	}
	return next;
}

void emu3_init_cluster_list(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);

	info->cluster_list[e3i->start_cluster] =
	    cpu_to_le16(EMU_LAST_FILE_CLUSTER);
}

void emu3_clear_cluster_list(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	short prev, next = e3i->start_cluster;

	while (le16_to_cpu(info->cluster_list[next]) != EMU_LAST_FILE_CLUSTER) {
		prev = next;
		next = le16_to_cpu(info->cluster_list[next]);
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

	next_cluster = le16_to_cpu(info->cluster_list[last_cluster]);
	while (next_cluster != EMU_LAST_FILE_CLUSTER) {
		info->cluster_list[last_cluster] =
		    pruning ? 0 : cpu_to_le16(EMU_LAST_FILE_CLUSTER);
		last_cluster = next_cluster;
		next_cluster = le16_to_cpu(info->cluster_list[last_cluster]);
		pruning = 1;
	}
	if (pruning)
		info->cluster_list[last_cluster] = 0;
}

int emu3_next_free_cluster(struct emu3_sb_info *info)
{
	int i;

	for (i = 1; i < info->clusters; i++)
		if (info->cluster_list[i] == 0)
			return i;
	return -ENOSPC;
}

sector_t emu3_get_phys_block(struct inode *inode, sector_t block)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int cluster = ((int)block) / info->blocks_per_cluster;
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
	struct buffer_head *b;
	int i;

	for (i = 0; i < info->cluster_list_blocks; i++) {
		b = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(b->b_data,
		       &info->cluster_list[EMU3_CLUSTER_ENTRIES_PER_BLOCK * i],
		       EMU3_BSIZE);
		mark_buffer_dirty(b);
		brelse(b);
	}
}

void emu3_read_cluster_list(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *b;
	int i;

	for (i = 0; i < info->cluster_list_blocks; i++) {
		b = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(&info->cluster_list[EMU3_CLUSTER_ENTRIES_PER_BLOCK * i],
		       b->b_data, EMU3_BSIZE);
		brelse(b);
	}
}

static int emu3_fill_super(struct super_block *sb, void *data, int silent,
			   bool emu4)
{
	struct emu3_sb_info *info;
	struct buffer_head *sbh;
	struct buffer_head *b;
	unsigned char *e3sb;
	struct inode *inode;
	int i, j, k, size, err = 0;
	short *block, index;
	struct emu3_dentry *e3d;
	unsigned int *parameters;
	unsigned int root_ino;

	if (sb_set_blocksize(sb, EMU3_BSIZE) != EMU3_BSIZE) {
		printk(KERN_ERR
		       "%s: 512B block size not allowed on this device\n",
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
		printk(KERN_ERR "%s: volume is not an EMU3 disk\n",
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

	printk(KERN_INFO "%s: %d blocks, %d clusters, %d blocks/cluster\n",
	       EMU3_MODULE_NAME, info->blocks, info->clusters,
	       info->blocks_per_cluster);
	printk(KERN_INFO "%s: cluster list start block @ %d + %d blocks\n",
	       EMU3_MODULE_NAME, info->start_cluster_list_block,
	       info->cluster_list_blocks);
	printk(KERN_INFO "%s: root start block @ %d + %d blocks\n",
	       EMU3_MODULE_NAME, info->start_root_block, info->root_blocks);
	printk(KERN_INFO "%s: dir content start block @ %d + %d blocks\n",
	       EMU3_MODULE_NAME, info->start_dir_content_block,
	       info->dir_content_blocks);
	printk(KERN_INFO "%s: data start block @ %d + %d clusters\n",
	       EMU3_MODULE_NAME, info->start_data_block, info->clusters);

	size = sizeof(bool) * info->dir_content_blocks;
	info->dir_content_block_list = kzalloc(size, GFP_KERNEL);
	if (!info->dir_content_block_list) {
		err = -ENOMEM;
		goto out3;
	}
	memset(info->dir_content_block_list, 0, size);

	size = sizeof(unsigned int) * EMU3_TOTAL_ENTRIES(info);
	info->i_maps = kzalloc(size, GFP_KERNEL);
	if (!info->i_maps) {
		err = -ENOMEM;
		goto out4;
	}
	memset(info->i_maps, 0, size);

	sb->s_op = &emu3_super_operations;

	if (emu4)
		root_ino = 1;
	else
		root_ino =
		    emu3_get_or_add_i_map(info, EMU3_DNUM
					  (info->start_root_block, 0));
	inode = emu3_get_inode(sb, root_ino);
	if (!inode) {
		err = -EIO;
		goto out5;
	}

	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		iput(inode);
		err = -ENOMEM;
		goto out5;
	}

	for (i = 0; i < info->root_blocks; i++) {
		b = sb_bread(sb, info->start_root_block + i);

		e3d = (struct emu3_dentry *)b->b_data;

		if (i == 0 && emu3_fix_first_dir_blocks(e3d, info))
			mark_buffer_dirty_inode(b, inode);

		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++) {
			if (!EMU3_DENTRY_IS_DIR(e3d))
				continue;

			block = e3d->data.dattrs.block_list;
			for (k = 0; k < EMU3_BLOCKS_PER_DIR; k++, block++) {
				if (EMU3_IS_DIR_BLOCK_FREE(*block))
					continue;

				index =
				    le16_to_cpu(*block) -
				    info->start_dir_content_block;

				if (index < 0
				    || index >= info->dir_content_blocks) {
					printk(KERN_CRIT
					       "%s: block %d marked as used by dir %.16s\n",
					       EMU3_MODULE_NAME, *block,
					       e3d->name);
					err = -EIO;
					goto out5;
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

 out5:
	kfree(info->dir_content_block_list);
 out4:
	kfree(info->i_maps);
 out3:
	kfree(info->cluster_list);
 out2:
	brelse(sbh);
 out1:
	kfree(info);
	sb->s_fs_info = NULL;
	return err;
}

static int emu3_fill_super_v3(struct super_block *sb, void *data, int silent)
{
	return emu3_fill_super(sb, data, silent, 0);
}

static int emu3_fill_super_v4(struct super_block *sb, void *data, int silent)
{
	return emu3_fill_super(sb, data, silent, 1);
}

static struct dentry *emu3_mount_v3(struct file_system_type *fs_type,
				    int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, emu3_fill_super_v3);
}

static struct dentry *emu3_mount_v4(struct file_system_type *fs_type,
				    int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, emu3_fill_super_v4);
}

static struct file_system_type emu3_fs_type_v3 = {
	.owner = THIS_MODULE,
	.name = "emu3",
	.mount = emu3_mount_v3,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static struct file_system_type emu3_fs_type_v4 = {
	.owner = THIS_MODULE,
	.name = "emu4",
	.mount = emu3_mount_v4,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init emu3_init(void)
{
	int err;

	printk(KERN_INFO "%s: init\n", EMU3_MODULE_NAME);
	err = init_inodecache();
	if (err)
		return err;
	err = register_filesystem(&emu3_fs_type_v3)
	    || register_filesystem(&emu3_fs_type_v4);
	if (err)
		destroy_inodecache();
	return err;
}

static void __exit emu3_exit(void)
{
	unregister_filesystem(&emu3_fs_type_v3);
	unregister_filesystem(&emu3_fs_type_v4);
	destroy_inodecache();
	printk(KERN_INFO "%s: exit\n", EMU3_MODULE_NAME);
}

module_init(emu3_init);
module_exit(emu3_exit);

MODULE_LICENSE("GPL");

MODULE_AUTHOR("David García Goñi <dagargo@gmail.com>");
MODULE_DESCRIPTION("E-Mu EIII filesystem for Linux");
