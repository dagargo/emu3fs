#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/vfs.h>

#define EMU3_MODULE_NAME "E-mu E3 filesystem module"

#define EMU3_ERROR_MSG "E-mu E3 error: "

#define EMU3_FS_SIGNATURE "EMU3"
#define EMU3_FS_TYPE 0x454d5533

#define EMU3_BSIZE 0x200

//TODO: why can't this be used as a left assignment operator?
#define EMU3_SB(sb) ((struct emu3_sb_info *)sb->s_fs_info)

#define EMU3_I(inode) ((struct emu3_inode *)container_of(inode, struct emu3_inode, vfs_inode))

#define EMU3_I_ID(e3d) ((e3d->id) + 1)

#define EMU3_MAX_FILES (EMU3_MAX_REGULAR_FILE + 2) //100 regular banks + 2 special rom files

#define EMU3_MAX_REGULAR_FILE 100

#define MAX_ENTRIES_PER_BLOCK 16

#define ROOT_DIR_INODE_ID 1024 //Any value is valid as long as is greater than the highest inode id.

#define MAX_LENGTH_FILENAME 16

#define IS_EMU3_FILE(e3d) ((e3d->clusters > 0) && (e3d->props[2] == 0x81 || e3d->props[2] == 0x80))

//TODO: change to int
struct emu3_sb_info {
	unsigned long start_root_dir_block;
	unsigned long start_data_block;
	unsigned long info_block;
	unsigned long blocks_per_cluster;
	unsigned long clusters;
	unsigned long blocks;
	unsigned long used_inodes;
	unsigned long next_available_cluster;
	//TODO: inode map?
};

struct emu3_dentry {
	char name[16];
	unsigned char unknown;
	unsigned char id;
	unsigned short start_cluster;
	unsigned short clusters;
	unsigned short blocks;
	unsigned char props[8];
};

struct emu3_inode {
	unsigned long start_block;
	unsigned long blocks;
	struct inode vfs_inode;
};

extern const struct file_operations emu3_file_operations_dir;

extern const struct inode_operations emu3_inode_operations_dir;

extern const struct file_operations emu3_file_operations_file;

extern const struct inode_operations emu3_inode_operations_file;

extern const struct address_space_operations emu3_aops;

struct inode * emu3_iget(struct super_block *, unsigned long);
