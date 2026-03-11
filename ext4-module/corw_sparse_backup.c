#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mbcache.h>
#include <linux/quotaops.h>
#include <linux/iversion.h>
#include <linux/backing-dev.h>
#include "ext4_jbd2.h"
#include "ext4.h"
#include "ext4_extents.h"
#include "xattr.h"
#include "acl.h"


#include "corw_sparse.h"

#include <linux/fdtable.h>
#include <linux/rcupdate.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/vmalloc.h>

#include <linux/memcontrol.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/writeback.h>
#include <linux/vmstat.h>




//Extended attributes names 
//const int CHILD_NAME_LEN = 16;
//const int CHILD_RANGE_LEN = 16;
char *scorw_child = "c_";       //Fill child num using sprintf. Eg: c_0, c_1, ... , c_255
char *scorw_range_start = "r_start_";   //Fill range using sprintf. Eg: r_0, r_1, ... , r_3
char *scorw_range_end = "r_end_";       //Fill range using sprintf. Eg: r_0, r_1, ... , r_3
char *scorw_child_frnd = "SCORW_CHILD"; //attribute maintained in friend file
char *scorw_parent = "SCORW_PARENT";
char *scorw_friend = "SCORW_FRIEND";
char *copy_size = "COPY_SIZE";
char *blocks_to_copy = "BLOCKS_TO_COPY";
char *num_ranges = "NUM_RANGES";        //How many ranges have been supplied to child?
char *version = "v";
int scorw_create_zero_page(struct inode *inode, unsigned lblk);

extern wait_queue_head_t sync_child_wait_queue;
extern void ext4_es_print_tree(struct inode *inode);
extern void ext4_ext_put_gap_in_cache(struct inode *inode, ext4_lblk_t hole_start, ext4_lblk_t hole_len);
extern unsigned long (*get_child_inode_num)(struct inode *);
extern int (*is_par_inode)(struct inode*, int );
extern int (*is_child_inode)(struct inode*, int );
extern unsigned long (*get_child_i_attr_val)(struct inode *, int );
extern unsigned long (*get_friend_attr_val)(struct inode *);
extern int (*is_block_copied)(struct inode *, unsigned );
extern struct page_copy *(*find_page_copy)(unsigned block_num, unsigned long par_ino_num, int child_index);
extern void (*__scorw_exit)(void);
extern int (*is_child_file)(struct inode* inode, int consult_extended_attributes);
extern int (*is_par_file)(struct inode* inode, int consult_extended_attributes);
extern int (*unlink_child_file)(struct inode *inode);
extern int (*unlink_par_file)(struct inode *inode);


void scorw_inc_process_usage_count(struct scorw_inode *scorw_inode)
{
        //printk("scorw_inc_process_usage_count called\n");

        //printk("scorw_inc_process_usage_count: Before usage_count: %d\n", scorw_inode->i_usage_count);
        //atomic64_inc(&(scorw_inode->i_process_usage_count));
        //printk("scorw_inc_process_usage_count: After usage_count: %d\n", scorw_inode->i_usage_count);
        spin_lock(&(scorw_inode->i_process_usage_count_lock));
        ++(scorw_inode->i_process_usage_count);
        spin_unlock(&(scorw_inode->i_process_usage_count_lock));

}



void special_open(struct inode *par_inode, struct inode *child_inode, int index)
{
	int i = 0;
	int n = 0;
	struct scorw_inode *par_scorw_inode = 0;
	struct scorw_inode *child_scorw_inode = 0;

	//printk("Inside %s(). par: %lu, child: %lu, index: %d\n", __func__, par_inode->i_ino, child_inode->i_ino, index);
	//par exists?
	//read_lock(&scorw_lock);
	//par_scorw_inode = scorw_search_inode_list(par_inode->i_ino);
	//read_unlock(&scorw_lock);
	par_scorw_inode = par_inode->i_scorw_inode;
	if(par_scorw_inode)
	{
		//create child scorw inode
		//printk("%s(): par: %lu scorw exists\n", __func__, par_inode->i_ino);
		child_scorw_inode = scorw_get_inode(child_inode, 1, 0);
		if(child_scorw_inode)
		{
			//attach child scorw inode to par
			//printk("%s(): child: %lu is open\n", __func__, child_inode->i_ino);
			down_write(&(par_scorw_inode->i_lock));
			par_scorw_inode->i_child_scorw_inode[index] = child_scorw_inode;
			child_scorw_inode->i_at_index = index;
			if(index > par_scorw_inode->i_last_child_index)
			{
				par_scorw_inode->i_last_child_index = index;
			}
			up_write(&(par_scorw_inode->i_lock));
			//printk("%s(): Attached child: %lu to par: %lu at index: %d\n", __func__, child_inode->i_ino, par_inode->i_ino, index);

			//Our setxattr utility is as following:
			//	open(child)
			//	open(parent)
			//	setxattr()	//create parent child relationship. Inside this we call get_inode to create child scorw inode and attach this scorw inode to parent
			//	close(child)	//However, due to this close(), child scorw inode that we just created, will get closed.
			//			//Hence, we are making an additional get_inode call that will complement this close i.e. put_inode
			//			//and first get_inode will be closed when close(parent) is called.
			//	close(parent)
			scorw_get_inode(child_inode, 1, 0);
		}
		else
		{
			//Par exists but child doesn't exists
			BUG_ON(par_scorw_inode);
		}
	}
	else
	{
		//printk("%s(): par: %lu scorw does not exists. Parent open() count: %d\n", __func__, par_inode->i_ino, atomic_read(&(par_inode->i_vfs_inode_open_count)));
		if(atomic_read(&(par_inode->i_vfs_inode_open_count)) > 1)
		{
			//printk("%s(): par: %lu scorw does not exists. But par file is open.\n", __func__, par_inode->i_ino);

			//This lock is meant to freeze the existance of par scorw inode i.e. prevent opening/closing of par scorw inode in parallel
			//Eventhough we are unlocking it here (because get_inode will need to acquire this lock), par scorw inode creation, removal is still
			//frozen due to i_vfs_inode_open_close_lock lock
			//
			mutex_unlock(&(par_inode->i_vfs_inode_lock));
			n = atomic_read(&(par_inode->i_vfs_inode_open_count));
			//open par scorw inode as many times as par file is open
			for(i=0 ; i < n; i++)
			{
				scorw_get_inode(par_inode, 0, 0);
			}
			mutex_lock(&(par_inode->i_vfs_inode_lock));

			//Our setxattr utility is as following:
			//	open(child)
			//	open(parent)
			//	setxattr()	//create parent child relationship. Inside this we call get_inode to create par scorw inode and its children's scorw inodes
			//	close(child)	//However, due to this close(), child scorw inode that we just created, will get closed.
			//			//Hence, we are making an additional get_inode call for child that will complement this close i.e. put_inode
			//	close(parent)
			scorw_get_inode(child_inode, 1, 0);
		}
		else
		{
			//printk("%s(): par: %lu scorw does not exists and par file is not open. Doing nothing.\n", __func__, par_inode->i_ino);
		}
	}
}

int scorw_init_friend_file(struct inode* frnd_inode, long long child_copy_size)
{
        int i = 0;
        unsigned total_pages = (((child_copy_size % (PAGE_BLOCKS << PAGE_SHIFT)) == 0) ? (child_copy_size / (PAGE_BLOCKS << PAGE_SHIFT)) : ((child_copy_size / (PAGE_BLOCKS << PAGE_SHIFT))+1));
        long long frnd_size = frnd_inode->i_size;

        //Build frnd file
        if(frnd_size == 0)
        {
                for(i = 0; i < total_pages; i++)
                {
                        scorw_create_zero_page(frnd_inode, i);
                }
        }
        return 0;
}

int scorw_create_zero_page(struct inode *inode, unsigned lblk)
{
        struct address_space *mapping = NULL;
        struct page *page_w = NULL;
        void *fsdata = 0;
        int error = 0;
	void *kaddr_w;
	int i;

	if(get_child_inode_num)
	{
		down_read(&(inode->i_scorw_rwsem));
	}
	mapping = inode->i_mapping;
	//printk("scorw_copy_zero_page: Inside scorw_copy_zero_page, lblk: %u\n", lblk);

	error = ext4_da_write_begin(NULL, mapping, ((unsigned long)lblk) << PAGE_SHIFT, PAGE_SIZE, 0, &page_w, &fsdata);
	//printk("################# Page allocated but not mapped. page_w->_refcount: %d, page_w->_mapcount: %d\n",  page_ref_count(page_w), atomic_read(&page_w->_mapcount));
	if(page_w != NULL)
	{
		kaddr_w = kmap_atomic(page_w);
		//printk("################# Page allocated and mapped. page_w->_refcount: %d, page_w->_mapcount: %d\n", page_ref_count(page_w),atomic_read(&page_w->_mapcount));

		for(i = 0; i < PAGE_SIZE; i++)
		{
			*((char*)kaddr_w + i) = '\0';
		}
		kunmap_atomic(kaddr_w);
		//printk("################# Page allocated and unmapped mapped. page_w->_refcount: %d, page_w->_mapcount: %d\n",  page_ref_count(page_w),atomic_read(&page_w->_mapcount));

		//ext4_es_print_tree(scorw_inode->i_vfs_inode);
		ext4_da_write_end(NULL, mapping , ((unsigned long)lblk) << PAGE_SHIFT, PAGE_SIZE, PAGE_SIZE, page_w, fsdata);
		//printk("1. scorw_copy_zero_page: size of frnd file as per its inode: %lu\n", inode->i_size);

		//There are only few pages in friend file. So, we can skip ratelimiting it.
		//This is done to avoid acquiring scorw_lock recursively. Read comment inside scorw_is_par_file() to know more.
		//balance_dirty_pages_ratelimited(mapping);
		//printk("2. scorw_copy_zero_page: size of frnd file as per its inode: %lu\n", inode->i_size);

		if(get_child_inode_num)
		{
			up_read(&(inode->i_scorw_rwsem));
		}

	}
	else
	{
		printk("scorw_create_zero_page: Error: %d in ext4_da_write_begin\n", error);
		if(get_child_inode_num)
		{
			up_read(&(inode->i_scorw_rwsem));
		}

		return error;
	}
	return 0;
}

struct scorw_inode* scorw_get_inode(struct inode *inode, int is_child_inode, int new_sparse)
{
	int i;
	unsigned long c_ino_num;
        struct scorw_inode *scorw_inode;
        struct scorw_inode *c_scorw_inode;
	struct inode *c_inode;


	//printk("1.Inside scorw_get_inode, inode: %lu, is_child_inode: %d\n", inode->i_ino, is_child_inode);
	mutex_lock(&(inode->i_vfs_inode_lock));

	//read_lock(&scorw_lock);
	//scorw_inode = scorw_search_inode_list(inode->i_ino);
	//read_unlock(&scorw_lock);
	scorw_inode = inode->i_scorw_inode;

	if(scorw_inode != NULL)
	{
		//If a child inode has been unlinked, scorw_get_inode can be called on it only when par is being opened.
		//Since, child is unlinked, don't open it anymore and don't attach the scorw inode of child
		//to par.
		if(is_child_inode && scorw_inode->i_ino_unlinked)
		{
			//printk("scorw_get_inode, child inode: %lu has been unlinked. Won't be attached with par\n", inode->i_ino);
			mutex_unlock(&(inode->i_vfs_inode_lock));
			return 0;
		}
		//printk("2.scorw_get_inode: Already there is a scorw inode corresponding inode: %lu\n", inode->i_ino);
		//increment usage count of already existing scorw inode
		scorw_inc_process_usage_count(scorw_inode);
	}
	else
	{

		//printk("3.scorw_get_inode: No. Already there is not a scorw inode corresponding inode: %lu\n", inode->i_ino);
		//printk("4.scorw_get_inode: Inserting new scorw inode\n");

		scorw_inode = scorw_alloc_inode();
		if(scorw_inode != NULL)
		{
			init_rwsem(&(scorw_inode->i_lock));
			spin_lock_init(&(scorw_inode->i_process_usage_count_lock));
			scorw_inode->added_to_page_copy = 0;
			scorw_inode->removed_from_page_copy = 0;
			if(is_child_inode)
			{
				//printk("5.scorw_get_inode: Calling scorw_prepare_child_inode\n");
				scorw_prepare_child_inode(scorw_inode, inode, new_sparse);
				if(scorw_thread)
				{
					//increment usage count of scorw inode on the behalf of kernel thread
					//printk("scorw_get_inode: Incrementing usage count\n");
					scorw_inc_thread_usage_count(scorw_inode);

					//Increase ref count of inode. This will be decreased by thread when
					//file has been deleted. If file has been deleted, scorw inode and vfs inode both should
					//exist so that if thread had already selected this scorw inode to process, then it can complete
					//that process and then stop using it,
					ext4_iget(inode->i_sb, inode->i_ino, EXT4_IGET_NORMAL);

					//do all changes needed for inode policy to work as required when a new scorw inode is added.
					inode_policy->inode_added(inode_policy, scorw_inode);

				}

			}
			else
			{
				//printk("6.scorw_get_inode: Calling scorw_prepare_par_inode\n");
				scorw_prepare_par_inode(scorw_inode, inode);
			}

			//increment usage count of scorw inode created
			//This refers to the usage on behalf of process of which this scorw inode is getting created
			//printk("7.scorw_get_inode: Incrementing usage count\n");
			scorw_inc_process_usage_count(scorw_inode);

			//attach newly created scorw inode to the corresponding vfs inode
			inode->i_scorw_inode = scorw_inode;

			if(!is_child_inode)
			{
				//printk("scorw_get_inode: Parent will create scorw inodes of children now\n");
				for(i = 0; i < SCORW_MAX_CHILDS; i++)
				{
					c_ino_num = scorw_get_child_i_attr_val(inode, i);
					if(c_ino_num)
					{
						//printk("scorw_get_inode: child %d, inode num: %lu\n", i, c_ino_num);
						c_inode = ext4_iget(inode->i_sb, c_ino_num, EXT4_IGET_NORMAL);
						c_scorw_inode = scorw_get_inode(c_inode, 1, 0);
						//printk("scorw_get_inode: scorw inode of child %d, inode num: %lu, got created\n", i, c_ino_num);
						scorw_inode->i_child_scorw_inode[i] = c_scorw_inode;
						if(c_scorw_inode)
						{
							c_scorw_inode->i_at_index = i;
						}
						scorw_inode->i_last_child_index = i;
						iput(c_inode);
					}
					else
					{
						scorw_inode->i_child_scorw_inode[i] = NULL;
					}
				}
			}

			//add scorw inode into list of scorw inodes
			//printk("8.scorw_get_inode: Adding to inode list\n");
			//This lock is required to prevent search operations from occuring in parallel while the insertion of a new
			//node takes place in the list
			//
			//This list is useful when scorw copy thread has to pick an scorw inode to process
			write_lock(&scorw_lock);
			scorw_add_inode_list(scorw_inode);
			write_unlock(&scorw_lock);
		}
		else
		{
			mutex_unlock(&(inode->i_vfs_inode_lock));
			printk(KERN_ERR "scorw_get_inode: SCORW_OUT_OF_MEMORY: Releasing vfs inode lock\n");
			return 0;
		}
	}
	//printk("scorw_get_inode: releasing vfs inode lock\n");
	mutex_unlock(&(inode->i_vfs_inode_lock));

	//printk("9.scorw_get_inode: returning\n");

	return scorw_inode;
}
