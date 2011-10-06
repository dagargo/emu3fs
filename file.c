#include "emu3_fs.h"

//This does not work on non 512B block devices.
//TODO: http://thecoffeedesk.com/geocities/rkfs.html see PAGE_SIZE
static int emu3_get_block(struct inode *inode, sector_t block,
			struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct emu3_inode * e3i = EMU3_I(inode);

	if (block < e3i->blocks) {
		if (!create) { //TODO: what is this for?
			map_bh(bh_result, sb, e3i->start_block + block);
			return 0;
		}
	}
	
	return -ENOSPC;
}

static int emu3_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, emu3_get_block);
}

const struct file_operations emu3_file_operations_file = {
	.llseek 	 = generic_file_llseek,
	.read		 = do_sync_read,
	.aio_read	 = generic_file_aio_read,
	.write		 = NULL, //do_sync_write,
	.aio_write	 = NULL, //generic_file_aio_write,
	.mmap		 = generic_file_mmap,
	.splice_read = generic_file_splice_read,
};

const struct inode_operations emu3_inode_operations_file = {
	.create			= NULL,
	.lookup			= NULL,
	.link			= NULL,
	.unlink			= NULL,
	.rename			= NULL,
};

const struct address_space_operations emu3_aops = {
	.readpage	 = emu3_readpage,
	.writepage	 = NULL, //emu3_writepage,
	.sync_page	 = NULL, //block_sync_page,
	.write_begin = NULL, //emu3_write_begin,
	.write_end	 = NULL, //generic_write_end,
	//TODO: release page?
};
