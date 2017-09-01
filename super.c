/*
 *   super.c
 *   Copyright (C) 2017 David García Goñi <dagargo at gmail dot com>
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

static int emu3_get_free_ids(struct emu3_sb_info *info)
{
	int free_ids = 0;
	int i;

	for (i = 0; i < EMU3_MAX_REGULAR_FILE; i++)
		if (info->id_list[i] == 0)
			free_ids++;
	return free_ids;
}

int emu3_get_free_id(struct emu3_sb_info *info)
{
	int i;

	for (i = 0; i < EMU3_MAX_REGULAR_FILE; i++)
		if (info->id_list[i] == 0)
			return i;
	return -1;
}

static int emu3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

	buf->f_type = EMU3_FS_TYPE;
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = info->clusters * info->blocks_per_cluster;
	buf->f_bfree = emu3_get_free_clusters(info) * info->blocks_per_cluster;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = emu3_get_free_ids(info);
	buf->f_ffree = EMU3_MAX_FILES - buf->f_files;
	buf->f_fsid.val[0] = (u32) id;
	buf->f_fsid.val[1] = (u32) (id >> 32);
	buf->f_namelen = LENGTH_FILENAME;
	return 0;
}

void emu3_mark_as_non_empty(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh1, *bhinfo;
	short int *data;
	short int key;
	int i;

	bhinfo = sb_bread(sb, info->start_info_block);
	data = (short int *)bhinfo->b_data;

	//The 7 short from data[9] on look like a list of used blocks of the directory
	if (data[9] == 0xffff) {
		bh1 = sb_bread(sb, 1);
		key = (short int)((short int *)bh1->b_data)[0]++;
		mark_buffer_dirty(bh1);
		brelse(bh1);
	} else
		key = data[9];

	//We mark them as used always.
	for (i = 0; i < 7; i++)
		data[9 + i] = key + i;

	mark_buffer_dirty(bhinfo);

	brelse(bhinfo);
}

static void emu3_put_super(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);

	mutex_lock(&info->lock);
	emu3_write_cluster_list(sb);
	if (emu3_get_free_ids(info) < EMU3_MAX_REGULAR_FILE)
		emu3_mark_as_non_empty(sb);
	mutex_unlock(&info->lock);

	mutex_destroy(&info->lock);

	if (info) {
		kfree(info->cluster_list);
		kfree(info->id_list);
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

	while (info->cluster_list[next] != cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
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
	info->cluster_list[next] = cpu_to_le16(LAST_CLUSTER_OF_FILE);
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
		    cpu_to_le16(LAST_CLUSTER_OF_FILE))
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
	    cpu_to_le16(LAST_CLUSTER_OF_FILE);
}

void emu3_clear_cluster_list(struct inode *inode)
{
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
void emu3_update_cluster_list(struct inode *inode)
{
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	short int clusters, last_cluster;
	int pruning;

	emu3_get_file_geom(inode, &clusters, NULL, NULL);
	last_cluster = emu3_get_cluster(inode, clusters - 1);
	pruning = 0;
	while (info->cluster_list[last_cluster] !=
	       cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
		int next = info->cluster_list[last_cluster];
		if (pruning)
			info->cluster_list[last_cluster] = 0;
		else
			info->cluster_list[last_cluster] =
			    cpu_to_le16(LAST_CLUSTER_OF_FILE);
		last_cluster = next;
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

sector_t emu3_get_phys_block(struct inode * inode, sector_t block)
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
		       &info->cluster_list[EMU3_CENTRIES_PER_BLOCK * i],
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
		memcpy(&info->cluster_list[EMU3_CENTRIES_PER_BLOCK * i],
		       bh->b_data, EMU3_BSIZE);
		brelse(bh);
	}
}

static int emu3_fill_super(struct super_block *sb, void *data, int silent)
{
	struct emu3_sb_info *info;
	struct buffer_head *sbh;
	unsigned char *e3sb;
	struct inode *inode;
	int i, j, size, err = 0;
	unsigned int *parameters;
	struct emu3_dentry *e3d;
	struct buffer_head *bh;

	if (sb_set_blocksize(sb, EMU3_BSIZE) != EMU3_BSIZE) {
		printk(KERN_ERR
		       "%s: impossible to mount. Linux does not allow 512B block size on this device.",
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
		printk(KERN_ERR "%s: volume is not an EMU3 disk.",
		       EMU3_MODULE_NAME);
		err = -EINVAL;
		goto out2;
	}

	parameters = (unsigned int *)e3sb;

	info->blocks = cpu_to_le32(parameters[1]);	//TODO: add 1 ??? Do we really use this?
	info->start_info_block = cpu_to_le32(parameters[2]);
	info->info_blocks = cpu_to_le32(parameters[3]);
	info->start_root_dir_block = cpu_to_le32(parameters[4]);
	info->root_dir_blocks = cpu_to_le32(parameters[5]);
	info->start_cluster_list_block = cpu_to_le32(parameters[6]);
	info->cluster_list_blocks = cpu_to_le32(parameters[7]);
	info->start_data_block = cpu_to_le32(parameters[8]);
	info->blocks_per_cluster = (0x10000 << (e3sb[0x28] - 1)) / EMU3_BSIZE;
	info->clusters = parameters[9] / (e3sb[0x28] >= 5 ? 2 : 1);

	//Now it's time to read the cluster list...
	size = EMU3_BSIZE * info->cluster_list_blocks;
	info->cluster_list = kzalloc(size, GFP_KERNEL);

	if (!info->cluster_list) {
		err = -ENOMEM;
		goto out2;
	}

	emu3_read_cluster_list(sb);

	//... and the inode id list.
	size = sizeof(int) * EMU3_MAX_REGULAR_FILE;
	info->id_list = kzalloc(size, GFP_KERNEL);

	if (!info->id_list) {
		err = -ENOMEM;
		goto out3;
	}
	//We need to map the used inodes
	for (i = 0; i < info->root_dir_blocks; i++) {
		bh = sb_bread(sb, info->start_root_dir_block + i);

		e3d = (struct emu3_dentry *)bh->b_data;

		for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
			//We only map the regular files
			if (IS_EMU3_FILE(e3d)) {
				if (e3d->type != FTYPE_DEL
				    && e3d->id < EMU3_MAX_REGULAR_FILE)
					info->id_list[e3d->id] = 1;
				else
					info->id_list[e3d->id] = 0;
			}
			e3d++;
		}
		brelse(bh);
	}

	printk("%s: %d blocks, %d clusters, b/c %d.\n", EMU3_MODULE_NAME,
	       info->blocks, info->clusters, info->blocks_per_cluster);
	printk("%s: info init block @ %d + %d blocks.\n", EMU3_MODULE_NAME,
	       info->start_info_block, info->info_blocks);
	printk("%s: cluster list init block @ %d + %d blocks.\n",
	       EMU3_MODULE_NAME, info->start_cluster_list_block,
	       info->cluster_list_blocks);
	printk("%s: root init block @ %d + %d blocks.\n", EMU3_MODULE_NAME,
	       info->start_root_dir_block, info->root_dir_blocks);
	printk("%s: data init block @ %d + %d clusters.\n", EMU3_MODULE_NAME,
	       info->start_data_block, info->clusters);

	sb->s_op = &emu3_super_operations;

	inode = emu3_get_inode(sb, ROOT_DIR_INODE_ID);

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

	if (!err) {
		mutex_init(&info->lock);
		brelse(sbh);
		return 0;
	}

 out4:
	kfree(info->id_list);
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

	printk(KERN_INFO "Init %s.\n", EMU3_MODULE_NAME);
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
	printk(KERN_INFO "Exit %s.\n", EMU3_MODULE_NAME);
}

module_init(emu3_init);
module_exit(emu3_exit);

MODULE_LICENSE("GPL");

MODULE_AUTHOR("David García Goñi <dagargo at gmail dot com>");
MODULE_DESCRIPTION("E-mu E3 sampler family filesystem for Linux");
