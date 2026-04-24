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
#include "truncate.h"

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
#include <linux/hashtable.h>

#include <linux/iomap.h>
#include <linux/timekeeping.h>
#include <linux/crc32c.h>

/*
 * ============================================================
 * CRASH INJECTION HOOKS FOR TESTING CRASH CONSISTENCY
 * ============================================================
 * These module parameters allow controlled simulation of torn
 * writes at specific points in the write path.
 *
 * Usage (as root on the VM):
 *
 *   Scenario A — Log hits disk, parent data does NOT:
 *     echo 1 > /sys/module/ext4/parameters/scorw_crash_after_log_write
 *     <perform a write to parent> → system panics → remount → check data is OLD
 *
 *   Scenario B — Parent data hits disk, log does NOT:
 *     echo 1 > /sys/module/ext4/parameters/scorw_crash_after_data_write
 *     <perform a write to parent> → system panics → remount → check data is NEW
 *     (the unreferenced appended block is garbage-collected harmlessly)
 *
 * Reset before normal use:
 *     echo 0 > /sys/module/ext4/parameters/scorw_crash_after_log_write
 *     echo 0 > /sys/module/ext4/parameters/scorw_crash_after_data_write
 * ============================================================
 */
static int scorw_crash_after_log_write  = 0;
static int scorw_crash_after_data_write = 0;
static int scorw_crash_with_bad_crc     = 0;
module_param(scorw_crash_after_log_write,  int, 0644);
module_param(scorw_crash_after_data_write, int, 0644);
module_param(scorw_crash_with_bad_crc,     int, 0644);
MODULE_PARM_DESC(scorw_crash_after_log_write,
	"[TEST] Flush log then panic before flushing parent data (Scenario A)");
MODULE_PARM_DESC(scorw_crash_after_data_write,
	"[TEST] Flush parent data then panic before flushing log (Scenario B)");
MODULE_PARM_DESC(scorw_crash_with_bad_crc,
	"[TEST] Write bad CRC, flush both log+data, then panic (Scenario C)");

enum batching_type
{
	NOT_STARTED,
	PRESENT_IN_CHILD,
	PRESENT_IN_PARENT
};

//Extended attributes names 
//const int CHILD_NAME_LEN = 16;
//const int CHILD_RANGE_LEN = 16;
LIST_HEAD(scorw_inodes_list);
LIST_HEAD(page_copy_llist);     //page copy linked list
LIST_HEAD(pending_frnd_version_cnt_inc_list);   //list of frnd inodes waiting for updation of their version count
DEFINE_MUTEX(frnd_version_cnt_lock);            //lock that protects above list

//MAHA_AARSH: list of log inodes whose dirty pages need to be synced at unmount
struct pending_log_sync {
	struct inode *log_inode;
	struct list_head list;
};
LIST_HEAD(pending_log_sync_list);
DEFINE_MUTEX(log_sync_list_lock);

DEFINE_RWLOCK(page_copy_lock);  //read-write spinlock. Protects list, hashtable of page copy structs
DEFINE_HASHTABLE(page_copy_hlist, HASH_TABLE_SIZE_BITS_2);	//page copy hash list
DECLARE_WAIT_QUEUE_HEAD(page_copy_thread_wq);	//page copy thread wait queue

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
//MAHA_AARSH_VERSION_start
char *curr_version = "curr_version";
char *par_org = "PAR_ORGY";
char *see_thru_ro = "SEE_THRU_RO";
char *last_open_time = "LAST_OPEN_TIME";
char *scorw_log = "SCORW_LOG";
//MAHA_AARSH_VERSION_end
unsigned long scorw_get_process_usage_count(struct scorw_inode *scorw_inode);
static int scorw_is_in_range(struct scorw_inode *scorw_inode, unsigned blk_num);
void scorw_dec_process_usage_count(struct scorw_inode *scorw_inode);
int scorw_create_zero_page(struct inode *inode, unsigned lblk);
struct page_copy *scorw_get_page_copy_to_process(void);
int scorw_copy_page(struct scorw_inode *scorw_inode, loff_t lblk, struct address_space *p_mapping);
void scorw_set_child_version_attr_val(struct inode *inode, unsigned long version_val);
int scorw_copy_page_from_page_copy(struct page_copy *page_copy, struct scorw_inode *scorw_inode);
int scorw_page_copy_thread_init(void);
int scorw_page_copy_thread_exit(void);
int scorw_page_copy_thread_fn(void *arg);
void scorw_remove_uncopied_blocks_list(struct uncopied_block* uncopied_block);
void scorw_free_all_page_copy(void);
int scorw_is_inode_dirty(struct inode* inode);
int scorw_prepare_page_copy(unsigned block_num, struct scorw_inode *par_scorw_inode, struct page_copy *page_copy, int is_4KB_write, struct page* par_data_page, unsigned char *is_target_child);
int scorw_copy_page_to_page_copy(struct page_copy *page_copy, int is_4KB_write, struct page* par_data_page);
void scorw_add_page_copy_llist(struct page_copy *page_copy);
void scorw_add_page_copy_hlist(struct page_copy *page_copy);
void scorw_read_barrier_begin(struct scorw_inode *p_scorw_inode, unsigned block_num, struct uncopied_block **uncopied_block);
void scorw_read_barrier_end(struct scorw_inode *p_scorw_inode, unsigned block_num, struct uncopied_block **uncopied_block);
void scorw_free_page_copy(struct page_copy *page_copy);
struct page_copy *scorw_find_page_copy(unsigned block_num, unsigned long par_ino_num, int child_index);
int scorw_put_inode(struct inode *inode, int is_child_inode, int is_thread_putting, int is_par_putting);
struct scorw_inode* scorw_find_inode(struct inode *inode);
int scorw_set_page_dirty(struct page *page);
struct page_copy *scorw_alloc_page_copy(void);
#ifndef USE_OLD_RANGE
//Returns the snapx write behavior for a child inode
static int snapx_get_range_info(struct child_range **cr, struct scorw_inode *scorw_inode, unsigned blk_num);
#endif
#ifdef USE_OLD_RANGE
int scorw_write_child_blocks_end(struct inode* inode, loff_t offset, size_t len, struct uncopied_block *uncopied_block);
#else
int scorw_write_child_blocks_end(struct inode* inode, loff_t offset, size_t len, struct uncopied_block *uncopied_block, bool shared);
#endif
void scorw_child_version_cnt_inc(struct inode *inode);
int scorw_write_par_blocks(struct inode* inode, loff_t offset, size_t len, struct page* par_data_page);
int scorw_remove_uncopied_block(struct scorw_inode *scorw_inode, unsigned key, struct uncopied_block *ptr_uncopied_block);
int scorw_put_uncopied_block(struct scorw_inode *scorw_inode, unsigned key, int processing_type, struct uncopied_block* ptr_uncopied_block);
void scorw_inc_process_usage_count(struct scorw_inode *scorw_inode);
int scorw_get_num_ranges_attr_val(struct inode *inode);
void scorw_inc_thread_usage_count(struct scorw_inode *scorw_inode);
void scorw_process_pending_frnd_version_cnt_inc_list(int sync_child);
void scorw_process_pending_log_sync_list(void);
void scorw_init(void);
void scorw_exit(void);
void scorw_remove_page_copy_hlist(struct page_copy *page_copy);
void scorw_unprepare_page_copy(struct page_copy *page_copy);
struct page* scorw_get_page(struct inode* inode, loff_t lblk);
unsigned long scorw_get_frnd_version_attr_val(struct inode *inode);
int scorw_unlink_par_file(struct inode *inode);
int scorw_unlink_child_file(struct inode *inode);
void scorw_set_block_copied(struct scorw_inode* scorw_inode, unsigned blk_num);
void scorw_free_uncopied_block(struct uncopied_block *uncopied_block);
ssize_t scorw_follow_on_read_child_blocks(struct inode* inode, struct kiocb *iocb, struct iov_iter *to);
int scorw_is_child_file(struct inode* inode, int consult_extended_attributes);
unsigned long scorw_get_friend_attr_val(struct inode *inode);
unsigned long scorw_get_child_version_attr_val(struct inode *inode);
int scorw_get_range_attr_val(struct inode *inode, int index, struct child_range_xattr *crx);
unsigned scorw_get_range_i_end_attr_val(struct inode *inode, int index);
unsigned scorw_get_blocks_to_copy_attr_val(struct inode *inode);
struct scorw_inode *scorw_search_inode_list(unsigned long ino_num);

#ifdef USE_OLD_RANGE
int scorw_write_child_blocks_begin(struct inode* inode, loff_t offset, size_t len, void **ptr_uncopied_block);
#else
int scorw_write_child_blocks_begin(struct inode* inode, loff_t offset, size_t len, void **ptr_uncopied_block, struct sharing_range_info *shr_info);
#endif

struct uncopied_block* scorw_get_uncopied_block(struct scorw_inode *scorw_inode, unsigned block_num, int processing_type);
struct uncopied_block *scorw_find_uncopied_block_list(struct scorw_inode *scorw_inode, unsigned key);
int scorw_is_compatible_processing_type(int pt1, int pt2);
void scorw_remove_child_i_attr(struct inode *inode, int child_i);
void scorw_set_block_copied_8bytes(struct inode* frnd_inode, unsigned long start_blk, unsigned long long value_8bytes);
struct scorw_inode *scorw_alloc_inode(void);
void scorw_free_inode(struct scorw_inode *scorw_inode);
int scorw_is_block_copied(struct inode *inode, unsigned blk_num);
unsigned long long scorw_min(unsigned long long a, unsigned long long b);
ssize_t scorw_read_from_child(struct kiocb *iocb, struct iov_iter *to, unsigned batch_start_blk, unsigned batch_end_blk);
void scorw_set_blocks_to_copy_attr_val(struct inode *inode, unsigned blocks_count);
void scorw_prepare_par_inode(struct scorw_inode *scorw_inode, struct inode *vfs_inode);
ssize_t scorw_read_from_parent(struct scorw_inode *scorw_inode, struct kiocb *iocb, struct iov_iter *to, unsigned batch_start_blk, unsigned batch_end_blk);
void scorw_add_inode_list(struct scorw_inode *scorw_inode);
void scorw_set_frnd_version_attr_val(struct inode *inode, unsigned long version_val);
void scorw_remove_inode_list(struct scorw_inode *scorw_inode);
unsigned long scorw_get_child_i_attr_val(struct inode *inode, int child_i);
void scorw_unprepare_par_inode(struct scorw_inode *scorw_inode);
struct pending_frnd_version_cnt_inc *scorw_alloc_pending_frnd_version_cnt_inc(void);
void scorw_free_pending_frnd_version_cnt_inc(struct pending_frnd_version_cnt_inc *pending_frnd_version_cnt_inc);
void scorw_add_pending_frnd_version_cnt_inc_list(struct pending_frnd_version_cnt_inc *pending_frnd_version_cnt_inc);
void scorw_remove_pending_frnd_version_cnt_inc_list(struct pending_frnd_version_cnt_inc *pending_frnd_version_cnt_inc);
struct uncopied_block *scorw_alloc_uncopied_block(void);
struct wait_for_commit *scorw_alloc_wait_for_commit(void);
void scorw_set_bit(u8 bitnum, volatile u8 *p);
void scorw_unprepare_child_inode(struct scorw_inode *scorw_inode);
void scorw_put_page(struct page* page);
unsigned scorw_get_range_i_start_attr_val(struct inode *inode, int index);
void scorw_add_uncopied_blocks_list(struct scorw_inode *scorw_inode, struct uncopied_block *uncopied_block);
int scorw_ext_precache_depth0(struct inode* child_inode);
ssize_t submit_read_request(struct scorw_inode* scorw_inode, struct kiocb *iocb, struct iov_iter *to, enum batching_type prev_batching_type, unsigned batch_start_blk, unsigned batch_end_blk);
void scorw_dec_yet_to_copy_blocks_count(struct scorw_inode* scorw_inode, unsigned n);
int scorw_rebuild_friend_file(struct inode* child_inode, struct inode* frnd_inode);
void scorw_child_copy_completion_cleanup(struct scorw_inode *scorw_inode);
void scorw_remove_page_copy_llist(struct page_copy *page_copy);
int scorw_recover_friend_file(struct inode* child_inode, struct inode* frnd_inode, long long child_copy_size);
int scorw_is_par_file(struct inode* inode, int consult_extended_attributes);
void scorw_add_wait_for_commit_list(struct wait_for_commit *wait_for_commit);
long long scorw_get_copy_size_attr_val(struct inode *inode);
int scorw_is_child_inode(struct scorw_inode *scorw_inode);
unsigned long scorw_get_parent_attr_val(struct inode *inode);
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

static unsigned long num_files_recovered = 0;

//global vars
int async_copy_on = 1;
int use_follow_on_read = 1;
int exiting = 0;
struct task_struct *scorw_thread = 0;
struct task_struct *page_copy_thread = 0;
struct inode_policy *inode_policy = 0;
struct extent_policy *extent_policy = 0;
struct kobject *scorw_sysfs_kobject = 0;


unsigned long long last_recovery_time_us = 0;
int stop_page_copy_thread = 0;
int par_creation_in_progress = 0;
int page_copy_thread_running = 0;
struct kmem_cache *page_copy_slab_cache = 0;

//Friend file rebuilding related sysfs attribute and functions
int enable_recovery = 0;
struct kobject *kobj_scorw=0;

//Enable/Disable recovery of frnd file
static ssize_t  sysfs_show_enable_recovery(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
static ssize_t  sysfs_store_enable_recovery(struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count);
struct kobj_attribute frnd_file_enable_recovery_attr = __ATTR(enable_recovery, 0660, sysfs_show_enable_recovery, sysfs_store_enable_recovery);

static ssize_t  sysfs_show_enable_recovery(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d", enable_recovery);
}

static ssize_t  sysfs_store_enable_recovery(struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count)
{
	sscanf(buf, "%d", &enable_recovery);
	enable_recovery = !!enable_recovery;
	return count;
}

//Fetch time taken (in ms) to recover friend file during last open() of any child file
static ssize_t  sysfs_show_last_recovery_time_us(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
static ssize_t  sysfs_store_last_recovery_time_us(struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count);
struct kobj_attribute frnd_file_last_recovery_time_us_attr = __ATTR(last_recovery_time_us, 0440, sysfs_show_last_recovery_time_us, sysfs_store_last_recovery_time_us);

static ssize_t  sysfs_show_last_recovery_time_us(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu", last_recovery_time_us);
}

static ssize_t  sysfs_store_last_recovery_time_us(struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count)
{
	//nothing to be done.
	//(last recovery time can't be overwritten from userspace)
	return count;
}

DEFINE_RWLOCK(scorw_lock);

//Note: This list + lock combo is used here as well as in jbd2 code. Hence, we have defined both these in 
//	fs/inode.c 
extern struct list_head wait_for_commit_list;	//list containing info about frnd inodes associated with child inodes waiting for commit 
extern spinlock_t commit_lock;			//spinlock that protects above list

//===================================================================================

struct pending_frnd_version_cnt_inc *scorw_alloc_pending_frnd_version_cnt_inc(void)
{
	return kzalloc(sizeof(struct pending_frnd_version_cnt_inc), GFP_KERNEL);
}

void scorw_free_pending_frnd_version_cnt_inc(struct pending_frnd_version_cnt_inc *pending_frnd_version_cnt_inc)
{
	return kfree(pending_frnd_version_cnt_inc);
}

void scorw_add_pending_frnd_version_cnt_inc_list(struct pending_frnd_version_cnt_inc *pending_frnd_version_cnt_inc)
{
	INIT_LIST_HEAD(&pending_frnd_version_cnt_inc->list);
	list_add_tail(&(pending_frnd_version_cnt_inc->list), &pending_frnd_version_cnt_inc_list);
}

void scorw_remove_pending_frnd_version_cnt_inc_list(struct pending_frnd_version_cnt_inc *pending_frnd_version_cnt_inc)
{
	list_del(&(pending_frnd_version_cnt_inc->list));
}

//===================================================================================



void scorw_inc_thread_usage_count(struct scorw_inode *scorw_inode)
{
	////Commentedprintk("scorw_inc_thread_usage_count called\n");
	atomic64_inc(&(scorw_inode->i_thread_usage_count));

}

struct wait_for_commit *scorw_alloc_wait_for_commit(void)
{
	return kzalloc(sizeof(struct wait_for_commit), GFP_KERNEL);
}

void scorw_add_wait_for_commit_list(struct wait_for_commit *wait_for_commit)
{
	INIT_LIST_HEAD(&wait_for_commit->list);
	list_add_tail(&(wait_for_commit->list), &wait_for_commit_list);
}

void scorw_inc_process_usage_count(struct scorw_inode *scorw_inode)
{
	////Commentedprintk("scorw_inc_process_usage_count called\n");

	////Commentedprintk("scorw_inc_process_usage_count: Before usage_count: %d\n", scorw_inode->i_usage_count);
	//atomic64_inc(&(scorw_inode->i_process_usage_count));
	////Commentedprintk("scorw_inc_process_usage_count: After usage_count: %d\n", scorw_inode->i_usage_count);
	spin_lock(&(scorw_inode->i_process_usage_count_lock));
	++(scorw_inode->i_process_usage_count);
	spin_unlock(&(scorw_inode->i_process_usage_count_lock));

}

unsigned scorw_get_range_i_start_attr_val(struct inode *inode, int index)
{
	int buf_size = 0;
	unsigned range_i_start = 0;
	char range_start[CHILD_RANGE_LEN];

	////Commentedprintk("Inside scorw_get_range_i_start_attr_val\n");
	if(index >= MAX_RANGES_SUPPORTED)
	{
		//Commentedprintk("Error: scorw_get_range_i_start_attr_val: Only %d ranges supported! \n", MAX_RANGES_SUPPORTED);
		return 0;
	}

	memset(range_start, '\0', CHILD_RANGE_LEN);
	sprintf(range_start, "%s%d", scorw_range_start, index);

	buf_size = ext4_xattr_get(inode, 1, range_start, &range_i_start, sizeof(unsigned));
	if(buf_size > 0)
	{
		return range_i_start;
	}
	return 0;
}


unsigned scorw_get_range_i_end_attr_val(struct inode *inode, int index)
{
	int buf_size = 0;
	unsigned range_i_end = 0;
	char range_end[CHILD_RANGE_LEN];

	////Commentedprintk(" Inside scorw_get_range_i_end_attr_val\n");
	if(index >= MAX_RANGES_SUPPORTED)
	{
		//Commentedprintk("Error: scorw_get_range_i_end_attr_val: Only %d ranges supported!\n", MAX_RANGES_SUPPORTED);
		return 0;
	}

	memset(range_end, '\0', CHILD_RANGE_LEN);
	sprintf(range_end, "%s%d", scorw_range_end, index);

	buf_size = ext4_xattr_get(inode, 1, range_end, &range_i_end, sizeof(unsigned));
	if(buf_size > 0)
	{
		return range_i_end;
	}
	return 0;
}



//given child vfs inode, returns number of ranges supplied to child
int scorw_get_num_ranges_attr_val(struct inode *inode)
{
	int n_ranges;
	int buf_size;

	////Commentedprintk("Inside scorw_get_num_ranges_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, num_ranges, &n_ranges, sizeof(int));
	if(buf_size > 0)
	{
		return n_ranges;
	}
	return 0;
}

unsigned long scorw_get_parent_attr_val(struct inode *inode)
{
	unsigned long p_ino_num;
	int buf_size;

	////Commentedprintk("Inside scorw_get_parent_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, scorw_parent , &p_ino_num, sizeof(unsigned long));
	if(buf_size > 0)
	{
		////Commentedprintk("Returning from scorw_get_parent_attr_val\n");
		return p_ino_num;
	}
	////Commentedprintk("Returning from scorw_get_parent_attr_val\n");
	return 0;
}

//given inode, returns its friends inode number (if it exists)
//returns 0 otherwise
unsigned long scorw_get_friend_attr_val(struct inode *inode)
{
	unsigned long f_ino_num;
	int buf_size;

	////Commentedprintk("Inside scorw_get_friend_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, scorw_friend , &f_ino_num, sizeof(unsigned long));
	if(buf_size > 0)
	{
		////Commentedprintk("Returning from scorw_get_friend_attr_val\n");
		return f_ino_num;
	}
	////Commentedprintk("Returning from scorw_get_friend_attr_val\n");
	return 0;
}

//MAHA_AARSH_Start
u32 scorw_get_last_open_time(struct inode* inode){
	u32 lopen_time; /*Short for last_open_time*/
	int buf_size;

	buf_size = ext4_xattr_get(inode, 1, last_open_time , &lopen_time, sizeof(u32));
	if(buf_size > 0){
		return lopen_time;
	} 
	return 0;	
}





u32 scorw_set_last_open_time(struct inode* inode){
	int buf_size;
	u32 curr_time = (u32) ktime_get_real_seconds();
	buf_size = ext4_xattr_set(inode, 1, last_open_time , &curr_time, sizeof(u32) , 0);
	if(buf_size > 0){
		return 1;
	} 
	return 0;	
}



int scorw_get_see_thru_attr_val(struct inode *inode)
{
	int is_see_thru_ro;
	int buf_size;

	////Commentedprintk("Inside scorw_get_friend_attr_val\n"); 
	buf_size = ext4_xattr_get(inode, 1, see_thru_ro , &is_see_thru_ro, sizeof(unsigned long));
	if(buf_size > 0)
	{
		////Commentedprintk("Returning from scorw_get_friend_attr_val\n");
		return is_see_thru_ro;
	}
	////Commentedprintk("Returning from scorw_get_friend_attr_val\n");
	return 0;
}


unsigned long scorw_get_log_attr_val(struct inode *inode)
{
	unsigned long log_ino_num;
	int buf_size;

	////Commentedprintk("Inside scorw_get_friend_attr_val\n"); 
	buf_size = ext4_xattr_get(inode, 1, scorw_log , &log_ino_num, sizeof(unsigned long));
	if(buf_size > 0)
	{
		////Commentedprintk("Returning from scorw_get_friend_attr_val\n");
		return log_ino_num;
	}
	////Commentedprintk("Returning from scorw_get_friend_attr_val\n");
	return 0;
}
//MAHA_AARSH_End


void special_open(struct inode *par_inode, struct inode *child_inode, int index)
{
	int i = 0;
	int n = 0;
	struct scorw_inode *par_scorw_inode = 0;
	struct scorw_inode *child_scorw_inode = 0;

	////Commentedprintk("Inside %s(). par: %lu, child: %lu, index: %d\n", __func__, par_inode->i_ino, child_inode->i_ino, index);
	//par exists?
	//read_lock(&scorw_lock);
	//par_scorw_inode = scorw_search_inode_list(par_inode->i_ino);
	//read_unlock(&scorw_lock);
	par_scorw_inode = par_inode->i_scorw_inode;
	if(par_scorw_inode)
	{
		//create child scorw inode
		////Commentedprintk("%s(): par: %lu scorw exists\n", __func__, par_inode->i_ino);
		child_scorw_inode = scorw_get_inode(child_inode, 1, 0);
		if(child_scorw_inode)
		{
			//attach child scorw inode to par
			////Commentedprintk("%s(): child: %lu is open\n", __func__, child_inode->i_ino);
			down_write(&(par_scorw_inode->i_lock));
			par_scorw_inode->i_child_scorw_inode[index] = child_scorw_inode;
			child_scorw_inode->i_at_index = index;
			if(index > par_scorw_inode->i_last_child_index)
			{
				par_scorw_inode->i_last_child_index = index;
			}
			up_write(&(par_scorw_inode->i_lock));
			////Commentedprintk("%s(): Attached child: %lu to par: %lu at index: %d\n", __func__, child_inode->i_ino, par_inode->i_ino, index);

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
		////Commentedprintk("%s(): par: %lu scorw does not exists. Parent open() count: %d\n", __func__, par_inode->i_ino, atomic_read(&(par_inode->i_vfs_inode_open_count)));
		if(atomic_read(&(par_inode->i_vfs_inode_open_count)) > 1)
		{
			////Commentedprintk("%s(): par: %lu scorw does not exists. But par file is open.\n", __func__, par_inode->i_ino);

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
			////Commentedprintk("%s(): par: %lu scorw does not exists and par file is not open. Doing nothing.\n", __func__, par_inode->i_ino);
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
long long scorw_get_copy_size_attr_val(struct inode *inode)
{
	long long c_size;
	int buf_size;

	////Commentedprintk("Inside scorw_get_copy_size_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, copy_size, &c_size, sizeof(long long));
	if(buf_size > 0)
	{
		return c_size;
	}
	return 0;
}

//given vfs inode, returns count of blocks yet to be copied from parent to child.
unsigned scorw_get_blocks_to_copy_attr_val(struct inode *inode)
{
	unsigned blocks_count;
	int buf_size;

	////Commentedprintk("Inside scorw_get_blocks_to_copy_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, blocks_to_copy, &blocks_count, sizeof(unsigned));
	////Commentedprintk("scorw_get_blocks_to_copy_attr_val: value: %llu\n", blocks_count);
	if(buf_size > 0)
	{
		return blocks_count;
	}
	return 0;
}

//MAHA_AARSH_start

//Version starts from 1
unsigned long scorw_get_curr_version_attr_val(struct inode *inode)
{
	unsigned long version_val;
	int buf_size;

	////Commentedprintk("Inside scorw_get_child_version_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, curr_version, &version_val, sizeof(unsigned long));
	if(buf_size > 0)
	{
		return version_val;
	}
	return 0;
}

//
void scorw_set_curr_version_attr_val(struct inode *inode , unsigned long val /*Value to be set*/)
{
	////Commentedprintk("Inside scorw_get_child_version_attr_val\n");
	ext4_xattr_set(inode, 1, curr_version, &val, sizeof(unsigned long), 0);
}
// Pass in parent inode
unsigned long scorw_get_original_parent_size(struct inode * p_inode){

	unsigned long original_size;
	int buf_size;

	////Commentedprintk("Inside scorw_get_child_version_attr_val\n");
	buf_size = ext4_xattr_get(p_inode, 1, par_org, &original_size, sizeof(unsigned long));
	if(buf_size > 0)
	{
		return original_size;
	}
	return 0;

}

int update_version(struct inode *c_inode){
	struct scorw_inode *sci = scorw_find_inode(c_inode);
	struct inode *p_inode_live;
	unsigned long old_v, new_v;

	if (!sci) return -EINVAL;

	if (sci->i_par_vfs_inode == NULL) {
		//Commentedprintk("SCORW_DEBUG: Update failed - Child has no Parent link.\n");
		return -ENOENT;
	}

	p_inode_live = sci->i_par_vfs_inode;
	old_v = sci->version;
	new_v = scorw_get_curr_version_attr_val(p_inode_live);

	sci->version = new_v;
	//scorw_set_curr_version_attr_val(c_inode, new_v);
	// Safety net: Just in case Ext4 put metadata in the child's mapping.
	// Since the data is in the parent, this takes 0.001ms because it's already empty.
	//truncate_inode_pages(c_inode->i_mapping, 0); 

	//Commentedprintk("SCORW_DEBUG: IOCTL UPDATE. Child %lu: V%lu -> V%lu. Fast Sync complete.\n", 
			//c_inode->i_ino, old_v, new_v);

	return 0;
}
//MAHA_AARSH_end




unsigned long scorw_get_child_version_attr_val(struct inode *inode)
{
	unsigned long version_val;
	int buf_size;

	////Commentedprintk("Inside scorw_get_child_version_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, version, &version_val, sizeof(unsigned long));
	if(buf_size > 0)
	{
		return version_val;
	}
	return 0;
}

unsigned long scorw_get_frnd_version_attr_val(struct inode *inode)
{
	unsigned long version_val;
	int buf_size;

	////Commentedprintk("Inside scorw_get_frnd_version_attr_val\n");
	buf_size = ext4_xattr_get(inode, 1, version, &version_val, sizeof(unsigned long));
	if(buf_size > 0)
	{
		return version_val;
	}
	return 0;
}

int scorw_create_zero_page(struct inode *inode, unsigned lblk)
{
	struct address_space *mapping = NULL;
	struct folio *page_w = NULL;
	void *fsdata = 0;
	int error = 0;
	void *kaddr_w;
	int i;

	if(get_child_inode_num)
	{
		down_read(&(inode->i_scorw_rwsem));
	}
	mapping = inode->i_mapping;
	////Commentedprintk("scorw_copy_zero_page: Inside scorw_copy_zero_page, lblk: %u\n", lblk);
	//This function is there in inode.c
	error = scorw_da_write_begin(NULL, mapping, ((unsigned long)lblk) << PAGE_SHIFT, PAGE_SIZE, &page_w, &fsdata);
	////Commentedprintk("################# Page allocated but not mapped. page_w->_refcount: %d, page_w->_mapcount: %d\n",  page_ref_count(page_w), atomic_read(&page_w->_mapcount));
	if(page_w != NULL)
	{
		kaddr_w = kmap_atomic(&(page_w->page)); //convert folio* to page*
							////Commentedprintk("################# Page allocated and mapped. page_w->_refcount: %d, page_w->_mapcount: %d\n", page_ref_count(page_w),atomic_read(&page_w->_mapcount));

		for(i = 0; i < PAGE_SIZE; i++)
		{
			*((char*)kaddr_w + i) = '\0';
		}
		kunmap_atomic(kaddr_w);
		////Commentedprintk("################# Page allocated and unmapped mapped. page_w->_refcount: %d, page_w->_mapcount: %d\n",  page_ref_count(page_w),atomic_read(&page_w->_mapcount));

		//ext4_es_print_tree(scorw_inode->i_vfs_inode);
		scorw_da_write_end(NULL, mapping , ((unsigned long)lblk) << PAGE_SHIFT, PAGE_SIZE, PAGE_SIZE, page_w, fsdata);
		////Commentedprintk("1. scorw_copy_zero_page: size of frnd file as per its inode: %lu\n", inode->i_size);

		//There are only few pages in friend file. So, we can skip ratelimiting it.
		//This is done to avoid acquiring scorw_lock recursively. Read comment inside scorw_is_par_file() to know more.
		//balance_dirty_pages_ratelimited(mapping);
		////Commentedprintk("2. scorw_copy_zero_page: size of frnd file as per its inode: %lu\n", inode->i_size);

		if(get_child_inode_num)
		{
			up_read(&(inode->i_scorw_rwsem));
		}

	}
	else
	{
		//Commentedprintk("scorw_create_zero_page: Error: %d in ext4_da_write_begin\n", error);
		if(get_child_inode_num)
		{
			up_read(&(inode->i_scorw_rwsem));
		}

		return error;
	}
	return 0;
}

void scorw_set_frnd_version_attr_val(struct inode *inode, unsigned long version_val)
{
	////Commentedprintk("Inside scorw_set_frnd_version_attr_val\n");
	ext4_xattr_set(inode, 1, version, &version_val, sizeof(unsigned long), 0);

}

void scorw_set_block_copied_8bytes(struct inode* frnd_inode, unsigned long start_blk, unsigned long long value_8bytes)
{
	struct address_space *mapping = NULL;
	struct folio* page_w=NULL;
	void *fsdata = 0;
	char* kaddr = 0;
	int which_8bytes = 0;

	////Commentedprintk("scorw_set_block_copied_8bytes called\n");
	mapping = frnd_inode->i_mapping;

	scorw_da_write_begin(NULL, mapping, ((unsigned long)start_blk/PAGE_BLOCKS) << PAGE_SHIFT, PAGE_SIZE, &page_w, &fsdata);       

	if(page_w != NULL)
	{
		start_blk = start_blk % PAGE_BLOCKS;

		kaddr = kmap_atomic(&(page_w->page));
		which_8bytes = start_blk / 64;  //8 bytes == 64 bits. To which 8 bytes, this blk belongs to?
		*((unsigned long long*)(kaddr + (which_8bytes * 8))) |= value_8bytes;
		kunmap_atomic(kaddr);
		scorw_da_write_end(NULL, mapping, ((unsigned long)start_blk/PAGE_BLOCKS) << PAGE_SHIFT, PAGE_SIZE,PAGE_SIZE,page_w,fsdata); 
		balance_dirty_pages_ratelimited(mapping);
	}
	else
	{
		//Commentedprintk("scorw_set_block_copied_8bytes: ext4_da_write_begin failed to return a page\n");
	}
}

unsigned long scorw_get_child_i_attr_val(struct inode *inode, int child_i)
{
	int buf_size = 0;
	unsigned long c_ino_num;
	char scorw_child_name[CHILD_NAME_LEN];

	////Commentedprintk("Inside scorw_get_child_i_attr_val\n");

	if(child_i >= SCORW_MAX_CHILDS)
	{
		//Commentedprintk("Error: scorw_get_child_i_attr_val: Only %d children supported!\n", SCORW_MAX_CHILDS);
		return 0;
	}

	memset(scorw_child_name, '\0', CHILD_NAME_LEN);
	sprintf(scorw_child_name, "%s%d", scorw_child, child_i);

	buf_size = ext4_xattr_get(inode, 1, scorw_child_name, &c_ino_num, sizeof(unsigned long));
	if(buf_size > 0)
	{
		return c_ino_num;
	}
	return 0;
}


int scorw_ext_precache_depth0(struct inode* child_inode)
{
	int i;
	//int depth = ext_depth(child_inode);
	struct ext4_extent_header *eh = ext_inode_hdr(child_inode);
	struct ext4_extent *ex = EXT_FIRST_EXTENT(eh);
	ext4_lblk_t prev = 0;

	for (i = le16_to_cpu(eh->eh_entries); i > 0; i--, ex++) {
		unsigned int status = EXTENT_STATUS_WRITTEN;
		ext4_lblk_t lblk = le32_to_cpu(ex->ee_block);
		int len = ext4_ext_get_actual_len(ex);

		if(prev && (prev != lblk))
		{
			ext4_es_cache_extent(child_inode, prev, lblk - prev, ~0, EXTENT_STATUS_HOLE);
		}

		if(ext4_ext_is_unwritten(ex))
		{
			status = EXTENT_STATUS_UNWRITTEN;
		}
		ext4_es_cache_extent(child_inode, lblk, len, ext4_ext_pblock(ex), status);
		prev = lblk + len;
	}

	return 0;
}


int scorw_rebuild_friend_file(struct inode* child_inode, struct inode* frnd_inode)
{
	struct ext4_es_tree *tree = 0;
	struct rb_node *node = 0;
	unsigned long long value_8bytes = 0;
	unsigned long start_blk = 0;
	unsigned long pending_extent_blks = 0;
	int num_bits_to_process = 0;
	int copy_bit = 0;
	int depth = ext_depth(child_inode);

	////Commentedprintk("Inside scorw_rebuild_friend_file. child inode: %lu, frnd inode: %lu\n", child_inode->i_ino, frnd_inode->i_ino);

	//precache extents (bring extent info from disk into extent status tree)
	////Commentedprintk("%s(): Child has depth %d\n", __func__, depth);
	if(depth == 0)
	{
		scorw_ext_precache_depth0(child_inode);
	}
	else
	{
		ext4_ext_precache(child_inode);
	}

	//scorw_es_print_tree(child_inode);	//<------------ Added for debugging purpose

	//process extents in extent status tree
	tree = &EXT4_I(child_inode)->i_es_tree;
	node = rb_first(&tree->root);
	while(node){
		struct extent_status *es;
		es = rb_entry(node, struct extent_status, rb_node);
		if((ext4_es_status(es) & ((ext4_fsblk_t)EXTENT_STATUS_WRITTEN)) ||
				(ext4_es_status(es) & ((ext4_fsblk_t)EXTENT_STATUS_UNWRITTEN)) ||
				(ext4_es_status(es) & ((ext4_fsblk_t)EXTENT_STATUS_DELAYED)))
		{
			start_blk = es->es_lblk;
			pending_extent_blks = es->es_len;
			////Commentedprintk("%s(): [%u/%u) %llu EXTENT_STATUS_WRITTEN\n", __func__, es->es_lblk, es->es_len, ext4_es_pblock(es));
			while(pending_extent_blks)
			{

				/*
				 * Eg:
				 * 	extent info = 32/200 (extent start blk num = 32, extent length = 200 i.e blks 32 to 231 are part of this extent)
				 *	Initially, start blk = 32, pending extent blks = 200
				 *
				 *	(start blk, num bits to process in current 8 bytes (64 bit) boundary, pending extent blks)
				 *	32, 32, 168
				 *	64, 64, 104
				 *	128, 64, 40
				 *	192, 40, 0
				 */
				num_bits_to_process = ((64 - (start_blk % 64)) < (pending_extent_blks)) ? (64 - (start_blk % 64)) : (pending_extent_blks);

				value_8bytes = 0;	//since we have reset all friend file bits to 0, no need to call scorw_get_block_copied_8bytes() to read
							//the 8bytes corresponding start blk (they will be 0)
							////Commentedprintk("1. %s(): start_blk: %lu, pending_extent_blks: %lu, num_bits_to_process: %d, [Before]value_8bytes: %llx\n", __func__, start_blk, pending_extent_blks, num_bits_to_process, value_8bytes);

							////Commentedprintk("2. %s(): copy_bit range: %lu to %lu\n", __func__, start_blk % 64, ((start_blk % 64) + num_bits_to_process));
				for(copy_bit = start_blk % 64; copy_bit < ((start_blk % 64) + num_bits_to_process); copy_bit++)
				{
					value_8bytes = value_8bytes | ((unsigned long long)1 << copy_bit);
				}
				////Commentedprintk("3. %s(): [After 1]value_8bytes: %llx\n", __func__, value_8bytes);
				scorw_set_block_copied_8bytes(frnd_inode, start_blk, value_8bytes);

				pending_extent_blks -= num_bits_to_process;
				start_blk += num_bits_to_process;
			}
		}
		//if(ext4_es_status(es) & ((ext4_fsblk_t)EXTENT_STATUS_HOLE))
		//	//Commentedprintk(KERN_DEBUG " [%u/%u) %llu EXTENT_STATUS_HOLE", es->es_lblk, es->es_len, ext4_es_pblock(es));
		node = rb_next(node);
	}

	return 0;
}


int scorw_recover_friend_file(struct inode* child_inode, struct inode* frnd_inode, long long child_copy_size)
{
	unsigned i = 0;
	unsigned total_pages = 0;
	unsigned long long rebuilding_time_start = 0;
	unsigned long long rebuilding_time_end = 0;

	total_pages = (((child_copy_size % (PAGE_BLOCKS << PAGE_SHIFT)) == 0) ? (child_copy_size / (PAGE_BLOCKS << PAGE_SHIFT)) : ((child_copy_size / (PAGE_BLOCKS << PAGE_SHIFT))+1));

	////Commentedprintk("scorw_prepare_friend_file called\n");
	////Commentedprintk("scorw_prepare_friend_file: frnd_size: %lld\n", frnd_size);
	////Commentedprintk("scorw_prepare_friend_file: total_pages to allocate in friend file: %lu\n", total_pages);
	////Commentedprintk("scorw_prepare_friend_file: Rebuilding frnd file corresponding child: %lu\n", child_inode->i_ino);
	rebuilding_time_start = ktime_get_real_ns();
	/*
	 * It is possible that child blks are not present on disk but friend file has copy bits set corresponding these blks.
	 * Eg: Assume that a child file has 1000 dirty blks. In a single page of friend file itself copy bit corresponding all 1000 blks can be present.
	 * Now, it is possible that the sole friend file block gets flushed to disk while not all of the 1000 dirty blks of child get flushed to disk and system crashes.
	 * In this case, on recovery, friend file will contain some bits as set even though child doesn't have data corresponding those blks.
	 * Hence, we are resetting all bits in frnd file to 0 before beginning the rebuilding process.
	 */
	for(i = 0; i < total_pages; i++)
	{
		scorw_create_zero_page(frnd_inode, i);
	}
	scorw_rebuild_friend_file(child_inode, frnd_inode);
	rebuilding_time_end = ktime_get_real_ns();
	////Commentedprintk("rebuilding_time_end: %llu, rebuilding_time_start: %llu, Rebuilding time (ms): %llu\n", rebuilding_time_end, rebuilding_time_start, (rebuilding_time_end - rebuilding_time_start)/1000000);
	last_recovery_time_us = (rebuilding_time_end - rebuilding_time_start)/1000;

	return 0;
}


struct scorw_inode* scorw_get_inode(struct inode *inode, int is_child_inode, int new_sparse)
{
	int i;
	unsigned long c_ino_num;
	struct scorw_inode *scorw_inode;
	struct scorw_inode *c_scorw_inode;
	struct inode *c_inode;


	//Commentedprintk("1.Inside scorw_get_inode, inode: %lu, is_child_inode: %d\n", inode->i_ino, is_child_inode);
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
		//Commentedprintk("[DEBUG] (already_linked) ::%s:opening i_ino=%lu,cnt=%lu",__func__,scorw_inode->i_ino_num,scorw_get_process_usage_count(scorw_inode));	
		if(is_child_inode && scorw_inode->i_ino_unlinked)
		{
			////Commentedprintk("scorw_get_inode, child inode: %lu has been unlinked. Won't be attached with par\n", inode->i_ino);
			mutex_unlock(&(inode->i_vfs_inode_lock));
			return 0;
		}
		////Commentedprintk("2.scorw_get_inode: Already there is a scorw inode corresponding inode: %lu\n", inode->i_ino);
		//increment usage count of already existing scorw inode
		scorw_inc_process_usage_count(scorw_inode);
		if(scorw_inode->i_par_vfs_inode && scorw_inode->i_par_vfs_inode->i_scorw_inode){
			scorw_inc_process_usage_count(scorw_inode->i_par_vfs_inode->i_scorw_inode);
		} else { /* increase ref count of only parent and not child , comment this shit */
			if(scorw_inode->i_par_vfs_inode && !(scorw_inode->i_par_vfs_inode->i_scorw_inode)){
				//Commentedprintk("[WIERD] :: i_par_vfs_exists but no scorw_inode for child_inode=%lu\n" , inode->i_ino);
			}
		}
	}
	else
	{

		////Commentedprintk("3.scorw_get_inode: No. Already there is not a scorw inode corresponding inode: %lu\n", inode->i_ino);
		////Commentedprintk("4.scorw_get_inode: Inserting new scorw inode\n");

		scorw_inode = scorw_alloc_inode();
		if(scorw_inode != NULL)
		{
			init_rwsem(&(scorw_inode->i_lock));
			spin_lock_init(&(scorw_inode->i_process_usage_count_lock));
			scorw_inode->added_to_page_copy = 0;
			scorw_inode->removed_from_page_copy = 0;
			if(is_child_inode)
			{
				////Commentedprintk("5.scorw_get_inode: Calling scorw_prepare_child_inode\n");
				scorw_prepare_child_inode(scorw_inode, inode, new_sparse);
				if(scorw_thread)
				{
					//increment usage count of scorw inode on the behalf of kernel thread
					////Commentedprintk("scorw_get_inode: Incrementing usage count\n");
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
				////Commentedprintk("6.scorw_get_inode: Calling scorw_prepare_par_inode\n");
				scorw_prepare_par_inode(scorw_inode, inode);
			}

			//increment usage count of scorw inode created
			//This refers to the usage on behalf of process of which this scorw inode is getting created
			////Commentedprintk("7.scorw_get_inode: Incrementing usage count\n");
			scorw_inc_process_usage_count(scorw_inode);

			//Commentedprintk("[DEBUG] :: %s : opening i_ino=%lu , cnt=%lu\n",__func__,scorw_inode->i_ino_num,scorw_get_process_usage_count(scorw_inode));	

			//attach newly created scorw inode to the corresponding vfs inode
			inode->i_scorw_inode = scorw_inode;

			if(!is_child_inode)
			{
				////Commentedprintk("scorw_get_inode: Parent will create scorw inodes of children now\n");
				for(i = 0; i < SCORW_MAX_CHILDS; i++)
				{
					c_ino_num = scorw_get_child_i_attr_val(inode, i);
					if(c_ino_num)
					{
						////Commentedprintk("scorw_get_inode: child %d, inode num: %lu\n", i, c_ino_num);
						c_inode = ext4_iget(inode->i_sb, c_ino_num, EXT4_IGET_NORMAL);
						if(IS_ERR_VALUE(c_inode) || !scorw_is_child_file(c_inode, 1))
						{
							//Commentedprintk("%s(): Possibly parent contains dangling extended attribute about a deleted child!\n", __func__);
							//Commentedprintk("%s(): Deleted this extended attribute..\n", __func__);
							scorw_remove_child_i_attr(inode, i);
							continue;
						}
						c_scorw_inode = scorw_get_inode(c_inode, 1, 0);
						////Commentedprintk("scorw_get_inode: scorw inode of child %d, inode num: %lu, got created\n", i, c_ino_num);
						scorw_inode->i_child_scorw_inode[i] = c_scorw_inode;
						if(c_scorw_inode)
						{
							c_scorw_inode->i_at_index = i;
							c_inode->i_scorw_inode = c_scorw_inode;
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
			////Commentedprintk("8.scorw_get_inode: Adding to inode list\n");
			//This lock is required to prevent search operations from occuring in parallel while the insertion of a new
			//node takes place in the list
			//
			//This list is useful when scorw copy thread has to pick an scorw inode to process
			write_lock(&scorw_lock);
			scorw_add_inode_list(scorw_inode);
			write_unlock(&scorw_lock);
			//Commentedprintk("[DEBUG] :: %s : BREAKPOINT1 i_ino=%lu , cnt=%lu\n",__func__,scorw_inode->i_ino_num,scorw_get_process_usage_count(scorw_inode));	
		}
		else
		{
			mutex_unlock(&(inode->i_vfs_inode_lock));
			//Commentedprintk(KERN_ERR "scorw_get_inode: SCORW_OUT_OF_MEMORY: Releasing vfs inode lock\n");
			return 0;
		}
	}
	////Commentedprintk("scorw_get_inode: releasing vfs inode lock\n");
	mutex_unlock(&(inode->i_vfs_inode_lock));

	//Commentedprintk("[DEBUG] :: %s : BREAKPOINT2 i_ino=%lu , cnt=%lu\n",__func__,scorw_inode->i_ino_num,scorw_get_process_usage_count(scorw_inode));	
	////Commentedprintk("9.scorw_get_inode: returning\n");

	return scorw_inode;
}

int scorw_get_range_attr_val(struct inode *inode, int index, struct child_range_xattr *crx)
{
	char range_start[CHILD_RANGE_LEN];
	int retval = 0;
	////Commentedprintk("Inside scorw_get_range_i_start_attr_val\n");
	if(index >= MAX_RANGES_SUPPORTED)
	{
		//Commentedprintk("Error: scorw_get_range_i_start_attr_val: Only %d ranges supported! \n", MAX_RANGES_SUPPORTED);
		goto ret;
	}

	memset(range_start, '\0', CHILD_RANGE_LEN);
	sprintf(range_start, "%s%d", scorw_range_start, index);

	retval = ext4_xattr_get(inode, 1, range_start, crx, sizeof(struct child_range_xattr));
ret:
	return retval;
}

// MAHA_AARSH_Start
void scorw_prepare_child_inode(struct scorw_inode *scorw_inode, struct inode *vfs_inode, int new_sparse)
{
	int i = 0;
	unsigned j = 0;
	int first_write_done = 0;
	unsigned long p_ino_num = 0;
	unsigned long f_ino_num = 0;
	//unsigned long log_ino_num = 0;
	unsigned temp_hash_table_size = 0;
	unsigned long child_version_val = 0;
	unsigned long frnd_version_val = 0;
	int curr_version = 1;


	p_ino_num = scorw_get_parent_attr_val(vfs_inode);
	BUG_ON(p_ino_num == 0);

	f_ino_num = scorw_get_friend_attr_val(vfs_inode);
	BUG_ON(f_ino_num == 0);

	curr_version = scorw_get_curr_version_attr_val(vfs_inode);
	BUG_ON(curr_version == 0);

	init_Transaction_locks(scorw_inode);
	scorw_inode->version = curr_version;
	scorw_inode->i_ino_num = vfs_inode->i_ino;
	//atomic64_set((&(scorw_inode->i_process_usage_count)), 0);
	scorw_inode->i_process_usage_count = 0;
	atomic64_set((&(scorw_inode->i_thread_usage_count)), 0);
	scorw_inode->i_ino_unlinked = 0;
	scorw_inode->i_vfs_inode = ext4_iget(vfs_inode->i_sb, vfs_inode->i_ino, EXT4_IGET_NORMAL);
	scorw_inode->i_par_vfs_inode = 	ext4_iget(vfs_inode->i_sb, p_ino_num, EXT4_IGET_NORMAL);
	scorw_inode->i_frnd_vfs_inode =	ext4_iget(vfs_inode->i_sb, f_ino_num, EXT4_IGET_NORMAL);
	scorw_inode->i_copy_size = scorw_get_copy_size_attr_val(vfs_inode);
	atomic64_set(&(scorw_inode->i_pending_copy_pages),  scorw_get_blocks_to_copy_attr_val(scorw_inode->i_vfs_inode));
	scorw_inode->i_at_index = -1;
	ext4_inode_attach_jinode(scorw_inode->i_frnd_vfs_inode);
	ext4_inode_attach_jinode(vfs_inode);


	scorw_inode->i_num_ranges = scorw_get_num_ranges_attr_val(vfs_inode);
	scorw_inode->is_see_thru_ro = 0;	// Initialize to 0
	for(i=0; i<scorw_inode->i_num_ranges; i++)
	{
#ifdef USE_OLD_RANGE
		scorw_inode->i_range[i].start = scorw_get_range_i_start_attr_val(vfs_inode, i);
		scorw_inode->i_range[i].end = scorw_get_range_i_end_attr_val(vfs_inode, i);
#else
		struct child_range_xattr crx;
		BUG_ON(!scorw_get_range_attr_val(vfs_inode, i, &crx));

		scorw_inode->i_range[i].start = crx.start;

		scorw_inode->i_range[i].end = crx.end;
		scorw_inode->i_range[i].snapx_behavior = crx.snap_behavior;

		// Check if this range is see through read only
		if(scorw_inode->i_range[i].snapx_behavior == SNAPX_FLAG_SEE_TH_RO)
		{
			scorw_inode->is_see_thru_ro = 1;
		}
#endif
	}
#if 0
	for(i=scorw_inode->i_num_ranges; i<MAX_RANGES_SUPPORTED; i++)
	{
		scorw_inode->i_range[i].start = -1;
		scorw_inode->i_range[i].end = -1;
		////Commentedprintk("scorw_prepare_child_inode: range: %d, %lld:%lld\n", i, scorw_inode->i_range[i].start, scorw_inode->i_range[i].end);
	}
#endif



	mutex_init(&(scorw_inode->i_uncopied_extents_list_lock));
	INIT_LIST_HEAD(&(scorw_inode->i_uncopied_extents_list));

	/*find the number of blocks occupied by parent file at the time of copying and divide the result by 4.
	  Then, round the number to lower power of 2.

	  Why divide by 4?
	  Because, we are creating hash lists (size of hash table) proportional to the size of parent file at the time of copying
	  Initially, we created 1 million (2^20) such lists  while testing for 16GB files
	  i.e. 2^20 lists for file with 2^22 blocks.
	  We keep this proportion unchanged i.e. for every 4 blocks in parent file, we create a list in the hash table

	  Why round the number to lower power of 2?
Eg: 6MB file => 384 lists.
hash table api's work with power of 2. Hence, when log2(384) is calculated, hash table api's
will treat hash table as of size 256 i.e. 256 linked lists only i.e. remaining lists will be unused.
Hence, we before hand round of number of linked lists to a power of 2, so that, unecessary
lists are not created.

	//Update:
	 * Number of active map entries at any time = ((write size)/4KB * num threads)
	 * For example, for a 32 threaded experiment with write size <= 4KB, max number of active map entries anytime will be 32
	 * For example, for a 32 threaded experiment with write size = 1MB, max number of active map entries anytime will be (32 * (1MB/4KB)) = 2^13
	 * Above, example is an extreme case and normally, active map with few ten's or hundred's of lists should suffice
	 * I think, size of parent file at the time of copying is irrelevant in deciding the size of active map lists
	 */
	//temp_hash_table_size = (((scorw_inode->i_copy_size - 1) >> PAGE_SHIFT) + 1) >> 2;
	temp_hash_table_size = 128;
	temp_hash_table_size = temp_hash_table_size > MIN_HASH_TABLE_SIZE ? temp_hash_table_size : MIN_HASH_TABLE_SIZE;
	temp_hash_table_size = 1 << (ilog2(temp_hash_table_size));
	scorw_inode->i_hash_table_size = temp_hash_table_size;

	scorw_inode->i_uncopied_blocks_list = vmalloc(sizeof(struct hlist_head) * scorw_inode->i_hash_table_size);
	BUG_ON(scorw_inode->i_uncopied_blocks_list == NULL);
	__hash_init(scorw_inode->i_uncopied_blocks_list, scorw_inode->i_hash_table_size);

	scorw_inode->i_uncopied_blocks_lock = vmalloc(sizeof(struct spinlock) * scorw_inode->i_hash_table_size);
	BUG_ON(scorw_inode->i_uncopied_blocks_lock == NULL);
	for(j = 0; j < scorw_inode->i_hash_table_size; j++)
	{
		spin_lock_init(&(scorw_inode->i_uncopied_blocks_lock[j]));
	}

	////Commentedprintk("%s(): [Before recovery code] child file (inode: %lu) child is dirty (%d) (inode dirty: %lu, data pages dirty: %lu)\n", __func__, scorw_inode->i_vfs_inode->i_ino, scorw_is_inode_dirty(scorw_inode->i_vfs_inode), scorw_inode->i_vfs_inode->i_state & I_DIRTY_INODE, scorw_inode->i_vfs_inode->i_state & I_DIRTY_PAGES);

	//Recovering frnd file
	//Note: Trigger recovery only when child version count hasn't been incremented yet
	//	i.e. we are not in the middle of the updation of child and frnd version counts.
	first_write_done = atomic_read(&(scorw_inode->i_vfs_inode->i_cannot_update_child_version_cnt));
	if(first_write_done == 0)
	{
		child_version_val = scorw_get_child_version_attr_val(scorw_inode->i_vfs_inode);
		frnd_version_val = scorw_get_frnd_version_attr_val(scorw_inode->i_frnd_vfs_inode);
		BUG_ON((child_version_val == 0) || (frnd_version_val == 0));

		////Commentedprintk("%s(): Recover frnd: %d (child version: %lu, frnd version: %lu)\n", __func__, child_version_val != frnd_version_val, child_version_val, frnd_version_val);
		if(child_version_val != frnd_version_val)
		{
			//recover frnd file
			scorw_recover_friend_file(scorw_inode->i_vfs_inode, scorw_inode->i_frnd_vfs_inode, scorw_inode->i_copy_size);

			//sync recovered frnd file blocks
			write_inode_now(scorw_inode->i_frnd_vfs_inode, 1);

			//set frnd version count equal to child version cnt
			frnd_version_val = child_version_val;
			scorw_set_frnd_version_attr_val(scorw_inode->i_frnd_vfs_inode, frnd_version_val);

			//sync frnd inode
			sync_inode_metadata(scorw_inode->i_frnd_vfs_inode, 1);

			++num_files_recovered;
			////Commentedprintk("%s(): Num files recovered: %lu\n", __func__, num_files_recovered);
		}

	}
	else
	{
		////Commentedprintk("%s(): Skipping frnd recovery. We are in the middle of version cnt update.\n", __func__);
	}
	////Commentedprintk("%s(): [After recovery code] child file (inode: %lu) child is dirty (%d) (inode dirty: %lu, data pages dirty: %lu)\n", __func__, scorw_inode->i_vfs_inode->i_ino, scorw_is_inode_dirty(scorw_inode->i_vfs_inode), scorw_inode->i_vfs_inode->i_state & I_DIRTY_INODE, scorw_inode->i_vfs_inode->i_state & I_DIRTY_PAGES);

	////Commentedprintk("scorw_prepare_child_inode: returning\n");
	//debugging start
	scorw_inode->i_par_vfs_inode->scorw_page_cache_hits = 0;
	scorw_inode->i_par_vfs_inode->scorw_page_cache_misses = 0;
	//debugging end
	//MAHA_AARSH_start
	//scorw_init_ram_and_log(vfs_inode, scorw_inode);
	//MAHA_AARSH_end
}
//add an scorw inode into list of scorw inode's
void scorw_add_inode_list(struct scorw_inode *scorw_inode)
{
	//struct scorw_inode *tmp;
	////Commentedprintk("Inside scorw_add_inode_list\n");
	////Commentedprintk("%s: Adding scrow_inode = %lx, inode_num = %lu, vfs_inode  = %lx \n", __func__, scorw_inode, scorw_inode->i_ino_num, scorw_inode->i_vfs_inode);
	INIT_LIST_HEAD(&scorw_inode->i_list);
	list_add_tail(&(scorw_inode->i_list), &scorw_inodes_list);

	/*list_for_each_entry(tmp, &scorw_inodes_list, i_list)
	  //Commentedprintk("%s: Listing scrow_inode = %lx, inode_num = %lu, vfs_inode  = %lx \n", __func__, tmp, tmp->i_ino_num, tmp->i_vfs_inode);
	 */

}

//allocate memory for scorw inode
struct scorw_inode *scorw_alloc_inode(void)
{
	////Commentedprintk("Inside scorw_alloc_inode\n");
	//return kzalloc(sizeof(struct scorw_inode), GFP_KERNEL);
	//return vzalloc(sizeof(struct scorw_inode));

	//vzalloc() internally calls __vmalloc() but with GFP_KERNEL flag. Changed the flag to GFP_NOWAIT
	return kzalloc(sizeof(struct scorw_inode), GFP_KERNEL);
}

//free memory occupied by scorw inode
void scorw_free_inode(struct scorw_inode *scorw_inode)
{
	//Commentedprintk("[DEBUG]  ::  Inside %s , called for inode_number = %lu\n" , __func__ , scorw_inode->i_ino_num);
	//return vfree(scorw_inode);/// Patching to kfree
	scorw_cleanup_versions(scorw_inode);
	return kfree(scorw_inode);
}

// MAHA_AARSH_Start



int scorw_is_opened_first_time(struct inode * inode){
	struct ext4_sb_info * sbi;
	struct ext4_super_block * es;
	struct super_block *sb;
	u32 lopen , mtime;		
	lopen = scorw_get_last_open_time(inode);
	
	if(lopen == 0){ /*We are opening for the very first time*/
		scorw_set_last_open_time(inode);
		return 0;
	}	
	
	if(!inode){
		return 0;
	}

	sb  = inode->i_sb;
	sbi = EXT4_SB(sb);
	es  = sbi->s_es;
	mtime = le32_to_cpu(es->s_mtime);
		
	return (mtime > lopen);
}

/*
 * Helper to calculate CRC32C over a range of pages in the page cache
 */
static __u32 calc_page_cache_crc(struct address_space *mapping, unsigned long start_blk, unsigned int len_blks) {
	__u32 crc = 0;
	int i;
	for (i = 0; i < len_blks; i++) {
		struct page *page = read_mapping_page(mapping, start_blk + i, NULL);
		if (IS_ERR(page)) {
			// continue;
            return 0;
		}
		char *kaddr = kmap_atomic(page);
		crc = crc32c(crc, kaddr, PAGE_SIZE);
		kunmap_atomic(kaddr);
		put_page(page);
	}
	return ~crc;
}

/*
 * Scans the log file and truncates any records that exceed the safely
 * committed version stored in the parent's xattr. Also discards partial records.
 */
void scorw_truncate_log_to_version(struct scorw_inode *s_inode)
{
	struct inode *log_inode = s_inode->i_log_vfs_inode;
	loff_t log_size, offset = 0;
	struct page *page;
	char *kaddr;
	struct scorw_log_record *record;
	loff_t valid_size = 0;
	char *log_page_buf;
	loff_t current_version_start_offset = 0;
	int current_evaluating_version = 0;
	__u32 safe_version = s_inode->version;

	if (!log_inode || IS_ERR(log_inode)) return;

	log_size = i_size_read(log_inode);
	if (log_size == 0) return;

	log_page_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!log_page_buf) return;

	while (offset < log_size) {
		pgoff_t index = offset >> PAGE_SHIFT;
		unsigned int page_offset = offset & (PAGE_SIZE - 1);
		unsigned int bytes_to_read = PAGE_SIZE - page_offset;

		if (offset + bytes_to_read > log_size)
			bytes_to_read = log_size - offset;

		page = read_mapping_page(log_inode->i_mapping, index, NULL);
		if (IS_ERR(page)) {
			break;
		}

		kaddr = (char *)kmap_atomic(page);
		memcpy(log_page_buf, kaddr, PAGE_SIZE);
		kunmap_atomic(kaddr);
		put_page(page);

		record = (struct scorw_log_record *)(log_page_buf + page_offset);

		while (bytes_to_read >= sizeof(struct scorw_log_record)) {
			if (record->version_num != current_evaluating_version) {
				current_evaluating_version = record->version_num;
				current_version_start_offset = offset;
			}

			// OPTIMIZATION: Only verify CRCs for versions newer than the xattr safe version
			if (record->version_num > safe_version) {
				__u32 disk_crc = calc_page_cache_crc(s_inode->i_vfs_inode->i_mapping, record->physical_start_blk, record->len_blks);
				
				if (disk_crc != record->data_crc32c) {
					// TORN WRITE DETECTED!
					valid_size = current_version_start_offset; 
					goto do_truncate;
				}
			}

			valid_size += sizeof(struct scorw_log_record);
			record++;
			bytes_to_read -= sizeof(struct scorw_log_record);
			offset += sizeof(struct scorw_log_record);
		}
		
		if (bytes_to_read > 0 && offset + bytes_to_read >= log_size) {
			break;
		}
	}

do_truncate:
	kfree(log_page_buf);

	if (valid_size < log_size) {
		inode_lock(log_inode);
		truncate_setsize(log_inode, valid_size);
		mark_inode_dirty(log_inode);
		inode_unlock(log_inode);
	}

	if (current_evaluating_version > safe_version && valid_size == log_size) {
		s_inode->version = current_evaluating_version;
		// scorw_set_curr_version_attr_val(s_inode->i_vfs_inode, s_inode->version);
	}
}

void scorw_do_recovery(struct scorw_inode * scorw_inode , struct inode * inode){
	/* Your logic goes here */
	int latest_version = scorw_inode->version;
	
	
	 
	

	return;
}


void scorw_prepare_par_inode(struct scorw_inode *scorw_inode, struct inode *vfs_inode)
{
	int i = 0;
	//unsigned long c_ino_num = 0;
	//struct scorw_inode *c_inode = 0;
	unsigned long log_ino_num = 0;
	int curr_version = 1;	
	unsigned long orig_size = 0;
	int is_see_thru_ro = 0;
	int to_be_checked;
	
	to_be_checked = scorw_is_opened_first_time(vfs_inode);
	scorw_set_last_open_time(vfs_inode);

	//Commentedprintk("[DEBUG] :: {%s} :: i_ino=%lu scorw_is_opened_first_time=%d\n" , __func__ , vfs_inode->i_ino , to_be_checked);
	is_see_thru_ro = scorw_get_see_thru_attr_val(vfs_inode);
	BUG_ON(is_see_thru_ro == -1);

	log_ino_num = scorw_get_log_attr_val(vfs_inode);
	BUG_ON(log_ino_num == 0);

	curr_version = scorw_get_curr_version_attr_val(vfs_inode);
	BUG_ON(curr_version == 0);

	orig_size = scorw_get_original_parent_size(vfs_inode);
	BUG_ON(orig_size == 0);

	scorw_inode->is_see_thru_ro = is_see_thru_ro;
	init_Transaction_locks(scorw_inode);

	scorw_inode->version = curr_version;
	scorw_inode->i_ino_num = vfs_inode->i_ino;
	//atomic64_set((&(scorw_inode->i_process_usage_count)), 0);
	scorw_inode->i_process_usage_count = 0;
	atomic64_set((&(scorw_inode->i_thread_usage_count)), 0);
	scorw_inode->i_vfs_inode = ext4_iget(vfs_inode->i_sb, vfs_inode->i_ino, EXT4_IGET_NORMAL);
	scorw_inode->i_par_vfs_inode = NULL;
	scorw_inode->i_frnd_vfs_inode = NULL;
	scorw_inode->i_log_vfs_inode = ext4_iget(vfs_inode->i_sb, log_ino_num, EXT4_IGET_NORMAL); //Loads the i_log_vfs_inode into the parent...
	scorw_inode->i_copy_size = 0;
	scorw_inode->i_ino_unlinked = 0;
	scorw_inode->i_at_index = -1;
	scorw_inode->i_num_ranges = 0;
	for(i=0; i<MAX_RANGES_SUPPORTED; i++)
	{
		scorw_inode->i_range[i].start = -1;
		scorw_inode->i_range[i].end = -1;
	}
	//Commentedprintk("SCORW_DEBUG: Inode %lu (Address: %p) - Init Flag is: %d\n",
			//vfs_inode->i_ino, scorw_inode, scorw_inode->is_log_initialized);
	if (!IS_ERR(scorw_inode->i_log_vfs_inode)) {
		if(scorw_inode->is_log_initialized == false)
		{
			scorw_inode->is_log_initialized = true;
		}
	}
	if(to_be_checked){
		scorw_truncate_log_to_version(scorw_inode);
		/* After consistency check, run garbage collection to punch holes
		 * for blocks no longer referenced by any child version. */
		scorw_gc_blocks(scorw_inode);
	}
}

//return 1 if child inodecd ./maha-flex-clone-cs-614-2026-Maha_port_clone/test_rajan/
int scorw_is_child_file(struct inode* inode, int consult_extended_attributes)
{
	unsigned long p_ino_num;
	struct scorw_inode* scorw_inode;
	int is_child_inode;

	////Commentedprintk("Inside scorw_is_child_file\n");

	//check in memory list of scorw inodes
	////Commentedprintk("scorw_is_child_file: checking in memory list of scorw inode\n");
	scorw_inode = scorw_find_inode(inode);
	////Commentedprintk("scorw_is_child_file: scorw_inode: %p\n", scorw_inode);
	if(scorw_inode != NULL)
	{
		is_child_inode = scorw_is_child_inode(scorw_inode);
		////Commentedprintk("scorw_is_child_file: is_child_inode: %d\n", is_child_inode);
		return (is_child_inode != 0);
	}

	////Commentedprintk("scorw_is_child_file: consult_extended_attributes: %d\n", consult_extended_attributes);
	if(!consult_extended_attributes)
	{
		return 0;
	}

	//check extended attribute
	////Commentedprintk("scorw_is_child_file: checking in extended attribute\n");

	p_ino_num = scorw_get_parent_attr_val(inode);
	////Commentedprintk("scorw_is_child_file: is child file? : %lu. Returning from this function\n", p_ino_num);
	if(p_ino_num > 0)
	{
		return 1;
	}
	return 0;       //not a child file
}

struct scorw_inode* scorw_find_inode(struct inode *inode)
{
	//struct scorw_inode *scorw_inode = NULL;

	////Commentedprintk("Inside scorw_find_inode\n");

	////Commentedprintk("scorw_find_inode: searching scorw_lock corresponding inode: %lu\n", inode->i_ino);
	//read_lock(&scorw_lock);
	//scorw_inode = scorw_search_inode_list(inode->i_ino);
	//read_unlock(&scorw_lock);

	return inode->i_scorw_inode;
}

int scorw_is_child_inode(struct scorw_inode *scorw_inode)
{
	////Commentedprintk("Inside scorw_is_child_inode\n");
	if(scorw_inode->i_par_vfs_inode != NULL)
	{
		return 1;
	}
	return 0;
}

int scorw_is_par_file(struct inode* inode, int consult_extended_attributes)
{
	int i;
	unsigned long c_ino_num;
	struct scorw_inode* scorw_inode;
	int is_child_inode;

	////Commentedprintk("Inside scorw_is_par_file\n");


	/*
	 * Note: scorw_find_inode acquires a global mutex lock that protects list of scorw inodes.
	 *      This can lead to recursive lock bug when scorw_get_inode is called (scorw_get_inode also acquires this global mutex lock)
	 *      scorw_get_inode creates zero pages for friend file. While creating zero pages balance_dirty_pages is called.
	 *      Inside this balance_dirty_pages fn, we call scorw_is_par_file (which calls scorw_find_inode) resulting in attempt to recursively
	 *      acquire the mutex lock.
	 *
	 *      Even if we consult extended attributes to conclude whether an inode is a parent inode or not, overhead should not be much because
	 *      page containing extended attributes will be cached in memory.
	 *
	 *      Update:
	 *              1) consulting extended attributes drops performance significantly. Ran a fio on baseline file s.t. it consulted extended attributes to confirm whether a file
	 *                      is a parent file or not. Throughput decreased from (~1400MBps when scorw_is_par_file concludes that a file is a parent file or not without consulting
	 *                      extended attributes vs ~450MBps when scorw_is_par_file consulted extended attributes.
	 *              2) Have changed the position inside balance_dirty_pages, from where scorw_is_par_file is called. I think, it won't be called now from the scorw_get_inode ---> frnd file zero pages
	 *                      path. So, we can use scorw_find_inode here without any problem.
	 *
	 *
	 */

	//check in memory list of scorw inodes
	//check in memory list of scorw inodes
	////Commentedprintk("scorw_is_par_file: checking in memory list of scorw inode\n");
	scorw_inode = scorw_find_inode(inode);
	////Commentedprintk("scorw_is_par_file: scorw_inode: %p\n", scorw_inode);
	if(scorw_inode != NULL)
	{
		is_child_inode = scorw_is_child_inode(scorw_inode);
		////Commentedprintk("scorw_is_par_file: is_child_inode: %d\n", is_child_inode);
		return (is_child_inode == 0);
	}

	////Commentedprintk("scorw_is_par_file: consult_extended_attributes: %d\n", consult_extended_attributes);
	if(!consult_extended_attributes)
	{
		return 0;
	}


	//check extended attributes if atleast one set child return true
	for(i = 0; i < SCORW_MAX_CHILDS; i++)
	{
		////Commentedprintk("scorw_is_par_file: Inside for loop\n");
		c_ino_num = scorw_get_child_i_attr_val(inode, i);
		////Commentedprintk("Inside c_ino_num of child %d: %llu\n", i, c_ino_num);

		if(c_ino_num)
		{
			return 1;
		}
	}
	return 0;
}


int scorw_put_inode(struct inode *inode, int is_child_inode, int is_thread_putting, int is_par_putting)
{
	int i = 0;
	int j = 0;
	int q = 0;
	unsigned long c_ino_num = 0;
	struct scorw_inode *scorw_inode = NULL;
	struct scorw_inode *c_scorw_inode = NULL;
	struct scorw_inode *p_scorw_inode = NULL;
	struct list_head *head;
	struct list_head *curr;
	struct page_copy *curr_page_copy;

	
//	dump_stack();
	//Commentedprintk("[DEBUG] :: %s called for inode %ld\n" , __func__ , inode->i_ino);

		

	////Commentedprintk("1.scorw_put_inode called for inode: %lu\n", inode->i_ino);
	//If this is the last reference to child scorw inode, we will be inserting
	//this child's info into the list to be processed by asynch thread
	//for updation of frnd version count for recovery.
	//
	//This list is protected by the following (frnd_version_cnt_lock) lock
	//
	//Ordering of the locks is as following:
	//	frnd_version_cnt_lock
	//		i_vfs_inode_lock
	//
	if(is_child_inode)
	{
		mutex_lock(&(frnd_version_cnt_lock));
	}
	mutex_lock(&(inode->i_vfs_inode_lock));

	//find scorw inode corresponding vfs inode
	//read_lock(&scorw_lock);
	//scorw_inode = scorw_search_inode_list(inode->i_ino);
	//read_unlock(&scorw_lock);
	scorw_inode = inode->i_scorw_inode;

	//MAHA_AARSH_VERSION_start
	//save the version into xattr

	//MAHA_AARSH_VERSION_start

	//scorw_put_inode of child corresponding par's scorw_get_inode has been already done
	//in unlink child function. So, don't do it again.
	if(scorw_inode == NULL)
	{
		////Commentedprintk("2.scorw_put_inode: scorw_inode is not present corresponding this inode (or) child is unlinked and par is putting. Releasing vfs inode lock.\n");
		mutex_unlock(&(inode->i_vfs_inode_lock));
		if(is_child_inode)
		{
			mutex_unlock(&(frnd_version_cnt_lock));
		}
		return 0;
	}
	////Commentedprintk("3.scorw_put_inode: scorw_inode is present corresponding this inode\n");
	ext4_xattr_set(inode , 1 , curr_version  , &(scorw_inode->version) , sizeof(int) , 0 ); //TODO put locks here
												//decrease usage count of scorw inode
	if(is_thread_putting)
	{
		++(scorw_inode->removed_from_page_copy);
		////Commentedprintk("3.scorw_put_inode: thread putting scorw inode. page copy structures processed: %lu\n", scorw_inode->removed_from_page_copy);
	}
	else
	{

		scorw_dec_process_usage_count(scorw_inode); /* XXX */
		/* Probably comment this shit out coz of our new idea */
		if(is_child_inode && (scorw_inode->i_par_vfs_inode) && (scorw_inode->i_par_vfs_inode->i_scorw_inode)){
			if(!is_par_putting){
				scorw_put_inode(scorw_inode->i_par_vfs_inode , 0 , 0 , 1);
			}
		}

		////Commentedprintk("4.scorw_put_inode: process putting scorw inode. updated usage count: %lu\n", scorw_get_process_usage_count(scorw_inode));
	}

	////Commentedprintk("5.scorw_put_inode: process usage count is: %ld\n", scorw_get_process_usage_count(scorw_inode));
	////Commentedprintk("6.scorw_put_inode: thread usage count is: %ld\n", scorw_get_thread_usage_count(scorw_inode));
	//////Commentedprintk("scorw_put_inode: uncopied blocks count is: %d\n", scorw_get_uncopied_blocks_count(scorw_inode));


	//free scorw inode if its overall usage count is 0
	//Note 1:
	//	* Different cases for blocks in page copy queue:
	//		- Parent hasn't called put inode but page copy thread has already processed all page copy structures related to parent
	//			+ In this case, following if condition will be false because process usage count is not zero
	//
	//		- Parent has called put inode and page copy thread has already processed all page copy structures related to parent
	//			+ In this case, following if condition will be true
	//
	//		- Parent has called put inode but page copy thread hasn't processed all page copy structures related to parent
	//			+ In this case, following if condition will be false because second condition will be false
	//
	//		- Parent has called put inode and page copy thread later processes all page copy structures related to parent
	//			+ In this case, following if condition will be true
	//Note 2:
	//	* process usage count should be checked first and only when process usage count becomes 0, page copy counters should
	//	  be checked because, process usage count being zero suggests that parent has closed
	//	  and now 'added to page copy counter won't change'.
	//

	//Commentedprintk("[DEBUG] :: %s called with %lu\n" , __func__ , scorw_get_process_usage_count(scorw_inode));
	if((scorw_get_process_usage_count(scorw_inode) == 0) && (scorw_inode->added_to_page_copy == scorw_inode->removed_from_page_copy))
	{
		////Commentedprintk("scorw_put_inode: inode: %lu, process and thread usage count is 0\n", scorw_inode->i_ino_num);
		if(is_child_inode)
		{
			//child file has been unlinked
			//Perform cleanup of parent-child relationship
			//Note: On parent's scorw_put_inode call, control won't reach here.
			if(scorw_inode->i_ino_unlinked)
			{
				////Commentedprintk("scorw_put_inode: child inode: %lu, child has been unlinked. Detaching from par if it exists\n", scorw_inode->i_ino_num);
				//detach from parent if it exists
				if(scorw_inode->i_at_index != -1)
				{
					p_scorw_inode = scorw_inode->i_par_vfs_inode->i_scorw_inode;

					read_lock(&(page_copy_lock));
					head = &page_copy_llist;
					list_for_each(curr, head)
					{
						curr_page_copy = list_entry(curr, struct page_copy, ll_node);
						curr_page_copy->is_target_child[scorw_inode->i_at_index] = 0;
						////Commentedprintk("scorw_put_inode: block_num: %u, parent inode num: %lu", curr_page_copy->block_num, curr_page_copy->par->i_ino_num);
					}
					read_unlock(&(page_copy_lock));

					down_write(&(p_scorw_inode->i_lock));
					p_scorw_inode->i_child_scorw_inode[scorw_inode->i_at_index] = 0;
					if(scorw_inode->i_at_index == p_scorw_inode->i_last_child_index)
					{
						p_scorw_inode->i_last_child_index = -1;	//in case parent has no child
						for(q=scorw_inode->i_at_index-1; q>= 0; q--)
						{
							if(p_scorw_inode->i_child_scorw_inode[q])
							{
								p_scorw_inode->i_last_child_index = q;
								break;
							}
						}
					}
					up_write(&(p_scorw_inode->i_lock));
					////Commentedprintk("scorw_put_inode: child inode: %lu detached from par: %lu\n", scorw_inode->i_ino_num, p_scorw_inode->i_ino_num);
				}
				else
				{
					////Commentedprintk("scorw_put_inode: child inode: %lu. par: doesn't %lu exists\n", scorw_inode->i_ino_num, scorw_inode->i_par_vfs_inode->i_ino);
				}

				//cleanup extended attributes irrespective of parent being open or not
				for(j = 0; j < SCORW_MAX_CHILDS; j++)
				{
					c_ino_num = scorw_get_child_i_attr_val(scorw_inode->i_par_vfs_inode, j);
					if(c_ino_num == inode->i_ino)
					{
						////Commentedprintk("scorw_put_inode: child inode: %lu info removed from par %lu extended attributes\n", scorw_inode->i_ino_num, scorw_inode->i_par_vfs_inode->i_ino);
						scorw_remove_child_i_attr(scorw_inode->i_par_vfs_inode, j);
						break;
					}
				}
			}
			else
			{
				////Commentedprintk("scorw_put_inode: child inode: %lu, child has not been unlinked\n", scorw_inode->i_ino_num);
			}

			//do cleanup on the basis of how much data has been copied from parent to child
			scorw_child_copy_completion_cleanup(scorw_inode);

			////Commentedprintk("8.scorw_put_inode: unpreparing child inode: %lu.\n", scorw_inode->i_ino_num);
			//free fields of scorw inode
			scorw_unprepare_child_inode(scorw_inode);
		}

		//remove scorw inode from scorw inodes list
		////Commentedprintk("10.scorw_put_inode: remove scorw inode from scorw inodes list: %lu\n", scorw_inode->i_ino_num);
		//This lock is required to prevent search operations from occuring in parallel while the deletion of a
		//node takes place from the list
		write_lock(&scorw_lock);
		scorw_remove_inode_list(scorw_inode);
		write_unlock(&scorw_lock);

		//detach scorw inode from its vfs inode
		//Commentedprintk("Detatching i_ino=%lu from it's i_scorw_inode\n" , inode->i_ino);
		inode->i_scorw_inode = 0;

		//decrease ref count of child scorw inodes of parent also.
		if(!is_child_inode)
		{
			////Commentedprintk("scorw_put_inode: putting parent scorw inode's children scorw inodes\n");
			//handle each child.
			for(i = 0; i < SCORW_MAX_CHILDS; i++)
			{
				c_scorw_inode = scorw_inode->i_child_scorw_inode[i];
				if(c_scorw_inode == NULL)
				{
					continue;
				}
				////Commentedprintk("scorw_put_inode: child number: %d has inode num: %lu. Putting this child.\n", i, c_scorw_inode->i_ino_num);

				scorw_put_inode(c_scorw_inode->i_vfs_inode, 1, 0, is_par_putting);

			}

			////Commentedprintk("9.scorw_put_inode: unpreparing parent scorw inode\n");
			//free fields of scorw inode
			scorw_unprepare_par_inode(scorw_inode);
		}

		//free memory occupied by scorw inode
		////Commentedprintk("scorw_put_inode: freeing memory occupied by scorw inode\n");
		scorw_free_inode(scorw_inode);

	}
	////Commentedprintk("scorw_put_inode: Releasing vfs inode lock\n");
	mutex_unlock(&(inode->i_vfs_inode_lock));
	if(is_child_inode)
	{
		mutex_unlock(&(frnd_version_cnt_lock));
	}

	////Commentedprintk("11.scorw_put_inode returning after processing inode: %lu\n", inode->i_ino);

	return 0;
}

void scorw_dec_process_usage_count(struct scorw_inode *scorw_inode)
{
	//if(atomic64_read(&(scorw_inode->i_process_usage_count)) > 0)
	//      atomic64_dec(&(scorw_inode->i_process_usage_count));
	spin_lock(&(scorw_inode->i_process_usage_count_lock));
	//Commentedprintk("[DEBUG] :: %s called for i_ino = %lu,count = %lu\n" , __func__ , scorw_inode->i_ino_num , scorw_inode->i_process_usage_count);
	BUG_ON(scorw_inode->i_process_usage_count <= 0);
	--(scorw_inode->i_process_usage_count);
	spin_unlock(&(scorw_inode->i_process_usage_count_lock));

}

unsigned long scorw_get_process_usage_count(struct scorw_inode *scorw_inode)
{
	////Commentedprintk("scorw_get_process_usage_count called\n");
	unsigned long usage_count = 0;

	spin_lock(&(scorw_inode->i_process_usage_count_lock));
	usage_count = scorw_inode->i_process_usage_count;
	spin_unlock(&(scorw_inode->i_process_usage_count_lock));

	return usage_count;
}

void scorw_child_copy_completion_cleanup(struct scorw_inode *scorw_inode)
{
	////Commentedprintk("Inside scorw_child_copy_completion_cleanup, scorw_inode inode num: %lu\n", scorw_inode->i_ino_num);
	////Commentedprintk("scorw_child_copy_completion_cleanup: scorw_inode->i_pending_copy_pages: %u\n", scorw_inode->i_pending_copy_pages);

	//End parent child relationship
	if((atomic64_read(&(scorw_inode->i_pending_copy_pages)) == 0) && ((scorw_inode->i_frnd_vfs_inode->i_state & I_DIRTY_PAGES) == 0))
	{
		//Note/Todo:
		//Consider a situation where, during write to parent, write operation happens on all parent blocks.
		//When fio has completed, page copy can be still processing the queued blocks to be processed.
		//Now consider the case when page copy also has finished its processing.
		//In this scenario we expect that cleanup of extended attributes of par,child and friend files can be done.
		//
		//However, there's a catch. Eventhough page copy thread has finished, in writeback path, still writeback
		//has to rely on inode numbers of par/child/frnd for writing back data blocks.
		//So, if we cleanup extended attributes + scorw inodes, writeback path's attempt to get inode numbers (from extended attributes)
		//will fail i.e. 0 will be returned by API's when writeback path tries to read extended attributes of par, child, frnd files.
		//
		//Sample bug: EXT4-fs error (device vdb1): writeback_sb_inodes:1966: comm kworker/u64:2: inode #0: comm kworker/u64:2: iget: illegal inode #
		//(This bug came because extended attributes got cleaned up and when writeback path tried to find the inode number of frnd file, it got 0 as
		//return value (i.e. extended attribute doesn't exists).
		//
		//So, temporarily stopping the cleanup of extended attributes until a better way to handle this is found.
		//

		//

		/*
		////Commentedprintk("scorw_child_copy_completion_cleanup: All data has been copied from parent to child. Doing cleanup\n");
		//remove parent attributes maintained by child inode
		scorw_remove_par_attr(scorw_inode->i_vfs_inode);

		//remove child attributes maintained by parent inode
		scorw_remove_child_attr(scorw_inode->i_par_vfs_inode, scorw_inode->i_ino_num);

		//remove child attributes maintained by friend inode
		scorw_remove_child_friend_attr(scorw_inode->i_frnd_vfs_inode);
		 */
	}
	else if(atomic64_read(&(scorw_inode->i_pending_copy_pages)) > 0)
	{
		//save info about count of blocks yet to be copied to disk
		scorw_set_blocks_to_copy_attr_val(scorw_inode->i_vfs_inode, atomic64_read(&(scorw_inode->i_pending_copy_pages)));
	}
}


void scorw_set_blocks_to_copy_attr_val(struct inode *inode, unsigned blocks_count)
{
	////Commentedprintk("Inside scorw_set_blocks_to_copy_attr_val\n");
	////Commentedprintk("scorw_set_blocks_to_copy_attr_val: new value: %llu\n", blocks_count);
	ext4_xattr_set(inode, 1, blocks_to_copy, &blocks_count, sizeof(unsigned), 0);

}

void scorw_remove_child_i_attr(struct inode *inode, int child_i)
{
	char scorw_child_name[CHILD_NAME_LEN];

	////Commentedprintk("scorw_remove_child_i_attr called\n");

	if(child_i >= SCORW_MAX_CHILDS)
	{
		//Commentedprintk("Error: scorw_remove_child_i_attr: Only %d children supported!\n", SCORW_MAX_CHILDS);
	}

	memset(scorw_child_name, '\0', CHILD_NAME_LEN);
	sprintf(scorw_child_name, "%s%d", scorw_child, child_i);

	ext4_xattr_set(inode, 1, scorw_child_name, NULL, 0, 0);
}

struct scorw_inode *scorw_search_inode_list(unsigned long ino_num)
{
	struct list_head *curr;
	struct scorw_inode *curr_scorw_inode;

	////Commentedprintk("Inside scorw_search_inode_list\n");

	list_for_each(curr, &scorw_inodes_list)
	{
		curr_scorw_inode = list_entry(curr, struct scorw_inode, i_list);
		////Commentedprintk("scorw_search_inode_list: Comparing %lu and %lu. Looking for scorw inode corresponding inode: %lu\n", curr_scorw_inode->i_ino_num, ino_num, ino_num);
		if(curr_scorw_inode->i_ino_num == ino_num)
		{
			////Commentedprintk("scorw_search_inode_list: Matching scorw inode found. Returning.\n");
			return curr_scorw_inode;
		}
	}
	////Commentedprintk("scorw_search_inode_list: No Matching scorw inode found\n");
	return NULL;
}

void scorw_remove_inode_list(struct scorw_inode *scorw_inode)
{
	////Commentedprintk("%s: Removing scrow_inode = %lx, inode_num = %lu, vfs_inode  = %lx \n", __func__, scorw_inode, scorw_inode->i_ino_num, scorw_inode->i_vfs_inode);
	list_del(&(scorw_inode->i_list));
}
void scorw_unprepare_par_inode(struct scorw_inode *scorw_inode)
{
	//int i;
	//struct inode *c_inode;
	////Commentedprintk("[DEBUG]  ::  Inside %s , called for inode_number = %lu\n" , __func__ , scorw_inode->i_ino_num);
	////Commentedprintk("Inside scorw_unprepare_par_inode\n");
	////Commentedprintk("scorw_unprepare_par_inode: inode num: %lu, inode ref count value: %u (Before iput)\n", scorw_inode->i_vfs_inode->i_ino, scorw_inode->i_vfs_inode->i_count);
	scorw_set_curr_version_attr_val(scorw_inode -> i_vfs_inode, scorw_inode->version); // UPDATE DISK XATTR
	iput(scorw_inode->i_vfs_inode);
	////Commentedprintk("scorw_unprepare_par_inode: inode num: %lu, inode ref count value: %u (After iput)\n", scorw_inode->i_vfs_inode->i_ino, scorw_inode->i_vfs_inode->i_count);
		
}


//unload fields of scorw inode. Eg: deallocation of memory occupied by fields, reseting of fields
void scorw_unprepare_child_inode(struct scorw_inode *scorw_inode)
{
	struct pending_frnd_version_cnt_inc *pending_frnd_version_cnt_inc = 0;
	int entry_found = 0;
	struct inode *child_inode = scorw_inode->i_vfs_inode;
	struct inode *par_inode = scorw_inode->i_par_vfs_inode;
	struct inode *frnd_inode = scorw_inode->i_frnd_vfs_inode;
	
	
	scorw_set_curr_version_attr_val(scorw_inode -> i_vfs_inode, scorw_inode->version); // UPDATE DISK XATTR
	////Commentedprintk("Inside scorw_unprepare_child_inode\n");

	////Commentedprintk("%s(): [closing file] child file (inode: %lu) child is dirty (%d) (inode dirty: %lu, data pages dirty: %lu)\n", __func__, scorw_inode->i_vfs_inode->i_ino, scorw_is_inode_dirty(scorw_inode->i_vfs_inode), scorw_inode->i_vfs_inode->i_state & I_DIRTY_INODE, scorw_inode->i_vfs_inode->i_state & I_DIRTY_PAGES);

	//debugging start
	////Commentedprintk("Num page cache hits: %lu\n", scorw_inode->i_par_vfs_inode->scorw_page_cache_hits);
	////Commentedprintk("Num page cache misses: %lu\n", scorw_inode->i_par_vfs_inode->scorw_page_cache_misses);
	//debugging end

	vfree(scorw_inode->i_uncopied_blocks_list);
	vfree(scorw_inode->i_uncopied_blocks_lock);

	////Commentedprintk("%s(): i_count of child inode: %u\n", __func__, atomic_read(&scorw_inode->i_vfs_inode->i_count) - 1);
	////Commentedprintk("%s(): i_count of par inode: %u\n", __func__, atomic_read(&scorw_inode->i_par_vfs_inode->i_count) - 1);
	////Commentedprintk("%s(): i_count of frnd inode: %u\n", __func__, atomic_read(&scorw_inode->i_frnd_vfs_inode->i_count) - 1);

	//If no write occurred on child, then skip updation of version cnt of frnd
	//Note:
	//      Recall, once write operation occurs on child, i_cannot_update_child_version_cnt flag is set and it remains set
	//      until version count of frnd inode is updated. So, intermediate open()/close() calls between setting and
	//      unsetting of the i_cannot_update_child_version_cnt, still see i_cannot_update_child_version_cnt flag as set.
	if(atomic_read(&child_inode->i_cannot_update_child_version_cnt) == 0)
	{
		////Commentedprintk("%s(): No write operation performed on child. Skipping updation of version count of frnd of child inode: %lu\n", __func__, child_inode->i_ino);
		iput(par_inode);
		iput(child_inode);
		iput(frnd_inode);
	}
	else
	{
		//queue relevant information for updation of version cnt of frnd
		//Note:
		//      Relevant locks s.a. inode lock for ordering opening/closing of child file (i_vfs_inode_lock) and
		//      lock that protects 'pending_frnd_version_cnt_inc_list' list
		//      are taken in scorw_put_inode() beforehand.
		//
		entry_found = 0;
		//check whether information is already queued
		list_for_each_entry(pending_frnd_version_cnt_inc, &pending_frnd_version_cnt_inc_list, list)
		{
			////Commentedprintk("%s(): Child inode: %lu waiting in queue for processing\n", __func__, pending_frnd_version_cnt_inc->child->i_ino);
			if(pending_frnd_version_cnt_inc->child->i_ino == child_inode->i_ino)
			{
				entry_found = 1;
				break;
			}
		}
		//information is already queued
		iput(par_inode);
		if(entry_found)
		{
			////Commentedprintk("%s(): Child inode: %lu ALREADY waiting in queue for processing\n", __func__, child_inode->i_ino);
			iput(frnd_inode);
			iput(child_inode);
		}
		else
		{
			////Commentedprintk("%s(): Child inode: %lu NOT in queue waiting for processing. Adding it.\n", __func__, child_inode->i_ino);
			//queue information
			//Note: we don't do iput(child, frnd, par) here. It will be done in
			//asynch thread, when it does version count related processing
			pending_frnd_version_cnt_inc = scorw_alloc_pending_frnd_version_cnt_inc();
			BUG_ON(pending_frnd_version_cnt_inc == NULL);

			pending_frnd_version_cnt_inc->child = child_inode;
			pending_frnd_version_cnt_inc->frnd = frnd_inode;
			pending_frnd_version_cnt_inc->iter_id = 0;

			scorw_add_pending_frnd_version_cnt_inc_list(pending_frnd_version_cnt_inc);
		}
	}
	////Commentedprintk("%s(): Closing child\n", __func__);
}

ssize_t scorw_follow_on_read_child_blocks(struct inode* inode, struct kiocb *iocb, struct iov_iter *to)
{
	int i = 0;
	ssize_t r = 0;        	//return value
	ssize_t ret = 0;        //return value
	unsigned curr_page = 0;
	unsigned first_page = 0;
	unsigned last_page = 0;
	unsigned last_block_eligible_for_copy = 0;
	unsigned scan_pages_till = 0;
	struct scorw_inode* scorw_inode = NULL;
	struct uncopied_block *uncopied_block = 0;
	struct page_copy *page_copy = 0;
	enum batching_type prev_batching_type = NOT_STARTED;
	enum batching_type batching_type = NOT_STARTED;
	unsigned batch_start_blk = 0;			//inclusive
	unsigned batch_end_blk = 0;			//not inclusive

	scorw_inode = scorw_find_inode(inode);
	BUG_ON(!scorw_inode || scorw_inode->i_vfs_inode != inode);

	//copy till page before last page.
	//Eg: ki_pos = 0, to->count = 4097, 
	//last_page = 2 because bytes 0 to 4095 of page 0, 4097th byte i.e. byte 0 of page 1 get written.
	first_page = iocb->ki_pos >> PAGE_SHIFT;
	last_page = (iocb->ki_pos + to->count + PAGE_SIZE-1) >> PAGE_SHIFT;				//not inclusive
	last_block_eligible_for_copy  = ((((long long)scorw_inode->i_copy_size)-1) >> PAGE_SHIFT);	//inclusive
	scan_pages_till = scorw_min(last_page-1, last_block_eligible_for_copy);


	////Commentedprintk("\n\nInside scorw_follow_on_read_child_blocks\n");
	////Commentedprintk("scorw_follow_on_read_child_blocks: first_page: %u, last_page: %u\n", first_page, last_page);

	//reading outside copy size	
	if(first_page > last_block_eligible_for_copy)
	{
		////Commentedprintk("scorw_follow_on_read_child_blocks: reading outside copy size. returning SCORW_PERFORM_ORIG_READ\n");
		return SCORW_PERFORM_ORIG_READ;
	}

	//read originating within copy size 
	curr_page = first_page;
	batch_start_blk = curr_page;
	batch_end_blk = curr_page;
	while(curr_page <= scan_pages_till)
	{
		////Commentedprintk("scorw_follow_on_read_child_blocks: Processing page num: %u. Acquiring READING lock\n", curr_page);
		uncopied_block = scorw_get_uncopied_block(scorw_inode, curr_page, READING);	
		////Commentedprintk("scorw_follow_on_read_child_blocks: Processing page num: %u. curr_page: %u, scan_pages_till: %u, batch_start_blk: %u, batch_end_blk: %u. Acquiring READING lock\n", curr_page, curr_page, scan_pages_till, batch_start_blk, batch_end_blk);

		if(scorw_is_block_copied(scorw_inode->i_frnd_vfs_inode, curr_page))		
		{
			//read from child
			////Commentedprintk("scorw_follow_on_read_child_blocks: Block is already copied to child. Will reading block (block num: %u) from child\n", curr_page);
			batching_type = PRESENT_IN_CHILD;

		}
		else	
		{
			page_copy = scorw_find_page_copy(curr_page, scorw_inode->i_par_vfs_inode->i_ino, scorw_inode->i_at_index);	
			//finds in the thread's memory
			if(page_copy != NULL)
			{
				////Commentedprintk("scorw_follow_on_read_child_blocks: Block is in page copy. Will reading block (block num: %u) from child\n", curr_page);
				//let data in page copy be copied to child's page cache
				//Then, perform read from child's page cache
				//We are handling both subcases of 
				//	- (page_copy->data_page_loaded == 0)	and
				//	- (page_copy->data_page_loaded == 1)	
				//using this approach itself
				//
				//Note: we have acquired READING lock and page copy thread acquires COPY_EXCL lock
				//So, if we found a pagecopy, it is gauaranteed that it is use that copies it to the child
				//due to incompatibility of READING and COPY_EXCL lock 
				scorw_copy_page_from_page_copy(page_copy, scorw_inode);
				scorw_set_block_copied(scorw_inode, curr_page);

				//decrement count of remaining blocks to be copied for the thread
				scorw_dec_yet_to_copy_blocks_count(scorw_inode, 1);

				//read from child
				batching_type = PRESENT_IN_CHILD;
			}
			else
			{
				//read from parent
				batching_type = PRESENT_IN_PARENT;
				////Commentedprintk("scorw_follow_on_read_child_blocks: Will be reading block (block num: %u) from parent\n", curr_page);
			}
		}
		++batch_end_blk;
		++curr_page;

		if(batching_type == prev_batching_type)
		{
			////Commentedprintk("scorw_follow_on_read_child_blocks: batching_type == prev_batching_type == %d\n", batching_type);
			continue;
		}
		if(prev_batching_type == NOT_STARTED)
		{
			prev_batching_type = batching_type;
			////Commentedprintk("scorw_follow_on_read_child_blocks: batching started from current block itself. Type of batching == %d\n", batching_type);
			continue;
		}
		if(batching_type != prev_batching_type)
		{
			////Commentedprintk("scorw_follow_on_read_child_blocks: batching_type != prev_batching_type, batching_type: %d, prev_batching_type: %d \n", batching_type, prev_batching_type);

			//submit read request
			r = submit_read_request(scorw_inode, iocb, to, prev_batching_type, batch_start_blk, batch_end_blk - 1);
			if(r > 0)
			{
				ret += r;
			}

			//release blocks from READING state
			for(i = batch_start_blk; i < batch_end_blk - 1; i++)
			{
				scorw_put_uncopied_block(scorw_inode, i, READING, uncopied_block);		
				scorw_remove_uncopied_block(scorw_inode, i, uncopied_block); 			
				////Commentedprintk("scorw_follow_on_read_child_blocks: Processing page num: %u. Released READING lock\n", i);
			}

			//error encountered, Stop reading further
			if(r <= 0)
			{
				return ret;
			}

			prev_batching_type = batching_type;
			batch_start_blk = batch_end_blk - 1;
		}
	}
	////Commentedprintk("[pid: %lu]scorw_follow_on_read_child_blocks: batch_start_blk: %u, batch_end_blk: %u\n", current->pid, batch_start_blk, batch_end_blk);
	if(batch_end_blk > batch_start_blk)
	{
		////Commentedprintk("[pid: %lu]scorw_follow_on_read_child_blocks: Last batch being processed. batching_type: %d, prev_batching_type: %d \n", current->pid, batching_type, prev_batching_type);

		//submit read request
		r = submit_read_request(scorw_inode, iocb, to, batching_type, batch_start_blk, batch_end_blk);
		////Commentedprintk("[pid: %lu]scorw_follow_on_read_child_blocks: ret: %lu, batch_start_blk: %u, batch_end_blk: %u\n", current->pid, r, batch_start_blk, batch_end_blk);
		if(r > 0)
		{
			ret += r;
		}

		//release blocks from READING state
		for(i = batch_start_blk; i < batch_end_blk; i++)
		{
			////Commentedprintk("[pid: %lu] scorw_follow_on_read_child_blocks: Processing page num: %u. Putting READING lock\n", current->pid, i);
			scorw_put_uncopied_block(scorw_inode, i, READING, uncopied_block);		
			////Commentedprintk("[pid: %lu] scorw_follow_on_read_child_blocks: Processing page num: %u. Removing READING lock\n", current->pid, i);
			scorw_remove_uncopied_block(scorw_inode, i, uncopied_block); 			
			////Commentedprintk("[pid: %lu] scorw_follow_on_read_child_blocks: Processing page num: %u. Removed READING lock\n", current->pid, i);
		}
		////Commentedprintk("[pid: %lu]scorw_follow_on_read_child_blocks: done, batch_start_blk: %u, batch_end_blk: %u\n", current->pid, batch_start_blk, batch_end_blk);

		//error encountered, Stop reading further
		if(r <= 0)
		{
			return ret;
		}
	}
	//read originating outside copy size 
	//For example:
	//	If read(page 0,1,2,3,4,5) request comes s.t.
	//		first_page = 0
	//		last_page = 6
	//		last_block_eligible_for_copy = 5
	//	Nothing to read after page 5. 
	//	So, don't enter the 'if' condition
	if(last_page > (last_block_eligible_for_copy + 1))
	{
		do
		{
			r = scorw_read_from_child(iocb, to, batch_start_blk, last_page);
			////Commentedprintk("scorw_follow_on_read_child_blocks: reading data outside copy range. Read: %ld bytes\n", r);
			if(r <= 0)	//Error or no more data in file
			{
				break;
			}
			ret += r;
		}while(1);
	}	

	////Commentedprintk("scorw_follow_on_read_child_blocks: Read: %lu bytes in total\n", ret);
	return ret;
}

unsigned long long scorw_min(unsigned long long a, unsigned long long b)
{
	////Commentedprintk("%s(), a: %llu, b: %llu, a < b? %d\n", __func__, a, b, a < b);
	return ((a < b) ? a : b);
}

ssize_t scorw_read_from_child(struct kiocb *iocb, struct iov_iter *to, unsigned batch_start_blk, unsigned batch_end_blk)
{
	ssize_t ret = 0;
	size_t old_to_count = 0;
	loff_t start = 0;
	loff_t end = 0;
	size_t expected_to_count = 0;


	////Commentedprintk("Inside scorw_read_from_child\n");
	old_to_count = to->count;
	start = iocb->ki_pos;
	end = scorw_min((iocb->ki_pos + to->count) , ((unsigned long long)batch_end_blk << PAGE_SHIFT));
	to->count = end - start;	//read these many bytes
	expected_to_count = to->count;
	////Commentedprintk("%s(), batch_end_blk: %u, old_to_count: %lu, start: %lu, end: %lu, to->count: %lu\n", __func__, batch_end_blk, old_to_count, start, end, to->count);

	ret = generic_file_read_iter(iocb, to);
	////Commentedprintk("%s(), read %lu bytes\n", __func__, ret);
	BUG_ON(ret < 0);

	to->count = old_to_count - ret;
	////Commentedprintk("%s(), updated to->count: %lu after read\n", __func__, to->count);

	////Commentedprintk("scorw_read_from_child: Total return value: %lu\n", ret);
	return ret;
}

//MAHA_AARSH_start
//Serve request from parent's page cache
ssize_t scorw_read_from_parent(struct scorw_inode *s_inode, struct kiocb *iocb, struct iov_iter *to, unsigned b_start, unsigned b_end)
{
	unsigned long target_phys_blk;
	loff_t orig_pos = iocb->ki_pos;
	ssize_t ret;
	loff_t logical_size;
	struct inode *read_inode;
	size_t orig_count = iov_iter_count(to);
	size_t max_read, bytes_to_page_end;

	if (s_inode->i_par_vfs_inode) 
		read_inode = s_inode->i_par_vfs_inode;
	else 
		read_inode = s_inode->i_vfs_inode;

	logical_size = scorw_get_original_parent_size(read_inode);
/*
	// 1. CLAMP EOF: Stop reading past the original logical file size
	if (orig_pos >= logical_size) {
		return 0; // EOF
	}


	max_read = logical_size - orig_pos;
	if (orig_count > max_read) {
		iov_iter_truncate(to, max_read);
		orig_count = max_read;
	}
*/
	// 2. CLAMP BLOCK BOUNDARY: Force kernel to read one block at a time
	// This ensures we never miss a Time Machine redirect that happens halfway through a read
	bytes_to_page_end = PAGE_SIZE - (orig_pos & (PAGE_SIZE - 1));
	if (orig_count > bytes_to_page_end) {
		iov_iter_truncate(to, bytes_to_page_end);
	}

	// 3. TIME MACHINE LOOKUP
	target_phys_blk = scorw_lookup_physical_block(s_inode, orig_pos / PAGE_SIZE);

	if (target_phys_blk != BLK_NOT_FOUND) {
		// MATCH: Shift offset to the CoW block in the Parent's memory
		iocb->ki_pos = (loff_t)(target_phys_blk * PAGE_SIZE) + (orig_pos & (PAGE_SIZE - 1));

		ret = generic_file_read_iter(iocb, to);

		// Restore logical position
		iocb->ki_pos = orig_pos + ret; 
		return ret;
	}

	// NO MATCH: Read baseline block from Parent's memory
	if (orig_pos >= logical_size) {/*Incase entry not found in logs and >= original_size return 0*/
		return 0; 
	}
	return generic_file_read_iter(iocb, to);
}
//MAHA_AARSH_end

struct uncopied_block* scorw_get_uncopied_block(struct scorw_inode *scorw_inode, unsigned block_num, int processing_type)
{
	int ret = 0;
	struct uncopied_block *uncopied_block = 0;

	////Commentedprintk("[pid: %lu] %s(): Getting blk: %u of child inode: %lu\n", current->pid, __func__, block_num, scorw_inode->i_ino_num);

	//Note:
	//	* We know hash table is a array of linked lists.
	//	* List used to hash an element is calculated using "hash_min(key, HASH_BITS(hashtable))" function
	//	* We are protecting i'th list with i'th spinlock
	//	* Since, list is protected by this spinlock, we don't need to worry about insertion/deletion/lookup
	//	  happening in parallel because if i'th spinlock is acquired, no other function can do insertion, 
	//	  deletion in this list.
	//	  In parallel, insertion/deletion/lookup can happen in other lists of hash table
	//
	spin_lock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(block_num, ilog2(scorw_inode->i_hash_table_size))]));

	//scorw_print_uncopied_blocks_list(scorw_inode);
	uncopied_block = scorw_find_uncopied_block_list(scorw_inode, block_num);
	if(uncopied_block)
	{
		while(!scorw_is_compatible_processing_type(uncopied_block->processing_type, processing_type))
		{
			uncopied_block->num_waiting += 1;
			spin_unlock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(block_num, ilog2(scorw_inode->i_hash_table_size))]));

			//wait_event(uncopied_block->wait_queue, scorw_is_compatible_processing_type(uncopied_block->processing_type, processing_type));
			ret = wait_event_timeout(uncopied_block->wait_queue, scorw_is_compatible_processing_type(uncopied_block->processing_type, processing_type), 10*HZ);
			/*
			   if(ret == 0)
			   {
			   //Commentedprintk("[pid: %d] %s(): waiting to acquire lock for blk: %u\n", current->pid, __func__, block_num);
			   }
			 */

			spin_lock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(block_num, ilog2(scorw_inode->i_hash_table_size))]));
			uncopied_block->num_waiting -=1;
		}
		uncopied_block->processing_type |= processing_type;
		if(processing_type == READING)
		{
			++(uncopied_block->num_readers);
		}
	}
	else
	{
		////Commentedprintk("scorw_get_uncopied_block: New uncopied block allocated\n"); start here
		uncopied_block = scorw_alloc_uncopied_block();
		if(uncopied_block)
		{
			uncopied_block->block_num = block_num;
			uncopied_block->processing_type = processing_type;
			uncopied_block->num_waiting = 0;
			uncopied_block->num_readers = 0;
			if(processing_type == READING)
			{
				uncopied_block->num_readers = 1;
			}
			init_waitqueue_head(&(uncopied_block->wait_queue));

			scorw_add_uncopied_blocks_list(scorw_inode, uncopied_block);
		}
		else
		{
			//Commentedprintk(KERN_ERR "SCORW_OUT_OF_MEMORY: Memory not allocated for uncopied_block\n");
		}
	}
	//scorw_print_uncopied_blocks_list(scorw_inode);
	spin_unlock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(block_num, ilog2(scorw_inode->i_hash_table_size))]));

	return uncopied_block;
}

struct uncopied_block *scorw_find_uncopied_block_list(struct scorw_inode *scorw_inode, unsigned key)
{
	struct uncopied_block *cur = 0;

	////Commentedprintk("Inside scorw_find_uncopied_block_list, inode: %lu. Looking for record with key: %u\n", scorw_inode->i_ino_num, key);
	/*
	   hash_for_each_possible(scorw_inode->i_uncopied_blocks_list, cur, node, key)
	   {
	////Commentedprintk("scorw_find_uncopied_block_list: cur->block_num: %lu, key: %u\n", cur->block_num, key);
	if (cur->block_num == key)
	{
	return cur;
	}
	}
	 */

	hlist_for_each_entry(cur, &scorw_inode->i_uncopied_blocks_list[hash_min(key, ilog2(scorw_inode->i_hash_table_size))], node)
	{
		////Commentedprintk("scorw_find_uncopied_block_list: cur->block_num: %u, key: %u\n", cur->block_num, key);
		if (cur->block_num == key)
		{
			////Commentedprintk("scorw_find_uncopied_block_list: match found!\n");
			return cur;
		}
	}

	////Commentedprintk("scorw_find_uncopied_block_list: No match found!\n");

	return NULL;
}

int scorw_is_compatible_processing_type(int pt1, int pt2)
{
	//readings are compatible with each other
	//reading and copying is compatible
	//copying is not compatible with copying

	if((pt1 & READING) && (pt2 & COPYING))
		return 1;
	if((pt1 & COPYING) && (pt2 & READING))
		return 1;
	if((pt1 & READING) && (pt2 & READING))
		return 1;
	if(pt1 == NOP)
		return 1;
	if(pt2 == NOP)
		return 1;

	return 0;
}

//allocate memory to maintain info about uncopied block being processed
struct uncopied_block *scorw_alloc_uncopied_block(void)
{
	////Commentedprintk("Inside scorw_alloc_uncopied_block\n");
	return ((struct uncopied_block *)kzalloc(sizeof(struct uncopied_block), GFP_NOWAIT));
}

void scorw_add_uncopied_blocks_list(struct scorw_inode *scorw_inode, struct uncopied_block *uncopied_block)
{
	////Commentedprintk("Inside scorw_add_uncopied_blocks_list\n");
	//hash_add((scorw_inode->i_uncopied_blocks_list), &(uncopied_block->node), uncopied_block->block_num);
	//hash_add(hashtable, node, key)
	//hlist_add_head(node, &hashtable[hash_min(key, HASH_BITS(hashtable))])

	////Commentedprintk("scorw_add_uncopied_block_list, inode: %lu. Added record with key: %u\n", scorw_inode->i_ino_num, uncopied_block->block_num);
	hlist_add_head(&(uncopied_block->node), &scorw_inode->i_uncopied_blocks_list[hash_min(uncopied_block->block_num, ilog2(scorw_inode->i_hash_table_size))]);
	//scorw_print_uncopied_blocks_list(scorw_inode);
	////Commentedprintk("Returning from scorw_add_uncopied_blocks_list\n");
}

int scorw_is_block_copied(struct inode *inode, unsigned blk_num)
{
	struct page* page = 0;
	char* kaddr = 0;
	int byte_num= 0;
	int bit_num = 0;
	char byte_value= 0;
	int copy_state = 0;

	////Commentedprintk("scorw_is_block_copied called\n");


	//Single 4KB can store 32768 bits (2^15) i.e. copy status of 2^15 blocks
	//i.e. 12 + 3 = 15 i.e. PAGE_SHIFT+3 blocks info
	page = scorw_get_page(inode, (blk_num/PAGE_BLOCKS));
	if(page == NULL)
	{
		//Commentedprintk(KERN_ERR "Failed to get page\n");
		return -1;
	}
	blk_num = blk_num%PAGE_BLOCKS;

	kaddr = kmap_atomic(page);
	byte_num = ((blk_num)/8);
	bit_num = ((blk_num)%8);
	byte_value = *(kaddr+byte_num);
	copy_state = (byte_value & (1<<bit_num));
	kunmap_atomic(kaddr);

	scorw_put_page(page);

	return copy_state;
}

struct page_copy *scorw_find_page_copy(unsigned block_num, unsigned long par_ino_num, int child_index)
{
	struct page_copy *cur;

	////Commentedprintk("scorw_find_page_copy_hlist: Looking for entry with block_num= %u, parent inode num: %lu, child_index: %d\n" , block_num, par_ino_num, child_index);
	if(child_index == -1)
	{
		//Consider the case when child is open and par isn't open.
		//In this case, child_index of child will be -1.
		//When it tries to do page copy lookup, it should fail.
		//This is because, page copy structs can only exist if a par exists
		return NULL;
	}
	BUG_ON(!(child_index>=0 &&  child_index<SCORW_MAX_CHILDS));
	read_lock(&(page_copy_lock));	
	hash_for_each_possible(page_copy_hlist, cur, h_node, block_num)
	{
		////Commentedprintk("scorw_find_page_copy_hlist: current page copy, block_num= %u, parent inode num: %lu, child_index: %d, is_target_child: %d\n" , cur->block_num, cur->par->i_ino_num, child_index, cur->is_target_child[child_index]);
		if((block_num == cur->block_num) && (par_ino_num == cur->par->i_ino_num) && (cur->is_target_child[child_index]))
		{
			////Commentedprintk("scorw_find_page_copy_hlist: match found!\n");
			read_unlock(&(page_copy_lock));	
			return cur;
		}
	}
	read_unlock(&(page_copy_lock));	
	////Commentedprintk("scorw_find_page_copy_hlist: No match found!\n");

	return NULL;
}

int scorw_copy_page_from_page_copy(struct page_copy *page_copy, struct scorw_inode *scorw_inode)
{

	struct address_space *child_mapping = NULL;
	struct folio *page_w = NULL;
	void *fsdata = 0;
	void *kaddr_w = 0;
	void *kaddr_r = 0;
	int error = 0;
	int i = 0;
	int cold_load = 0;	//Tells whether block_read_full_page() was called or not
	unsigned lblk = 0;
	unsigned long len;	// In native ext4, len means bytes to write to page 
	unsigned long copied;	// In native ext4, copied means bytes copied from user 
				// In our case, len == copied 

				////Commentedprintk("scorw_copy_page_from_page_copy: Inside this fn\n");

	child_mapping = scorw_inode->i_vfs_inode->i_mapping;
	lblk = page_copy->block_num;

	len = (PAGE_SIZE <= (scorw_inode->i_copy_size - (lblk << PAGE_SHIFT)) ? PAGE_SIZE: (scorw_inode->i_copy_size - (lblk << PAGE_SHIFT)));
	copied = len;

	if(len <= 0)
	{
		return 0;
	}


	//read page of parent from disk 
	//Note: Ideally, need to use lock here to avoid the race condition when page copy thread is processing a page copy struct and another code path (say read from child) 
	//is also calling this function
	//For example: Read of child happens and this child (say child 4 of parent) wants to read from page copy. Simultaneously, page copy thread is processing 
	//child 1 of parent.
	//Both find data_page_loaded to be 0 and both should not start reading from disk.
	//
	//Update: lock_page(data_page) below can help to resolve this problem
	if(page_copy->data_page_loaded == 0)
	{
		page_copy->data_page = alloc_pages(GFP_KERNEL, 0);
		BUG_ON(page_copy->data_page == NULL);

		page_copy->data_page->mapping = scorw_inode->i_par_vfs_inode->i_mapping;
		page_copy->data_page->index = page_copy->block_num;

		/*
		   kaddr_r = kmap_atomic(page_copy->data_page);
		 *((char*)kaddr_r + 0) = 'x';
		 *((char*)kaddr_r + 1) = 'y';
		 *((char*)kaddr_r + 2) = 'z';
		 *((char*)kaddr_r + 3) = 'A';
		 //Commentedprintk("[pid: %u]scorw_copy_page_from_page_copy: block: %u, (writing to data page before reading block contents from disk) kaddr_r: %c%c%c%c\n", current->pid, page_copy->block_num, *((char*)kaddr_r + 0), *((char*) kaddr_r + 1), *((char*)kaddr_r + 2), *((char*)kaddr_r + 3));
		 kunmap_atomic(kaddr_r);
		 */
		////Commentedprintk("scorw_copy_page_from_page_copy: block: %u being read from disk\n", page_copy->block_num);

		lock_page(page_copy->data_page);

		block_read_full_folio(page_folio(page_copy->data_page), ext4_get_block);	//upgrade to folio API
		wait_on_page_locked(page_copy->data_page);			//Update: page is unlocked on end-io
		BUG_ON(!PageUptodate(page_copy->data_page));
		page_copy->data_page_loaded = 1;	
		cold_load = 1;
		/*
		   kaddr_r = kmap_atomic(page_copy->data_page);
		   //Commentedprintk("scorw_copy_page_from_page_copy: block: %u being read from disk. First 4 bytes: %c%c%c%c\n", page_copy->block_num, *((char*)kaddr_r + 0), *((char*)kaddr_r + 1), *((char*)kaddr_r + 2), *((char*)kaddr_r + 3));
		   kunmap_atomic(kaddr_r);
		 */
	}

	//write page contents
	////Commentedprintk("scorw_copy_page_from_page_copy: writing page contents\n");
	error = scorw_da_write_begin(NULL, child_mapping, ((unsigned long)lblk) << PAGE_SHIFT, len,&page_w, &fsdata);
	if(page_w != NULL)
	{
		////Commentedprintk("scorw_copy_page_from_page_copy: ext4_da_write_begin returned successfully\n");
		kaddr_r = kmap_atomic(page_copy->data_page);
		kaddr_w = kmap_atomic(&(page_w->page));

		copy_page(kaddr_w, kaddr_r);
		for(i = copied; i < PAGE_SIZE; i++)
		{
			*((char*)kaddr_w + i) = '\0';
		}

		kunmap_atomic(kaddr_r);
		kunmap_atomic(kaddr_w);


		////Commentedprintk("scorw_copy_page_from_page_copy: Before calling ext4_da_write_end called, copied: %d, PageDirty(page_w): %d\n", copied, PageDirty(page_w));
		scorw_da_write_end(NULL, child_mapping , ((unsigned long)lblk) << PAGE_SHIFT, len, copied, page_w, fsdata);
		////Commentedprintk("scorw_copy_page_from_page_copy: After calling ext4_da_write_end called, copied: %d, PageDirty(page_w): %d\n", copied, PageDirty(page_w));
		////Commentedprintk("scorw_copy_page_from_page_copy: balance_dirty_pages_ratelimited called\n");
		balance_dirty_pages_ratelimited(child_mapping);
	}
	else
	{
		//Commentedprintk("scorw_copy_page_from_page_copy: Error in ext4_da_write_begin\n");
		if(cold_load)
		{
			lock_page(page_copy->data_page);	
			try_to_free_buffers(page_folio(page_copy->data_page));
			unlock_page(page_copy->data_page);	
		}
		page_copy->data_page->mapping = NULL;
		return error;
	}
	if(cold_load)
	{
		lock_page(page_copy->data_page);	
		try_to_free_buffers(page_folio(page_copy->data_page));
		unlock_page(page_copy->data_page);	
	}
	page_copy->data_page->mapping = NULL;
	////Commentedprintk("scorw_copy_page_from_page_copy: Returning from this fn\n");

	return 0;
}

void scorw_set_block_copied(struct scorw_inode* scorw_inode, unsigned blk_num)
{
	struct page* page = 0;
	char* kaddr = 0;
	int byte_num= 0;
	int bit_num = 0;
	int ret = 0;
	struct inode *inode = scorw_inode->i_frnd_vfs_inode;
	//struct address_space *mapping = inode->i_mapping;

	////Commentedprintk("scorw_set_block_copied called\n");


	//Single 4KB can store 32768 bits (2^15) i.e. copy status of 2^15 blocks
	//i.e. 12 + 3 = 15 i.e. PAGE_SHIFT+3 blocks info
	page = scorw_get_page(inode, (blk_num/PAGE_BLOCKS));
	if(page == NULL)
	{
		//Commentedprintk(KERN_ERR "Failed to get page\n");
	}

	////Commentedprintk("%s(): page: %u obtained. Has buffers? %d\n", __func__, (blk_num/PAGE_BLOCKS), page_has_buffers(page));
	//new start
	if(!page_has_buffers(page))
	{
		////Commentedprintk("%s(): page: %u doesn't have buffers\n", __func__, (blk_num/PAGE_BLOCKS));
		lock_page(page);
		// In case writeback began while the page was unlocked 
		wait_for_stable_page(page);

		//needed to create buffer heads and fill them with physical addresses
		//will be needed during writeback
		//Don't perform read operation, so, passed PAGE_SIZE as length
		ret = __block_write_begin(page_folio(page), page->index << PAGE_SHIFT, PAGE_SIZE, ext4_da_get_block_prep);
		if(ret < 0)
		{
			unlock_page(page);
			scorw_put_page(page);
			//Commentedprintk("%s(): Error inside __block_write_begin\n", __func__);
			BUG_ON(ret<0);
		}

		unlock_page(page);
	}
	//new end

	blk_num = blk_num%PAGE_BLOCKS;

	kaddr = kmap_atomic(page);
	byte_num = ((blk_num)/8);
	bit_num = ((blk_num)%8);
	//Todo: want to update byte atomically. set_bit() is too expensive. Find an alternative.
	//set_bit(bit_num, (unsigned long *)(kaddr+byte_num));
	//*(kaddr+byte_num) |= (1<<bit_num);
	////Commentedprintk("%s(): Before: frnd byte: %d, value: %x\n", __func__, byte_num, *(char*)(kaddr+byte_num));
	scorw_set_bit(bit_num, (char*)(kaddr+byte_num));
	////Commentedprintk("%s(): After: frnd byte: %d, value: %x\n", __func__, byte_num, *(char*)(kaddr+byte_num));
	kunmap_atomic(kaddr);

	if(!PageDirty(page))
	{
		////Commentedprintk("%s(): page: %u is not dirty. Setting it dirty.\n", __func__, page->index);
		lock_page(page);
		scorw_set_page_dirty(page);
		unlock_page(page);
	}

	scorw_put_page(page);
}

struct page* scorw_get_page(struct inode* inode, loff_t lblk)
{
	struct address_space *mapping = NULL;
	struct page *page = NULL;
	//void *kaddr = NULL;
	int error = 0;
	loff_t isize = i_size_read(inode);

	mapping = inode->i_mapping;

	/*Fast approach (avoids locking of page)*/
	page = find_get_page(mapping, lblk);
	if (page && PageUptodate(page))
	{
		return page;
	}

	/*Fall back approach */

	//returns a locked page
	page = find_or_create_page(mapping, lblk, mapping_gfp_mask(mapping));
	if(page == NULL)
	{
		//current->backing_dev_info = NULL;
		//Commentedprintk(KERN_ERR "Error!! Failed to allocate page. Out of memory!!\n");
		return NULL;
	}

	/* Adding logic incase a page is newly created */
	if(lblk >= (isize >> PAGE_SHIFT))
	{
		memset(page_address(page), 0, PAGE_SIZE);
		//Commentedprintk("[DEBUG] :: {%s} :: Requested block number: %lu is greater than logical blocks in file. Returning empty page\n", __func__, lblk);
		SetPageUptodate(page);
		unlock_page(page);
		return page;
	}


	//If parent's page is already in cache, don't read it from disk
	if(PageUptodate(page))
	{
		unlock_page(page);
	}
	else
	{
		//parent's page is not in cache, read it from disk
		//calling ext4_readpage. ext4_readpage calls ext4_mpage_readpages internally.
		error = mapping->a_ops->read_folio(NULL, page_folio(page));
		if(error)
		{
			//Commentedprintk(KERN_ERR "scorw_get_page: Error while Reading parent's page into pagecache\n");
			put_page(page);
			//current->backing_dev_info = NULL;
			//inode_unlock(scorw_inode->i_vfs_inode);
			return NULL;
		}      

		if(!PageUptodate(page))
		{
			folio_lock_killable(page_folio(page));
			unlock_page(page);
		}
	}
	return page;
}

void scorw_put_page(struct page* page)
{
	put_page(page);
}

void scorw_set_bit(u8 bitnum, volatile u8 *p)
{
	BUG_ON(bitnum > 7);
	u8 bitmask = 1 << bitnum;
	////Commentedprintk("%s(): p: %lx, bitmask: %x\n", __func__, p, bitmask);
	asm volatile(LOCK_PREFIX "orb %b1,%0"
			: "+m" (*(volatile char*)(p))
			: "iq" (bitmask)
			: "memory");
}

int scorw_set_page_dirty(struct page *page)
{
	//WARN_ON_ONCE(!PageLocked(page) && !PageDirty(page));
	//WARN_ON_ONCE(!page_has_buffers(page));
	//return __set_page_dirty_buffers(page); changed in new kernel
	return set_page_dirty(page);
}


void scorw_dec_yet_to_copy_blocks_count(struct scorw_inode* scorw_inode, unsigned n){
	//scorw_inode->i_pending_copy_pages -= n;
	atomic64_sub(n, &(scorw_inode->i_pending_copy_pages)); 
	////Commentedprintk("scorw_dec_yet_to_copy_blocks_count: After: scorw_inode->i_pending_copy_pages: %u\n", scorw_inode->i_pending_copy_pages);
}

ssize_t submit_read_request(struct scorw_inode* scorw_inode, struct kiocb *iocb, struct iov_iter *to, enum batching_type prev_batching_type, unsigned batch_start_blk, unsigned batch_end_blk)
{
	ssize_t ret = 0;        //return value

	if(prev_batching_type == PRESENT_IN_PARENT)
	{
		ret = scorw_read_from_parent(scorw_inode, iocb, to, batch_start_blk, batch_end_blk);
	}
	else if(prev_batching_type == PRESENT_IN_CHILD)
	{
		ret = scorw_read_from_child(iocb, to, batch_start_blk, batch_end_blk);
	}
	else
	{
		BUG();
	}

	return ret;
}

int scorw_put_uncopied_block(struct scorw_inode *scorw_inode, unsigned key, int processing_type, struct uncopied_block* ptr_uncopied_block)
{
	int updated_processing_type = 0;
	struct uncopied_block *uncopied_block = 0;

	////Commentedprintk("[pid: %lu] %s(): Putting blk: %u for child inode: %lu\n", current->pid, __func__, key, scorw_inode->i_ino_num);

	spin_lock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(key, ilog2(scorw_inode->i_hash_table_size))]));

	//Note: This lookup is important because it is possible that
	//	two threads can simultaneously call this function + scorw_remove_uncopied_block function for same block.
	//	As a result, uncopied block can be freed before this function runs resulting
	//	in use after free problem.
	//
	uncopied_block = scorw_find_uncopied_block_list(scorw_inode, key);
	if(uncopied_block == NULL)
	{
		spin_unlock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(key, ilog2(scorw_inode->i_hash_table_size))]));
		return -1;
	}

	if(processing_type == READING)
	{
		--(uncopied_block->num_readers);

		if((uncopied_block->num_readers) > 0)
		{
			spin_unlock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(key, ilog2(scorw_inode->i_hash_table_size))]));
			return 0;
		}
	}
	updated_processing_type = ~(processing_type);
	updated_processing_type &= uncopied_block->processing_type;
	uncopied_block->processing_type = updated_processing_type;

	if((uncopied_block->processing_type == NOP) &&  (uncopied_block->num_waiting != 0))
	{
		wake_up(&(uncopied_block->wait_queue));
	}
	/*
	   else
	   {
	   //Commentedprintk("[pid: %d] %s(): Decided not to call wakeup for blk num: %lu, processing type: %u, num waiters: %u, child inode: %lu\n", current->pid, __func__, key, uncopied_block->processing_type, uncopied_block->num_waiting, scorw_inode->i_ino_num);
	   }
	 */

	spin_unlock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(key, ilog2(scorw_inode->i_hash_table_size))]));
	return 0;
}

int scorw_remove_uncopied_block(struct scorw_inode *scorw_inode, unsigned key, struct uncopied_block *ptr_uncopied_block)
{
	struct uncopied_block *uncopied_block = 0;

	////Commentedprintk("Inside %s()\n", __func__);
	spin_lock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(key, ilog2(scorw_inode->i_hash_table_size))]));

	//Note: This lookup is important because it is possible that
	//	two threads can simultaneously call this function for same block.
	//	As a result, double free can occur if we don't perform this lookup
	//	and directly free the uncopied block passed as the argument
	//
	uncopied_block = scorw_find_uncopied_block_list(scorw_inode, key);
	if(uncopied_block == NULL)
	{
		spin_unlock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(key, ilog2(scorw_inode->i_hash_table_size))]));
		return 0;
	}
	else if((uncopied_block->processing_type == NOP) &&  (uncopied_block->num_waiting == 0))
	{
		scorw_remove_uncopied_blocks_list(uncopied_block);
		scorw_free_uncopied_block(uncopied_block);
	}
	spin_unlock(&(scorw_inode->i_uncopied_blocks_lock[hash_min(key, ilog2(scorw_inode->i_hash_table_size))]));

	return 0;
}

void scorw_remove_uncopied_blocks_list(struct uncopied_block* uncopied_block)
{
	////Commentedprintk("Inside scorw_remove_uncopied_blocks_list\n");
	////Commentedprintk("scorw_remove_uncopied_block_list, Removing record with key: %u\n", uncopied_block->block_num);
	hash_del(&(uncopied_block->node));
	////Commentedprintk("Returning from scorw_remove_uncopied_blocks_list\n");
}

void scorw_free_uncopied_block(struct uncopied_block *uncopied_block)
{
	////Commentedprintk("Inside scorw_free_uncopied_block\n");
	kfree(uncopied_block);
}

#ifdef USE_OLD_RANGE
int scorw_write_child_blocks_begin(struct inode* inode, loff_t offset, size_t len, void **ptr_uncopied_block)
#else
int scorw_write_child_blocks_begin(struct inode* inode, loff_t offset, size_t len, void **ptr_uncopied_block, struct sharing_range_info *shr_info)
#endif
{
	struct scorw_inode *scorw_inode = NULL;
	unsigned start_block = 0;
	unsigned end_block = 0;
	unsigned last_block_eligible_for_copy = 0;
	unsigned first_blk_num = 0;
	unsigned last_blk_num = 0;
	int i = 0;
	struct uncopied_block *uncopied_block = NULL;
	struct page_copy *page_copy = 0;
#ifndef USE_OLD_RANGE
	struct child_range *cr = NULL;
#endif

	scorw_inode = scorw_find_inode(inode);
	BUG_ON(!scorw_inode || scorw_inode->i_vfs_inode != inode);

	//first and last block (inclusive) covered by range i.e. offset, len passed to this function
	first_blk_num = (offset >> PAGE_SHIFT);
	last_blk_num = ((offset+len-1)>>PAGE_SHIFT);
	last_block_eligible_for_copy  = ((scorw_inode->i_copy_size-1) >> PAGE_SHIFT);
	////Commentedprintk("scorw_write_child_blocks_begin: offset: %lld, offset+len: %lld\n", offset, offset+len);
	////Commentedprintk("scorw_write_child_blocks_begin: first_blk_num: %u, last_blk_num: %u, last_block_eligible_for_copy: %u\n", first_blk_num, last_blk_num, last_block_eligible_for_copy);

	start_block = first_blk_num;
	end_block = (last_blk_num < last_block_eligible_for_copy ?  last_blk_num : last_block_eligible_for_copy);
	////Commentedprintk("scorw_write_child_blocks_begin: start_block: %u, end_block: %u\n", start_block, end_block);


	//This write is purely append operation. Nothing to be done by us.
	if(start_block > last_block_eligible_for_copy)
	{
		////Commentedprintk("scorw_write_child_blocks_begin: This portion of write is purely append operation. Nothing to be done by us. \n");
		return 0;
	}

#ifndef USE_OLD_RANGE
	for(i= start_block; i<=end_block; ++i){
		WriteCh snxb;	
		if(!cr && i != start_block) 
			break;	     
		snxb = snapx_get_range_info(&cr, scorw_inode, i);

		//Check if we were handling a stream of shared mode write
		if(snxb != SNAPX_FLAG_SHARED && shr_info->initialized){
			WARN_ON(1);   //Mixed write is not handled. We will perform a partial write prep and go back
			end_block = i -1;  
			break;
		}

		if(snxb == SNAPX_FLAG_COW_RO || snxb == SNAPX_FLAG_SEE_TH_RO || snxb == SNAPX_FLAG_SHARED_RO)
			return -1; 
		if(snxb == SNAPX_FLAG_SHARED) { // we need to maintain a shared range and handle it latter
			if(i != start_block && !shr_info->initialized){
				WARN_ON(1);   //Mixed write to CoW is not handled. We will perform a partial write prep and go back
				end_block = i -1;  
				shr_info->partial_cow = true;
				shr_info->end_block = end_block;
				shr_info->start_block = start_block;
				break;
			}else if(shr_info->initialized){
				shr_info->end_block++;     
			}else if(i == start_block){
				BUG_ON(shr_info->initialized);   
				shr_info->initialized = true;
				shr_info->start_block = i;
				shr_info->end_block = i;
			}
		}   
	}
#endif


	for(i = start_block; i <= end_block; )
	{
		////Commentedprintk("scorw_write_child_blocks_begin: calling scorw_get_uncopied_block\n");
		uncopied_block = scorw_get_uncopied_block(scorw_inode, i, COPYING);
		*ptr_uncopied_block = uncopied_block;	//This uncopied block will be passed to write_end fn
							////Commentedprintk("scorw_write_child_blocks_begin: sizeof(uncopied_block): %d\n", sizeof(struct uncopied_block));

#ifndef USE_OLD_RANGE
		//we don't perform cow for shared blks. We are just interested in acquiring active map locks
		if(shr_info->initialized){
			i++;
			continue;
		}	
#endif
		////Commentedprintk("scorw_write_child_blocks_begin: start_block: %u, i: %u, end_block: %u\n", start_block, i, end_block);
		if(scorw_is_block_copied(scorw_inode->i_frnd_vfs_inode, i))
		{
			////Commentedprintk("scorw_write_child_blocks_begin: Not copying block: %u because it is already copied\n", i);
			scorw_put_uncopied_block(scorw_inode, i, COPYING, uncopied_block);
			scorw_remove_uncopied_block(scorw_inode, i, uncopied_block);
			i++;
			continue;
		}

		//increment child version count on first write to a blk
		//that is not copied to child yet because only then
		//friend file is also modified
		scorw_child_version_cnt_inc(inode);

		if((start_block == end_block)  && (i == start_block) && ((offset%PAGE_SIZE) == 0) && (((offset+len)%PAGE_SIZE) == 0))
		{
			//Note: For such cases also, i_pending_copy_pages is decreased.
			////Commentedprintk("scorw_write_child_blocks_begin: first block: %u is equal to end block: %u and it will be fully overwritten\n", start_block, end_block);
			i++;
			continue;
		}
		if((start_block != end_block) && (i == start_block) && ((offset%PAGE_SIZE) == 0))
		{
			//Note: For such cases also, i_pending_copy_pages is decreased.
			////Commentedprintk("scorw_write_child_blocks_begin: Not copying first block: %u because it will be fully overwritten\n", i);
			i++;
			continue;
		}
		if((start_block != end_block) && (i == end_block) && (((offset+len)%PAGE_SIZE) == 0))
		{
			//Note: For such cases also, i_pending_copy_pages is decreased.
			////Commentedprintk("scorw_write_child_blocks_begin: Not copying last block: %u because it will be fully overwritten\n", i);
			i++;
			continue;
		}
		if((i==start_block) || (i == end_block))
		{
			////Commentedprintk("scorw_write_child_blocks_begin: block i: %u will be copied\n", i);

			page_copy = scorw_find_page_copy(i, scorw_inode->i_par_vfs_inode->i_ino, scorw_inode->i_at_index);
			if(page_copy != NULL)	//page copy exists
			{
				//Block hasn't been alraedy copied to child. This, implies that it is safe to copy block from page copy.
				////Commentedprintk("scorw_write_child_blocks_begin: block i: %u will be copied from page copy\n", i);
				scorw_copy_page_from_page_copy(page_copy, scorw_inode);	
			}
			else	//page copy does not exist i.e with parent
			{
				//Block has not been already copied. Page copy doesn't exists. Only possible if page copy was never created. So, read from parent.
				//copy page from parent
				////Commentedprintk("scorw_write_child_blocks_begin: block i: %u will be copied from parent\n", i);
				scorw_copy_page(scorw_inode, i, scorw_inode->i_par_vfs_inode->i_mapping);
			}
		}

		i++;
	}
	return 0;
}

#ifndef USE_OLD_RANGE
//Returns the snapx write behavior for a child inode
static int snapx_get_range_info(struct child_range **cr, struct scorw_inode *scorw_inode, unsigned blk_num)
{

	if(!scorw_inode->i_num_ranges)
		return SNAPX_FLAG_COW;

	if(*cr == NULL || (*cr)->end < blk_num){ //No prev history, or stale history
		int i;
		for(i=0; i < scorw_inode->i_num_ranges; i++){
			if((scorw_inode->i_range[i].start <= blk_num) && (blk_num <= scorw_inode->i_range[i].end)){ 
				*cr = &scorw_inode->i_range[i];
				break;
			}
		}
	}
	if(*cr == NULL) //Its the default behavior
		return SNAPX_FLAG_COW;
	return (*cr)->snapx_behavior; 
}
#endif

void scorw_child_version_cnt_inc(struct inode *inode)
{
	unsigned long child_version_val = 0;
	struct wait_for_commit *wait_for_commit = 0;

	if(atomic_xchg(&(inode->i_cannot_update_child_version_cnt), 1) == 0)
	{
		//A freshly created frnd inode will have sync frnd flag as 0.
		//We know, this flag in frnd is allowed to have only 2 values: {-1, 1}
		//At this point, frnd is not dirty. But after this step (write to child)
		//frnd will become dirty.
		//We are initializing this flag to -1 to indicate that
		//this frnd inode is currently not allowed to sync
		inode->i_scorw_inode->i_frnd_vfs_inode->i_can_sync_frnd = -1;


		//update child version cnt
		child_version_val = scorw_get_child_version_attr_val(inode);
		BUG_ON(child_version_val == 0);

		++child_version_val;

		scorw_set_child_version_attr_val(inode, child_version_val);

		//queue frnd info for setting of flag that allows frnd inode/blks syncing
		wait_for_commit = scorw_alloc_wait_for_commit();
		BUG_ON(wait_for_commit == NULL);

		wait_for_commit->frnd = inode->i_scorw_inode->i_frnd_vfs_inode;
		wait_for_commit->child_staged_for_commit = 0;

		spin_lock(&commit_lock);
		scorw_add_wait_for_commit_list(wait_for_commit);
		spin_unlock(&commit_lock);
	}	
}

void scorw_set_child_version_attr_val(struct inode *inode, unsigned long version_val)
{
	////Commentedprintk("Inside scorw_set_child_version_attr_val\n");
	ext4_xattr_set(inode, 1, version, &version_val, sizeof(unsigned long), 0);

}


int scorw_copy_page(struct scorw_inode *scorw_inode, loff_t lblk, struct address_space *p_mapping)
{
	//struct address_space *par_mapping = NULL;
	struct address_space *child_mapping = NULL;
	struct page *page = NULL;
	struct folio *page_w = NULL;
	void *fsdata = 0;
	int error = 0;
	int i = 0;
	unsigned long len;	/* In native ext4, len means bytes to write to page */
	unsigned long copied;	/* In native ext4, copied means bytes copied from user */
	/* In our case, len == copied */


	//parent's page 
	page = scorw_get_page(scorw_inode->i_par_vfs_inode, lblk);
	/*
	////Commentedprintk("scorw_copy_page: Reading 1 page of parent\n");
	par_mapping = p_mapping;
	////Commentedprintk("scorw_copy_page: par_mapping: %x, page index: %lu\n", par_mapping, lblk);

	//returns a locked page
	page = find_or_create_page(par_mapping, lblk, mapping_gfp_mask(par_mapping));
	if(page == NULL)
	{
	//Commentedprintk("Error!! Failed to allocate page. Out of memory!!\n");
	return SCORW_OUT_OF_MEMORY;
	}


	//If parent's page is already in cache, don't read it from disk
	if(PageUptodate(page))
	{
	unlock_page(page);
	}
	else
	{
	////Commentedprintk("scorw_copy_page: Reading parent's page into pagecache\n");
	//parent's page is not in cache, read it from disk
	//calling ext4_readpage. ext4_readpage calls ext4_mpage_readpages internally.
	error = par_mapping->a_ops->readpage(NULL, page);
	if(error)
	{
	//Commentedprintk("scorw_copy_page: Error while Reading parent's page into pagecache\n");
	put_page(page);
	return error;
	}      

	//Looks like this is reqd. to make sure that read operation completes
	//i.e. block until read completes. 
	////Commentedprintk("scorw_copy_page: PageUptodate(read page): %d\n", PageUptodate(page));
	if(!PageUptodate(page))
	{
	////Commentedprintk("scorw_copy_page: locking read page.\n"); 
	lock_page_killable(page);

	////Commentedprintk("scorw_copy_page: unlocking read page\n"); 
	unlock_page(page);
	}
	}
	 */

	////Commentedprintk("scorw_copy_page: obtaining child mapping\n"); 
	child_mapping = scorw_inode->i_vfs_inode->i_mapping;


	len = (PAGE_SIZE <= (scorw_inode->i_copy_size - (lblk << PAGE_SHIFT)) ? PAGE_SIZE: (scorw_inode->i_copy_size - (lblk << PAGE_SHIFT)));
	copied = len;
	////Commentedprintk("scorw_copy_page: Amount of data copied/written to page: %lu\n", copied); 

	////Commentedprintk("scorw_copy_page: calling ext4_da_write_begin\n"); 

	//error = ext4_da_write_begin(NULL, child_mapping, ((unsigned long)lblk) << PAGE_SHIFT, len, 0, &page_w, &fsdata);
	error = scorw_da_write_begin(NULL, child_mapping, ((unsigned long)lblk) << PAGE_SHIFT, len , &page_w, &fsdata);
	if(page_w != NULL)
	{
		////Commentedprintk("scorw_copy_page: Page allocated by ext4_da_write_begin()\n");

		////Commentedprintk("scorw_copy_page: Mapping parent's page using kmap_atomic\n");
		////Commentedprintk("scorw_copy_page: Mapping child's page using kmap_atomic\n");
		void *kaddr = kmap_atomic(page);
		void *kaddr_w = kmap_atomic(&(page_w->page));

		////Commentedprintk("page[0]: %d\n", *((char*)kaddr + 0));
		////Commentedprintk("page[1]: %d\n", *((char*)kaddr + 1));
		////Commentedprintk("page[2]: %d\n", *((char*)kaddr + 2));
		////Commentedprintk("page[3]: %d\n", *((char*)kaddr + 3));
		////Commentedprintk("page[4095]: %d\n", *((char*)kaddr + 4095));
		copy_page(kaddr_w, kaddr);

		for(i = copied; i < PAGE_SIZE; i++)
		{
			*((char*)kaddr_w + i) = '\0';
		}

		////Commentedprintk("scorw_copy_page: unmapping parent's page using kunmap_atomic\n");
		////Commentedprintk("scorw_copy_page: unmapping child's page using kunmap_atomic\n");
		kunmap_atomic(kaddr);
		kunmap_atomic(kaddr_w);

		////Commentedprintk("scorw_copy_page: calling ext4_da_write_end\n"); 
		//ext4_da_write_end(NULL, child_mapping , ((unsigned long)lblk) << PAGE_SHIFT, len, copied, page_w, fsdata);
		scorw_da_write_end(NULL, child_mapping , ((unsigned long)lblk) << PAGE_SHIFT, len, copied, page_w, fsdata);
		////Commentedprintk("scorw_copy_page: returning from ext4_da_write_end\n"); 

		balance_dirty_pages_ratelimited(child_mapping);

	}
	else
	{
		//Commentedprintk("scorw_copy_page: Error in ext4_da_write_begin\n");
		scorw_put_page(page);

		return error;
	}
	////Commentedprintk("scorw_copy_page: put_page() called\n");
	scorw_put_page(page);


	return 0;

}

#ifdef USE_OLD_RANGE
int scorw_write_child_blocks_end(struct inode* inode, loff_t offset, size_t len, struct uncopied_block *uncopied_block)
#else
int scorw_write_child_blocks_end(struct inode* inode, loff_t offset, size_t len, struct uncopied_block *uncopied_block, bool shared)
#endif
{
	struct scorw_inode *scorw_inode = NULL;
	unsigned start_block = 0;
	unsigned end_block = 0;
	unsigned last_block_eligible_for_copy = 0;
	unsigned first_blk_num = 0;
	unsigned last_blk_num = 0;
	int i = 0;

	scorw_inode = scorw_find_inode(inode);
	BUG_ON(!scorw_inode || scorw_inode->i_vfs_inode != inode);

	////Commentedprintk("scorw_write_child_blocks_end called\n");
	first_blk_num = (offset >> PAGE_SHIFT);
	last_blk_num = ((offset+len-1)>>PAGE_SHIFT);
	last_block_eligible_for_copy  = ((scorw_inode->i_copy_size-1) >> PAGE_SHIFT);
	start_block = first_blk_num;
	end_block = (last_blk_num < last_block_eligible_for_copy ?  last_blk_num : last_block_eligible_for_copy);
	////Commentedprintk("scorw_write_child_blocks_end: start_block: %u, end_block: %u\n", start_block, end_block);


	//This write is purely append operation. Nothing to be done by us.
	if(start_block > last_block_eligible_for_copy)
	{
		////Commentedprintk("scorw_write_child_blocks_end: This portion of write is purely append operation. Nothing to be done by us.\n");
		return 0;
	}


	for(i = start_block; i <= end_block; )
	{

		////Commentedprintk("scorw_write_child_blocks_end: start_block: %u, i: %u, end_block: %u\n", start_block, i, end_block);
		if(scorw_is_block_copied(scorw_inode->i_frnd_vfs_inode, i))
		{
			////Commentedprintk("scorw_write_child_blocks_end: Not copying block: %u because it is already copied\n", i);
			i++;
			continue;
		}

#ifndef USE_OLD_RANGE
		if(!shared)
#endif
			scorw_set_block_copied(scorw_inode, i);
		////Commentedprintk("scorw_write_child_blocks_end: checking whether block is set as copied: %d\n", (scorw_is_block_copied(scorw_inode, i)));

		//decrement count of remaining blocks to be copied
		////Commentedprintk("scorw_write_child_blocks_end: Before: scorw_inode->i_pending_copy_pages: %u\n", scorw_inode->i_pending_copy_pages);
		scorw_dec_yet_to_copy_blocks_count(scorw_inode, 1);
		////Commentedprintk("scorw_write_child_blocks_end: After: scorw_inode->i_pending_copy_pages: %u\n", scorw_inode->i_pending_copy_pages);


		scorw_put_uncopied_block(scorw_inode, i, COPYING, uncopied_block);
		scorw_remove_uncopied_block(scorw_inode, i, uncopied_block);

		i++;
	}

	return 0;
}

void scorw_init(void)
{
	int error = 0;
	int ret = 0;
	scorw_thread = NULL;
	inode_policy = NULL;
	extent_policy = NULL;

	////Commentedprintk("************************ Hello from scorwExt4 ******************************* \n");


	hash_init(page_copy_hlist);
	//start threads
	//scorw_thread_init();
	ret = scorw_page_copy_thread_init();
	if(ret < 0)
	{
		//Commentedprintk("Error while initialising page copy thread\n");
	}

	/*Creating a directory in /sys/ */
	kobj_scorw = kobject_create_and_add("corw_sparse", NULL);

	/*Creating sysfs file*/
	if(kobj_scorw)
	{
		//Commentedprintk("scorw_init: created corw_sparse sysfs directory successfully");
		error = sysfs_create_file(kobj_scorw, &frnd_file_enable_recovery_attr.attr);
		if(error)
		{
			//Commentedprintk(KERN_ERR "scorw_init: Error while creating enable_recovery sysfs file\n");
			sysfs_remove_file(kobj_scorw, &frnd_file_enable_recovery_attr.attr);
			kobject_put(kobj_scorw);
		}
		else
		{
			//Commentedprintk("scorw_init: created enable_recovery file within corw_sparse sysfs directory successfully");
		}

		error = sysfs_create_file(kobj_scorw, &frnd_file_last_recovery_time_us_attr.attr);
		if(error)
		{
			//Commentedprintk(KERN_ERR "scorw_init: Error while creating last_recovery_time_ms sysfs file\n");
			sysfs_remove_file(kobj_scorw, &frnd_file_last_recovery_time_us_attr.attr);
			kobject_put(kobj_scorw);
		}
		else
		{
			//Commentedprintk("scorw_init: created last_recovery_time_ms within corw_sparse sysfs directory successfully");
		}
	}
	else
	{
		//Commentedprintk("scorw_init: Failed to create corw_sparse sysfs directory successfully. Does it already exists?");
	}


	////////////////////////////////////////////////////////
	//exported symbols (functions pointers) initialisation
	////////////////////////////////////////////////////////

	//If this function pointer(get_child_inode_num) is set, it enables the code in fs/fs-writeback.c that handles ordering of 
	//friend file and child files blocks.
	////
	//get_child_inode_num = scorw_get_child_friend_attr_val;
	get_child_inode_num = NULL;

	//If this function pointer(is_par_inode) is set, it enables the code is fs/fs-writeback.c that handles the ordering of
	//parent file blocks and child files blocks
	is_par_inode = scorw_is_par_file;
	//is_par_inode = NULL;

	//If this function pointer(is_par_inode) is set, it enables the code is fs/ext4-module/inode.c that handles the 
	//optimization of the code that handles the sparse files (ext4_da_map_blocks())
	is_child_inode = scorw_is_child_file;

	//For a parent file, find its children's inode numbers (if they exist)
	//Must set this function pointer to an actual function if is_par_inode is set
	get_child_i_attr_val = scorw_get_child_i_attr_val;

	//Find friend file inode num corresponding a child file
	//Must set this function pointer to an actual function if is_par_inode is set
	get_friend_attr_val = scorw_get_friend_attr_val;

	//Find whether a block is copied as per friend file or not
	//Must set this function pointer to an actual function if is_par_inode is set
	is_block_copied = scorw_is_block_copied;

	//Find whether page copy corresponding a block exists or not.
	//Must set this function pointer to an actual function if is_par_inode is set
	find_page_copy = scorw_find_page_copy;

	//unmount
	__scorw_exit = scorw_exit;

	is_child_file = scorw_is_child_file;
	is_par_file = scorw_is_par_file;
	unlink_child_file = scorw_unlink_child_file;
	unlink_par_file = scorw_unlink_par_file;

	init_waitqueue_head(&sync_child_wait_queue);
}

//Todo: Unlink associated frnd file on the deletion of child file
int scorw_unlink_child_file(struct inode *c_inode)
{
	int i = 0;
	struct inode *p_inode = 0;
	struct scorw_inode *c_scorw_inode = 0;
	struct scorw_inode *p_scorw_inode = 0;
	unsigned long c_ino_num = 0;
	unsigned long p_ino_num = 0;

	////Commentedprintk("Inside %s(). child unlinked: %lu\n", __func__, c_inode->i_ino);

	//Freeze the existance of child and par inode.s i.e. prevent opening/closing of child inode in parallel
	mutex_lock(&(c_inode->i_vfs_inode_open_close_lock));

	p_ino_num = scorw_get_parent_attr_val(c_inode);
	p_inode = ext4_iget(c_inode->i_sb, p_ino_num, EXT4_IGET_NORMAL);
	if(IS_ERR_VALUE(p_inode))
	{
		//Commentedprintk("Error: scorw_unlink_child_file: p_inode: %lu (After iget)\n", (unsigned long)p_inode);
		mutex_unlock(&(c_inode->i_vfs_inode_open_close_lock));
		//iput(p_inode);	//Since, iget() failed, there should be no iput()
		return -1;
	}
	mutex_lock(&(p_inode->i_vfs_inode_open_close_lock));


	c_scorw_inode = c_inode->i_scorw_inode;
	p_scorw_inode = p_inode->i_scorw_inode;

	//child scorw inode exists
	//To keep things simple, let's perform cleanup when child scorw inode is closed
	//Note: This can be further optimized
	if(c_scorw_inode)
	{
		//mark child scorw inode as orphan
		////Commentedprintk("%s(): marked child: %lu scorw as orphan\n", __func__, c_inode->i_ino);
		c_scorw_inode->i_ino_unlinked = 1;

		//parent is open while child is being deleted
		//Note: We only handle the case for parent being open and child
		//scorw inode being open due to parent.
		//i.e. We donot handle the case for parent and child files
		//both being open and simulatenously child getting deleted.
		//
		//For now, we can assume that child deletion is disallowed in that
		//case.
		if(p_scorw_inode)
		{
			mutex_unlock(&(c_inode->i_vfs_inode_open_close_lock));
			//put on behalf of parent
			scorw_put_inode(c_inode, 1, 0, 0);
		}
		//parent is not open while child is being deleted
		else
		{
			mutex_unlock(&(c_inode->i_vfs_inode_open_close_lock));
		}
	}
	else
	{
		//Neither child nor parent is open. Delete extended attributes related to child stored in parent.
		////Commentedprintk("%s(): child: %lu scorw is not open. This implies neither child nor parent is open\n", __func__, c_inode->i_ino);
		for(i = 0; i < SCORW_MAX_CHILDS; i++)
		{
			c_ino_num = scorw_get_child_i_attr_val(p_inode, i);
			////Commentedprintk("%s(): child attribute %d has inode num: %lu\n", __func__, i, c_ino_num);
			if(c_ino_num == c_inode->i_ino)
			{
				////Commentedprintk("%s(): corresponding attribute in parent found. Removing this attribute.\n", __func__);
				scorw_remove_child_i_attr(p_inode, i);
				break;
			}
		}
		mutex_unlock(&(c_inode->i_vfs_inode_open_close_lock));
	}
	mutex_unlock(&(p_inode->i_vfs_inode_open_close_lock));
	iput(p_inode);

	return 0;
}

int scorw_unlink_par_file(struct inode *inode)
{

	/*
	   int i;
	   struct inode *c_inode;
	   struct scorw_inode *p_scorw_inode;
	   struct scorw_inode *c_scorw_inode;

	////Commentedprintk("scorw_unlink_par_file called\n");

	////Commentedprintk("scorw_unlink_par_file: finding/creating scorw inode of parent file\n");
	p_scorw_inode = scorw_get_inode(inode, 0, 0);
	mutex_lock(&(p_scorw_inode->i_lock));


	////Commentedprintk("scorw_unlink_par_file: Copying pending data from parent to children.\n");
	////Commentedprintk("scorw_unlink_par_file: Removing parent's info saved in child inodes.\n");
	for(i = 0; i < SCORW_MAX_CHILDS; i++)
	{
	////Commentedprintk("scorw_unlink_par_file: Processing child %d.\n", i);
	if((c_inode = p_scorw_inode->i_child_vfs_inode[i]) == NULL)
	continue;

	////Commentedprintk("scorw_unlink_par_file: inode number of child %d: %lu.\n", i, c_inode->i_ino);
	c_scorw_inode = scorw_find_inode(c_inode);

	//Copy pending data from parent to children.
	////Commentedprintk("scorw_unlink_par_file: Copying pending data from parent to child %d.\n", i);
	scorw_copy_blocks(c_scorw_inode, 0, ((c_scorw_inode->i_copy_size-1)/PAGE_SIZE));

	//Remove parents info saved in child inode's
	////Commentedprintk("scorw_unlink_par_file: Removing parent's info saved in child %d's inodes.\n", i);
	scorw_remove_par_attr(c_inode);
	}
	mutex_unlock(&(p_scorw_inode->i_lock));

	//decrease ref count of parent scorw inode (and its childrens)
	scorw_put_inode(inode, 0, 0);
	 */


	return 0;
}

void scorw_exit(void)
{
	//Commentedprintk("scorw_exit called\n");

	//scorw_inode_list();
	//scorw_clean_inode_list();

	//stop threads
	//scorw_thread_exit();
	scorw_page_copy_thread_exit();
	scorw_free_all_page_copy();
	scorw_process_pending_frnd_version_cnt_inc_list(1);

	scorw_process_pending_log_sync_list();
	
	/*
	   if(scorw_sysfs_kobject)
	   {	
	////Commentedprintk("scorw_sysfs_kobject exists! Removing it and files inside it\n");
	sysfs_remove_file(scorw_sysfs_kobject, &async_copy_status_attr.attr);
	kobject_del(scorw_sysfs_kobject);
	}
	else
	{
	////Commentedprintk("scorw_sysfs_kobject doesn't exist!\n");
	}
	 */
	if(kobj_scorw)
	{
		//Commentedprintk("scorw_exit: kobj_scorw exists! Removing it and files inside it\n");
		sysfs_remove_file(kobj_scorw, &frnd_file_enable_recovery_attr.attr);
		sysfs_remove_file(kobj_scorw, &frnd_file_last_recovery_time_us_attr.attr);
		kobject_put(kobj_scorw);
	}
	else
	{
		//Commentedprintk("scorw_exit: kobj_scorw doesn't exist!\n");
	}

	///////////////////////////////////////////////////
	//exported symbols (functions pointers) resetting
	///////////////////////////////////////////////////
	//get_child_inode_num = 0;
	/*
	   get_child_inode_num = 0;
	   is_par_inode = 0;
	   get_child_i_attr_val = 0;
	   get_friend_attr_val = 0;
	   is_block_copied = 0;
	   __scorw_exit = 0;
	 */
	is_child_file = 0;
	is_par_file = 0;
	unlink_child_file = 0;
	unlink_par_file = 0;

	////Commentedprintk("************************ Good Bye from scorwExt4 *******************************\n");
}

void scorw_free_all_page_copy(void)
{
	struct page_copy *page_copy = 0;

	////Commentedprintk("Inside scorw_free_all_page_copy\n");
	while(!list_empty(&page_copy_llist))
	{
		//Note: FS is unmounting and page copy thread has exited. So, no insertion/deletion will happen from this list anymore.
		page_copy = list_first_entry_or_null(&(page_copy_llist), struct page_copy, ll_node);
		if(!page_copy)
		{
			continue;
		}
		////Commentedprintk("cleaning page copy corresponding block: %u, parent: %lu\n", page_copy->block_num, page_copy->par->i_ino_num);
		scorw_unprepare_page_copy(page_copy);
		scorw_free_page_copy(page_copy);
	}
}

void scorw_unprepare_page_copy(struct page_copy *page_copy)
{
	if(page_copy->data_page != 0)
	{
		__free_pages(page_copy->data_page, 0);
	}

	write_lock(&(page_copy_lock));
	scorw_remove_page_copy_llist(page_copy);
	scorw_remove_page_copy_hlist(page_copy);
	write_unlock(&(page_copy_lock));	

	//Parent file can be closed before all the page structs have been processed.
	//Thus, to trigger the freeing of vfs inodes of par, child, frnd inodes, it is essential that
	//ref count of scorw inode is continously checked to evaluate whether the ref count has become 0.
	//On refcount of scorw inode becoming zero, iput() the vfs inodes.
	//Hence, scorw_dec_process_usage_count is insufficient here and we have to fallback to scorw_put_inode()
	//
	//scorw_dec_process_usage_count(page_copy->par);
	scorw_put_inode(page_copy->par->i_vfs_inode, 0, 1, 0);
	//Commentedprintk("[DEBUG_SYNC] :: Back from scorw_put_inode ma'am\n");
}

void scorw_remove_page_copy_hlist(struct page_copy *page_copy)
{
	hash_del(&(page_copy->h_node));
}

//free memory occupied by page copy 
void scorw_free_page_copy(struct page_copy *page_copy)
{
	////Commentedprintk("Inside scorw_free_page_copy\n");
	//return kfree(page_copy);
	return kmem_cache_free(page_copy_slab_cache, page_copy);
}

void scorw_process_pending_frnd_version_cnt_inc_list(int sync_child)
{
	struct pending_frnd_version_cnt_inc *request = NULL;
	struct pending_frnd_version_cnt_inc *tmp = NULL;
	unsigned long child_version_val = 0;
	unsigned long frnd_version_val = 0;
	struct inode *child = 0;
	struct inode *frnd = 0;
	static unsigned long iter_id = 1;

	////Commentedprintk("%s(): Will process requests under id: %lu\n", __func__, iter_id);

	mutex_lock(&frnd_version_cnt_lock);
	list_for_each_entry_safe(request, tmp, &pending_frnd_version_cnt_inc_list, list)
	{
		////Commentedprintk("%s(): Chosen request corresponding child: %lu, frnd: %lu. sync_child: %d\n", __func__, request->child->i_ino, request->frnd->i_ino, sync_child);
		//iterated through all requests. Some entries have been requeued.
		//Skipping processing them.
		if(request->iter_id == iter_id)
		{
			break;
		}
		child = request->child;
		frnd = request->frnd;
		request->iter_id = iter_id;

		//we want atomicity w.r.t. open()/close() of child file
		mutex_lock(&(child->i_vfs_inode_lock));

		if(sync_child)
		{
			////Commentedprintk("%s(): will explicitly sync child before updating frnd inode\n", __func__);

			//Expecting that all files are closed before unmounting
			//Recall, otherwise, umount throws 'target busy' error
			BUG_ON(child->i_scorw_inode);

			//remove entry from list
			scorw_remove_pending_frnd_version_cnt_inc_list(request);	
			kfree(request);

			//sync child if dirty
			if(scorw_is_inode_dirty(child))
			{
				write_inode_now(child, 1);
			}

			//update frnd version count
			child_version_val = scorw_get_child_version_attr_val(child);
			BUG_ON(child_version_val == 0);
			frnd_version_val = child_version_val;
			scorw_set_frnd_version_attr_val(frnd, frnd_version_val);

			//mark frnd as dirty and sync it
			mark_inode_dirty(frnd);
			write_inode_now(frnd, 1);

			//reset flags related to version count management
			atomic_set(&child->i_cannot_update_child_version_cnt, 0);	//allow updation of child version count on next write to child 
			frnd->i_can_sync_frnd = -1;	//disallow syncing of frnd file
			mutex_unlock(&(child->i_vfs_inode_lock));
			iput(child);
			iput(frnd);
		}
		else
		{
			////Commentedprintk("%s(): will NOT explicitly sync child before updating frnd inode\n", __func__);

			//child file is open
			if(child->i_scorw_inode)
			{
				////Commentedprintk("%s(): child file is open. removing request from list\n", __func__);
				scorw_remove_pending_frnd_version_cnt_inc_list(request);	
				kfree(request);
				mutex_unlock(&(child->i_vfs_inode_lock));
				iput(child);
				iput(frnd);
			}
			//
			//child is dirty or child version count is not yet synced to disk
			else if(scorw_is_inode_dirty(child) || (frnd->i_can_sync_frnd != 1))
			{
				//requeue request at the tail of the list
				scorw_remove_pending_frnd_version_cnt_inc_list(request);	
				scorw_add_pending_frnd_version_cnt_inc_list(request);	
				mutex_unlock(&(child->i_vfs_inode_lock));
			}
			//All set. Frnd version count can be updated.
			else
			{
				////Commentedprintk("%s(): child file is not open and child is not dirty. Updating and syncing frnd version count\n", __func__);
				//remove entry from list
				scorw_remove_pending_frnd_version_cnt_inc_list(request);	
				kfree(request);

				//update frnd version count
				child_version_val = scorw_get_child_version_attr_val(child);
				BUG_ON(child_version_val == 0);
				frnd_version_val = child_version_val;
				scorw_set_frnd_version_attr_val(frnd, frnd_version_val);

				//mark frnd as dirty and sync it
				mark_inode_dirty(frnd);
				write_inode_now(frnd, 1);

				//reset flags related to version count management
				atomic_set(&child->i_cannot_update_child_version_cnt, 0);	//allow updation of child version count on next write to child 
				frnd->i_can_sync_frnd = -1;	//disallow syncing of frnd file
				mutex_unlock(&(child->i_vfs_inode_lock));
				iput(child);
				iput(frnd);
			}
		}
	}
	mutex_unlock(&frnd_version_cnt_lock);
	////Commentedprintk("%s(): Reached end of current iteration\n", __func__);
	++iter_id;
}

//MAHA_AARSH: Flush all pending log files to disk.
//Called from scorw_exit() at unmount time.
//During normal file close, log inodes are queued (not synced) — dirty pages
//stay in the page cache. At unmount we force-sync all dirty log pages + metadata.
//(Same deferred pattern as scorw_process_pending_frnd_version_cnt_inc_list)
void scorw_process_pending_log_sync_list(void)
{
	struct pending_log_sync *entry, *tmp;
	mutex_lock(&log_sync_list_lock);
	list_for_each_entry_safe(entry, tmp, &pending_log_sync_list, list) {
		if (entry->log_inode) {
			filemap_write_and_wait(entry->log_inode->i_mapping);
			mark_inode_dirty(entry->log_inode);
			write_inode_now(entry->log_inode, 1);
			iput(entry->log_inode);
		}
		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&log_sync_list_lock);
}


void scorw_remove_page_copy_llist(struct page_copy *page_copy)
{
	list_del(&(page_copy->ll_node));
}

int scorw_is_inode_dirty(struct inode* inode)
{
	int dirty_state = 0;
	spin_lock(&inode->i_lock);	
	dirty_state = !!(inode->i_state & I_DIRTY);
	spin_unlock(&inode->i_lock);	
	return dirty_state;
}

int scorw_page_copy_thread_init(void)
{
	////Commentedprintk("Inside scorw_page_copy_thread_init\n");
	page_copy_slab_cache = kmem_cache_create("page_copy", sizeof(struct page_copy), 0, SLAB_HWCACHE_ALIGN, NULL);
	if(page_copy_slab_cache == NULL)
	{
		//Commentedprintk("scorw_page_copy_thread_init: Failed to create page copy slab allocator cache\n");
		return -1;
	}
	if(page_copy_thread)
	{
		//Commentedprintk("scorw_page_copy_thread_init: Thread already exists\n");
		//return -1;
	}

	////Commentedprintk("scorw_page_copy_thread_init: Creating Thread\n");
	page_copy_thread = kthread_run(scorw_page_copy_thread_fn, NULL, "page_copy_thread");
	if(page_copy_thread)
	{
		////Commentedprintk("scorw_page_copy_thread_init: Thread created successfully\n");
	}
	else
	{
		////Commentedprintk("scorw_page_copy_thread_init: Thread creation failed\n");
		return -1;
	}
	return 0;
}


int scorw_page_copy_thread_exit(void)
{
	////Commentedprintk("Inside scorw_page_copy_thread_exit. Terminating Thread\n");
	if(page_copy_thread)
	{
		//Commentedprintk("scorw_page_copy_thread_exit: Thread about to be terminated\n");
		stop_page_copy_thread = 1;
		wake_up(&page_copy_thread_wq);
		while(stop_page_copy_thread != 2)
		{
			msleep(1000);
		}
		//Commentedprintk("scorw_page_copy_thread_exit: Thread terminated\n");
		page_copy_thread = 0;
	}
	else
	{
		//Commentedprintk("scorw_page_copy_thread_exit: No thread to terminate\n");
	}
	kmem_cache_destroy(page_copy_slab_cache);
	return 0;
}

int scorw_page_copy_thread_fn(void *arg)
{
	struct page_copy *cur_page_copy = 0;
	struct scorw_inode *p_scorw_inode = 0;
	struct scorw_inode *c_scorw_inode = 0;
	struct uncopied_block *uncopied_block = 0;
	unsigned long last_process_time_jiffies =  0;
	unsigned long timeout_jiffies =  0;
	int block_num = 0;
	int j = 0;
	int ret = 0;

	timeout_jiffies = 5*HZ;
	last_process_time_jiffies =  jiffies;
	while(1)
	{
		////Commentedprintk("Inside scorw_page_copy_thread_fn\n");

		//Todo: Ideally, before stopping this thread during unmount, make sure that all pending copy's are processed.
		//Because, when we do unmount, pending writes to parent (waiting for page copies to be applied to children)
		//will be committed to disk. So, parent's data will get overwritten. So, unless this data has been copied to children,
		//we will have correctness issue.
		//
		//update: Done. Check below.
		//update: Commented out the modifications to achieve above. So, that testing can complete faster.
		//
		////Commentedprintk("scorw_page_copy_thread_fn: calling wait_event, stop_page_copy_thread: %d, list_empty(&page_copy_llist): %d\n", stop_page_copy_thread, list_empty(&page_copy_llist));
		//
		//Note: 
		//	* 1 Hz is equal to number of jiffies in one sec.
		//	* jiffies + x*Hz  means timeout of x sec from now
		wait_event_timeout(page_copy_thread_wq, (!list_empty(&page_copy_llist)) || (stop_page_copy_thread==1), timeout_jiffies);
		////Commentedprintk("scorw_page_copy_thread_fn: Woke up from waiting, stop_page_copy_thread: %d, list_empty(&page_copy_llist): %d\n", stop_page_copy_thread, list_empty(&page_copy_llist));

		//waited long enough for processing requests waiting for 
		//updation of frnd version count
		if(time_after_eq(jiffies, last_process_time_jiffies + timeout_jiffies))
		{
			scorw_process_pending_frnd_version_cnt_inc_list(0);
			last_process_time_jiffies = jiffies;
			continue;
		}

		//If stop_page_copy_thread is set this implies unmount is in progress. So, no more writes will happen.
		//Unmount only when all page copies have been done
		//
		//if((stop_page_copy_thread == 1) && (list_empty(&page_copy_llist)))
		if((stop_page_copy_thread == 1))
		{
			//cleanup of page copy
			////Commentedprintk("scorw_page_copy_thread_fn: Told to exit. Doing cleanup before unmount.\n");
			scorw_free_all_page_copy();
			//Commentedprintk("scorw_page_copy_thread_fn: Stopping page copy thread\n");
			break;
		}
		////Commentedprintk("scorw_page_copy_thread_fn: Selecting page copy struct to process\n");

		//select page copy to process
		cur_page_copy = scorw_get_page_copy_to_process();
		//BUG_ON(cur_page_copy == NULL);
		if(cur_page_copy == NULL)
		{
			continue;
		}
		page_copy_thread_running = 1;

		//process selected page copy
		////Commentedprintk("scorw_page_copy_thread_fn: copying block %u (parent inode: %lu), to all children\n", cur_page_copy->block_num, cur_page_copy->par->i_ino_num);

		//For each child, check if page is copied. If not, then copy it.
		p_scorw_inode = cur_page_copy->par;
		block_num = cur_page_copy->block_num;
		////Commentedprintk("scorw_page_copy_thread_fn: trying down_read par i_lock. copying block %u (parent inode: %lu), to all children\n", cur_page_copy->block_num, cur_page_copy->par->i_ino_num);
		down_read(&(p_scorw_inode->i_lock));
		////Commentedprintk("scorw_page_copy_thread_fn: down_read par i_lock succeeded. copying block %u (parent inode: %lu), to all children\n", cur_page_copy->block_num, cur_page_copy->par->i_ino_num);
		for(j = 0; j < SCORW_MAX_CHILDS; j++)
		{
			//pick a child
			c_scorw_inode = p_scorw_inode->i_child_scorw_inode[j];
			if((c_scorw_inode == NULL) || (cur_page_copy->is_target_child[j] == 0))
			{
				////Commentedprintk("scorw_page_copy_thread_fn: Child: %d doesn't exist (or) child exists but isn't target of this page copy struct\n", j);
				continue;
			}

			//If number of blocks in child file are less than the block being processed, donot copy the content of this block to child
			if(((c_scorw_inode->i_copy_size - 1) >> PAGE_SHIFT) < block_num)
			{
				////Commentedprintk("scorw_page_copy_thread_fn: Out of range block for child: %d, child inode: %lu. Skipping it!", j, c_scorw_inode->i_ino_num);
				continue;
			}

			//copy block to a child
			//Note: Copying from page copy to child has to happen in COPYING_EXCL mode.
			//Consider the case when read from child acquires READING lock and finds 
			//that page copy struct corresponding a block exists.
			//Now, assume that before it can read from page copy struct, it gets scheduled out. 
			//Now, page copy struct gets processed and deleted in this page copy thread.
			//When read from page copy struct resumes, it will fault because page copy struct doesn't exist any more.
			//Thus, page copying here should not be done with COPYING mode (which is compatible 
			//with READING mode but with COPYING_EXCL mode)	
			uncopied_block = scorw_get_uncopied_block(c_scorw_inode, block_num, COPYING_EXCL);
			if(!scorw_is_block_copied(c_scorw_inode->i_frnd_vfs_inode, block_num))
			{
				////Commentedprintk("scorw_page_copy_thread_fn: copying block %u (parent inode: %lu) to child: %d with inode num: %lu\n", cur_page_copy->block_num, cur_page_copy->par->i_ino_num, j, c_scorw_inode->i_ino_num);
				//increment child version count on first write to a blk
				//that is not copied to child yet because only then
				//friend file is also modified
				scorw_child_version_cnt_inc(c_scorw_inode->i_vfs_inode);
				ret = scorw_copy_page_from_page_copy(cur_page_copy, c_scorw_inode);
				if(ret == 0)
				{
					scorw_set_block_copied(c_scorw_inode, block_num);
					scorw_dec_yet_to_copy_blocks_count(c_scorw_inode, 1);
					//signal to sync path regarding completion of this copy
					//so that child blk can be flushed to disk
					//Commentedprintk("waking up buddy\n");
					wake_up(&sync_child_wait_queue);
				}

			} else {
				//Commentedprintk("waking up buddy\n");
				wake_up(&sync_child_wait_queue);
			}
			scorw_put_uncopied_block(c_scorw_inode, block_num, COPYING_EXCL, uncopied_block);
			scorw_remove_uncopied_block(c_scorw_inode, block_num, uncopied_block);

			//sync the dirty blocks of the child file
			//scorw_inode_write_and_wait_range(c_scorw_inode->i_vfs_inode, 0, c_scorw_inode->i_copy_size);
		}	
		////Commentedprintk("scorw_page_copy_thread_fn: block: %u is copied to all children\n",  block_num);
		up_read(&(p_scorw_inode->i_lock));

		//free processed page copy
		scorw_unprepare_page_copy(cur_page_copy);
		scorw_free_page_copy(cur_page_copy);
		page_copy_thread_running = 0;
	}	
	stop_page_copy_thread = 2;	//To signal that thread is ready to exit

	return 0;
}

struct page_copy *scorw_get_page_copy_to_process(void)
{
	////Commentedprintk("Inside scorw_get_page_copy_to_process\n");
	if(list_empty(&page_copy_llist))
	{
		////Commentedprintk("scorw_get_page_copy_to_process: list is empty. returning.\n");
		return NULL;
	}
	return list_first_entry(&(page_copy_llist), struct page_copy, ll_node);
}

int scorw_write_par_blocks(struct inode* inode, loff_t offset, size_t len, struct page* par_data_page)
{
	unsigned blk_num = 0;
	struct scorw_inode *p_scorw_inode = NULL;
	struct scorw_inode *c_scorw_inode = NULL;
	unsigned last_block_eligible_for_copy = 0;
	struct page_copy *page_copy = 0;
	struct page_copy *existing_page_copy = 0;
	int j = 0;
	int in_range = 0;
	int is_4KB_write = 0;
	int target_children_exists = 0;
	unsigned char is_target_child[SCORW_MAX_CHILDS];

	////Commentedprintk("Inside scorw_write_par_blocks \n");
	if(len <= 0)
	{
		//Commentedprintk("scorw_write_par_blocks: length is 0. Thus, skipping write.\n");
		return 0;
	}

	blk_num = (offset >> PAGE_SHIFT);
	last_block_eligible_for_copy  = ((inode->i_size-1) >> PAGE_SHIFT);
	////Commentedprintk("[pid: %u] Inside scorw_write_par_blocks: processing block: %u, parent inode: %lu.\n", current->pid, blk_num, inode->i_ino);

	//This write is purely append operation. Nothing to be done by us.
	if(blk_num > last_block_eligible_for_copy)
	{
		////Commentedprintk("scorw_write_par_blocks: This portion of write is purely append operation. Nothing to be done by us.\n");
		return 0;
	}

	//find scorw inode corresponding the parent vfs inode
	p_scorw_inode = scorw_find_inode(inode);
	BUG_ON(!p_scorw_inode || p_scorw_inode->i_vfs_inode != inode);

	//Find target children (to which par contents needs to be copied)
	target_children_exists = 0;
	////Commentedprintk("scorw_write_par_blocks: Finding target children to which par contents needs to be copied\n");
	for(j = 0; j <= p_scorw_inode->i_last_child_index; j++)
	{
		is_target_child[j] = 0;
		c_scorw_inode = p_scorw_inode->i_child_scorw_inode[j];
		if(c_scorw_inode == NULL)
		{
			continue;
		}
		////Commentedprintk("**** Child %d, inode num: %lu ****\n", j, c_scorw_inode->i_ino_num);
		//This blk should not be copied to child if child's range doesn't
		//include this blk i.e. child doesn't want static snapshot of
		//par's contents
		in_range = scorw_is_in_range(c_scorw_inode, blk_num);   //Here non-zero in_range means it is a CoW
		if(!in_range)
		{
			////Commentedprintk("scorw_write_par_blocks: block: %u, not in range of child: %d with inode num: %lu\n", blk_num, j, c_scorw_inode->i_ino_num);
			continue;
		}
		//existing page copy with child as target exists?
		//If existing page copy exists, it implies, static snapshot of this block
		//has already been created for this child.
		existing_page_copy = scorw_find_page_copy(blk_num, inode->i_ino, c_scorw_inode->i_at_index);
		if(existing_page_copy)
		{
			////Commentedprintk("scorw_write_par_blocks: block: %u, existing page copy exists corressponding child: %d with inode num: %lu\n", blk_num, j, c_scorw_inode->i_ino_num);
			continue;
		}
		//if blk is already copied, nothing to do for this child
		if(scorw_is_block_copied(c_scorw_inode->i_frnd_vfs_inode, blk_num))
		{
			////Commentedprintk("scorw_write_par_blocks: block: %u, blk is already copied corressponding child: %d with inode num: %lu\n", blk_num, j, c_scorw_inode->i_ino_num);
			continue;
		}
		is_target_child[j] = 1;
		target_children_exists = 1;
		////Commentedprintk("scorw_write_par_blocks: block: %u, child: %d with inode num: %lu is a target child\n", blk_num, j, c_scorw_inode->i_ino_num);

	}	

	if(!target_children_exists)
	{
		////Commentedprintk("scorw_write_par_blocks: block: %u, no target children exists\n", blk_num);
		return 0;
	}

	////Commentedprintk("scorw_write_par_blocks: write on block: %u, offset: %lu, len: %lu\n", blk_num, offset, len);
	//check whether contents of entire block will be overwritten or not (4KB write or not)
	if((offset <= ((unsigned long)blk_num << PAGE_SHIFT)) && ((offset + len) >= (((unsigned long)blk_num << PAGE_SHIFT) + PAGE_SIZE)))
	{
		is_4KB_write = 1;
		////Commentedprintk("scorw_write_par_blocks: write on block: %u is a 4KB write\n", blk_num);
	}

	//create page copy
	page_copy = scorw_alloc_page_copy();
	BUG_ON(page_copy == NULL);

	scorw_prepare_page_copy(blk_num, p_scorw_inode, page_copy, is_4KB_write, par_data_page, is_target_child);
	////Commentedprintk("Printing page copy list\n");
	//scorw_print_page_copy_hlist();

	////Commentedprintk("scorw_write_par_blocks: Waking up page copy thread\n");
	/* Let page copy thread wakeup be triggered due to timeout
	   if(!page_copy_thread_running)
	   {
	   wake_up(&page_copy_thread_wq); 
	   }
	 */

	return 0;
}

static int scorw_is_in_range(struct scorw_inode *scorw_inode, unsigned blk_num)
{
	int i = 0;
#ifdef USE_OLD_RANGE 
	int in_range = 0;
#else
	int in_range = 1;
#endif
	for(i=0; i < scorw_inode->i_num_ranges; i++)
	{
		if((scorw_inode->i_range[i].start <= blk_num) && (blk_num <= scorw_inode->i_range[i].end))
		{
#ifdef USE_OLD_RANGE 
			in_range = 1;
#else
			if(scorw_inode->i_range[i].snapx_behavior > SNAPX_FLAG_COW_RO)
				in_range = 0;	     
#endif 
			break;
		}
	}
	return in_range;
}

struct page_copy *scorw_alloc_page_copy(void)
{
	////Commentedprintk("Inside scorw_alloc_page_copy\n");
	//return kzalloc(sizeof(struct page_copy), GFP_KERNEL);
	return kmem_cache_alloc(page_copy_slab_cache, GFP_KERNEL);
}

int scorw_prepare_page_copy(unsigned block_num, struct scorw_inode *par_scorw_inode, struct page_copy *page_copy, int is_4KB_write, struct page* par_data_page, unsigned char *is_target_child)
{
	int i = 0;

	////Commentedprintk("Inside scorw_prepare_page_copy\n");
	page_copy->block_num = block_num;
	page_copy->par = par_scorw_inode;
	page_copy->data_page_loaded = 0;
	page_copy->data_page = 0;

	scorw_copy_page_to_page_copy(page_copy, is_4KB_write, par_data_page);

	//parent scorw inode should not get freed until page copy struct exists
	//So, increase reference count of parent scorw inode
	//scorw_inc_process_usage_count(par_scorw_inode);
	++par_scorw_inode->added_to_page_copy;
	////Commentedprintk("%s(): added page copy structure. Total page copy structures added till now: %lu\n", __func__, par_scorw_inode->added_to_page_copy);
	for(i=0; i<SCORW_MAX_CHILDS; i++)
	{
		if(i <= par_scorw_inode->i_last_child_index)
		{
			page_copy->is_target_child[i] = is_target_child[i];
		}
		else
		{
			page_copy->is_target_child[i] = 0;
		}
		////Commentedprintk("scorw_prepare_page_copy: page_copy->is_target_child[%d]: %u\n", i, page_copy->is_target_child[i]);
	}

	write_lock(&(page_copy_lock));	
	scorw_add_page_copy_llist(page_copy);
	scorw_add_page_copy_hlist(page_copy);
	write_unlock(&(page_copy_lock));	

	return 0;
}

int scorw_copy_page_to_page_copy(struct page_copy *page_copy, int is_4KB_write, struct page* par_data_page)
{
	void *kaddr_r = 0;
	void *kaddr_w = 0;

	////Commentedprintk("scorw_copy_page_to_page_copy: Inside this function\n");

	//page up to date means hot cache scenario
	//page not up to date and is_4KB_write = 0 means cold cache with <4KB write 
	if(PageUptodate(par_data_page) || (!is_4KB_write))
	{
		////Commentedprintk("scorw_copy_page_to_page_copy: page corresponding block: %u exists in page cache either due to hot cache or <4KB cold cache scenario\n", page_copy->block_num);
		page_copy->data_page = alloc_pages(GFP_KERNEL, 0);
		BUG_ON(page_copy->data_page == NULL);

		kaddr_r = kmap_atomic(par_data_page);		//par_data_page's get_page is done in ext4_da_write_begin 
								//and its put_page will be done in ext4_da_write_end
		kaddr_w = kmap_atomic(page_copy->data_page);

		copy_page(kaddr_w, kaddr_r);

		kunmap_atomic(kaddr_r);
		kunmap_atomic(kaddr_w);

		page_copy->data_page_loaded = 1;
	}

	return 0;
}

//add a page copy into linked list of page copy's
void scorw_add_page_copy_llist(struct page_copy *page_copy)
{
	INIT_LIST_HEAD(&(page_copy->ll_node));
	list_add_tail(&(page_copy->ll_node), &page_copy_llist);
}

void scorw_add_page_copy_hlist(struct page_copy *page_copy)
{
	hash_add(page_copy_hlist, &(page_copy->h_node), (page_copy->block_num));
}


//Why read barrier logic is required?
//====================================
//Putting a barrier here so that read requests do not read modified data of parent. Instead they finish before this barrier.
//Consider a scenario when read request comes before the page copy is created (write to parent is happening in parallel).
//It will check that page copy doesn't exist and suppose it get scheduled out. If we don't add this barrier, then the 
//read operation will get scheduled. Assume that write to parent has completed by this time. Thus, it will read the modified 
//data from the parent.
//This barrier makes sure that read operation completes before this barrier itself. READING and COPYING_EXCL conflict with each other.
//So, COPYING_EXCL lock can be obtained only when READING lock is not there.
//
//All reads after the barrier will be read from child or page copy.
//
//=================================================================
//Why read barrier logic is split into two halves (begin and end)?
//=================================================================
//Done to fix following ordering deadlock in our older code when
//read barrier was called from within scorw_write_par, instead of the 
//approach used now:
//Assume one writer and one reader are writing/reading same file.
//If write parent comes first, it acquires lock on page 'p' being written (aops->write_begin), then it acquires
//Active map lock on this page 'p'
//On the other hand, reader acquires Active map lock on page 'p' first (scorw_follow_on_read())and then acquires
//page lock (generic_file_buffered_read()). Thus, this can lead to deadlock.
//
//So, we modified barrier logic
void scorw_read_barrier_begin(struct scorw_inode *p_scorw_inode, unsigned block_num, struct uncopied_block **uncopied_block)
{
	int j = 0;
	struct scorw_inode *c_scorw_inode = 0;

	//Note: we acquire following lock in barrier_begin()
	//	and release it in barrier_end(). This won't cause
	//	issue in case of multiple writers to parent
	//	because vfs inode serializes writers.
	//	Also, this won't cause stalling of open()
	//	of child files because this function is acquired
	//	and then released per page basis (see 
	//	scorw_generic_perform_write())
	down_read(&(p_scorw_inode->i_lock));
	for(j = 0; j <= p_scorw_inode->i_last_child_index; j++)
	{
		c_scorw_inode = p_scorw_inode->i_child_scorw_inode[j];
		if(c_scorw_inode == NULL)
		{
			continue;
		}

		//barrier should be set for a child file only if it is open
		//When a parent file is opened, reference count of child is also increased
		//If write on parent is happening (parent file is open) and ref count of child is 1, this implies
		//that child file is not open
		////Commentedprintk("child %d, process ref count: %lu\n", j, c_scorw_inode->i_process_usage_count);
		uncopied_block[j] = 0;
		if(scorw_get_process_usage_count(c_scorw_inode) > 1)
		{
			uncopied_block[j] = scorw_get_uncopied_block(c_scorw_inode, block_num, COPYING_EXCL);
		}
	}	
}

//see comment above scorw_read_barrier_begin()
void scorw_read_barrier_end(struct scorw_inode *p_scorw_inode, unsigned block_num, struct uncopied_block **uncopied_block)
{
	int j = 0;
	struct scorw_inode *c_scorw_inode = 0;

	for(j = 0; j <= p_scorw_inode->i_last_child_index; j++)
	{
		c_scorw_inode = p_scorw_inode->i_child_scorw_inode[j];
		if(c_scorw_inode == NULL)
		{
			continue;
		}

		//see comment in scorw_read_barrier_begin()
		if(uncopied_block[j])
		{
			scorw_put_uncopied_block(c_scorw_inode, block_num, COPYING_EXCL, uncopied_block[j]);
			scorw_remove_uncopied_block(c_scorw_inode, block_num, uncopied_block[j]);
		}

	}	
	//This lock is acquired in scorw_read_barrier_begin()
	up_read(&(p_scorw_inode->i_lock));
}


//MAHA_AARSH_start //
// File will always be a parent file;
loff_t scorw_write_see_thru_ro(struct file *file, struct iov_iter *i, loff_t pos, int *out_status)
{
	struct inode *inode = file->f_mapping->host;
	struct scorw_inode *p_inode = scorw_find_inode(inode);
	unsigned long target_logical_blk = pos / PAGE_SIZE;
	unsigned long appended_ext4_blk;
	size_t count = iov_iter_count(i);
	int status , error , start_blk = pos / PAGE_SIZE , end_blk = (pos + count-1)/PAGE_SIZE;
	loff_t append_pos , logical_size;
	unsigned int nr_blk;
	size_t append_count;
	unsigned long to_write_block;
	unsigned long copy_helper;
        unsigned long start_page_idx, end_page_idx;

	logical_size = scorw_get_original_parent_size(p_inode->i_vfs_inode);

	if (!p_inode) return pos;
	nr_blk = (end_blk - start_blk + 1);	
	append_count = nr_blk * PAGE_SIZE;

	/*Check if the lock has been acquired previously by current process*/	
	if(scorw_self_transaction_status(inode , file) == SET_TRANSACTION){
		status = SET_TRANSACTION;
	} else { /*if not, proceed after taking this lock*/
		error = scorw_set_transaction(inode , file , SET_NORMAL_WRITE);
		if(error){
			//Commentedprintk("[ERROR] :: %s, line %d\n" , __func__ , __LINE__);
			return -EIO;
		}
		//Commentedprintk("[%s] :: Acquired %d lock\n" , __func__ , status);
		status = SET_NORMAL_WRITE; /*TODO : move this one line up*/
	}

	// 1. Calculate and Copy
	append_pos = (i_size_read(inode) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	appended_ext4_blk = append_pos / PAGE_SIZE;
	

	to_write_block = scorw_lookup_physical_block(p_inode, target_logical_blk);
		
	if( (to_write_block != BLK_NOT_FOUND)  || (pos < logical_size) ){	
		// if (scorw_internal_copy_blocks(file, to_write_block * PAGE_SIZE, append_pos, append_count) < 0) { 
		// 	return pos;
		// }
		start_page_idx = pos/PAGE_SIZE;
                end_page_idx = (pos + count - 1)/PAGE_SIZE;
                copy_helper = end_page_idx - start_page_idx;
                if ( (pos % PAGE_SIZE) || (copy_helper == 0 && (pos + count) % PAGE_SIZE != 0) ) {
                        if (scorw_internal_copy_blocks(file, to_write_block * PAGE_SIZE, append_pos, PAGE_SIZE) < 0) {
                                return pos;
                        }
                }
                if ( ((pos + count) % PAGE_SIZE) && (copy_helper > 0) && (end_page_idx * PAGE_SIZE < logical_size) ) {
                        if (scorw_internal_copy_blocks(file, (end_page_idx) * PAGE_SIZE,
                                                                 append_pos + (copy_helper * PAGE_SIZE), PAGE_SIZE) < 0) {
                                return pos;
                        }
                }
	}

	*out_status = status;
	return append_pos + (pos % PAGE_SIZE);
}





/*
//MAHA_AARSH_start //
// File will always be a parent file;
loff_t scorw_write_see_thru_ro(struct file *file, struct iov_iter *i, loff_t pos)
{
        struct inode *inode = file->f_mapping->host;
        struct scorw_inode *p_inode = scorw_find_inode(inode);
        unsigned long target_logical_blk = pos / PAGE_SIZE;
        unsigned long appended_ext4_blk;
        size_t count = iov_iter_count(i);
        int status , error , start_blk = pos / PAGE_SIZE , end_blk = (pos + count-1)/PAGE_SIZE;
        loff_t append_pos , logical_size;
        unsigned int nr_blk;
        size_t append_count;
        unsigned long to_write_block;
        unsigned long copy_helper;
        unsigned long start_page_idx, end_page_idx;

        logical_size = scorw_get_original_parent_size(p_inode->i_vfs_inode);

        if (!p_inode) return pos;
        nr_blk = (end_blk - start_blk + 1);
        append_count = nr_blk * PAGE_SIZE;

        if(scorw_self_transaction_status(inode , file) == SET_TRANSACTION){
                status = SET_TRANSACTION;
        } else { 
                error = scorw_set_transaction(inode , file , SET_NORMAL_WRITE);
                if(error){
                        //Commentedprintk("[ERROR] :: %s, line %d\n" , __func__ , __LINE__);
                        return -EIO;
                }
                //Commentedprintk("[%s] :: Acquired %d lock\n" , __func__ , status);
                status = SET_NORMAL_WRITE; 
        }

        // 1. Calculate and Copy
        append_pos = (i_size_read(inode) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        appended_ext4_blk = append_pos / PAGE_SIZE;


        to_write_block = scorw_lookup_physical_block(p_inode, target_logical_blk);

        if( (to_write_block != BLK_NOT_FOUND)  || (pos < logical_size) ){

                start_page_idx = pos/PAGE_SIZE;
                end_page_idx = (pos + count - 1)/PAGE_SIZE;
                copy_helper = end_page_idx - start_page_idx;
                if ( (pos % PAGE_SIZE) || (copy_helper == 0 && (pos + count) % PAGE_SIZE != 0) ) {
                        if (scorw_internal_copy_blocks(file, to_write_block * PAGE_SIZE, append_pos, PAGE_SIZE) < 0) {
                                return pos;
                        }
                }
                if ( ((pos + count) % PAGE_SIZE) && (copy_helper > 0) && (end_page_idx * PAGE_SIZE < logical_size) ) {
                        if (scorw_internal_copy_blocks(file, (end_page_idx) * PAGE_SIZE,
                                                                 append_pos + (copy_helper * PAGE_SIZE), PAGE_SIZE) < 0) {
                                return pos;
                        }
                }

        }

        // 2. Increment and PERSIST
        if(status == SET_NORMAL_WRITE){
                __sync_fetch_and_add(&(p_inode->version), 1);
                //scorw_set_curr_version_attr_val(inode, p_inode->version); // UPDATE DISK XATTR
                ////Commentedprintk("[DEBUG] :: {%s} updated version from %d -> %d" , __func__ , (p_inode->version) - 1 , p_inode->version);
        }

        // 3. Log it
        scorw_record_write(p_inode, target_logical_blk, appended_ext4_blk, nr_blk , status);

//      //Commentedprintk("SCORW_DEBUG: WRITE PARENT. Inode: %lu, New Version: %d, Blk: %lu -> Phys: %lu\n", 
                //      inode->i_ino, p_inode->version, target_logical_blk, appended_ext4_blk);

        if(status == SET_NORMAL_WRITE){
                error = scorw_set_transaction(inode , file , UNSET_TRANSACTION);
                if(error){
                        //Commentedprintk("[ERROR] :: %s, line %d\n" , __func__ , __LINE__);
                        return -EIO;
                }
//              //Commentedprintk("[%s] :: Released %d lock\n" , __func__ , status);
        }
        return append_pos + (pos % PAGE_SIZE);
}
*/



void scorw_write_see_thru_ro_end(struct file *file, unsigned long target_logical_blk, unsigned long appended_ext4_blk, unsigned int nr_blk, int status) {
	struct inode *inode = file_inode(file);
	struct scorw_inode *p_inode = scorw_find_inode(inode);
	__u32 crc;

	// 1. Calculate CRC from the NOW UPDATED page cache
	crc = calc_page_cache_crc(inode->i_mapping, appended_ext4_blk, nr_blk);

	// Scenario C: Poison the CRC so recovery detects a mismatch
	if (unlikely(scorw_crash_with_bad_crc))
		crc = 0xDEADBEEF;

	// 2. Increment version and Log
	if(status == SET_NORMAL_WRITE){
		__sync_fetch_and_add(&(p_inode->version), 1);
	}
	scorw_record_write(p_inode, target_logical_blk, appended_ext4_blk, nr_blk, status, crc);

	/*
	 * ---- CRASH INJECTION POINT A ----
	 * Scenario: Log record is flushed to disk, but the CoW'd parent
	 * data block is still only in the page cache (never hit disk).
	 *
	 * Recovery expectation: scorw_truncate_log_to_version() will read
	 * the physical block at appended_ext4_blk, compute its CRC, find a
	 * mismatch (the block may be zeroed or stale), and truncate the log
	 * back to the previous valid version. The parent data should appear
	 * as the OLD value on the next mount.
	 */
	if (unlikely(scorw_crash_after_log_write)) {
		printk(KERN_CRIT "SCORW TEST [Scenario A]: Flushing log inode %lu to disk, "
			"then panicking. Parent data inode %lu NOT flushed.\n",
			p_inode->i_log_vfs_inode->i_ino, inode->i_ino);
		/* Force only the log to disk */
		filemap_write_and_wait(p_inode->i_log_vfs_inode->i_mapping);
		sync_inode_metadata(p_inode->i_log_vfs_inode, 1);
		/* Parent page cache is intentionally NOT flushed here */
		panic("SCORW TEST Scenario A: crash after log write, before data flush");
	}

	/*
	 * ---- CRASH INJECTION POINT B ----
	 * Scenario: The CoW'd parent data block is flushed to disk, but the
	 * log record is still only in the page cache (never hit disk).
	 *
	 * Recovery expectation: scorw_truncate_log_to_version() finds no
	 * log record for this version (the log was not persisted). The
	 * appended block at EOF is simply unreferenced dark matter. The GC
	 * (scorw_gc_blocks) will eventually punch a hole for it. No
	 * corruption — the parent will read as the OLD value.
	 */
	if (unlikely(scorw_crash_after_data_write)) {
		printk(KERN_CRIT "SCORW TEST [Scenario B]: Flushing parent data inode %lu to disk, "
			"then panicking. Log inode %lu NOT flushed.\n",
			inode->i_ino, p_inode->i_log_vfs_inode->i_ino);
		/* Force only the parent data to disk */
		filemap_write_and_wait(inode->i_mapping);
		/* Log page cache is intentionally NOT flushed here */
		panic("SCORW TEST Scenario B: crash after data write, before log flush");
	}

	/*
	 * ---- CRASH INJECTION POINT C ----
	 * Scenario: Both log and data are correctly flushed to disk, but
	 * the CRC stored in the log is intentionally wrong (0xDEADBEEF).
	 * The xattr safe_version is NOT updated (we panic before close).
	 *
	 * Recovery expectation: scorw_truncate_log_to_version() sees a
	 * version newer than safe_version, reads the physical data block,
	 * computes its real CRC, compares it against 0xDEADBEEF, and finds
	 * a mismatch. The log is truncated back to the previous version,
	 * proving that CRC validation actually catches corruption.
	 */
	if (unlikely(scorw_crash_with_bad_crc)) {
		printk(KERN_CRIT "SCORW TEST [Scenario C]: Bad CRC 0xDEADBEEF written. "
			"Flushing BOTH log inode %lu and parent data inode %lu to disk, "
			"then panicking. xattr safe_version NOT updated.\n",
			p_inode->i_log_vfs_inode->i_ino, inode->i_ino);
		/* Flush both to disk so everything is persisted */
		filemap_write_and_wait(p_inode->i_log_vfs_inode->i_mapping);
		sync_inode_metadata(p_inode->i_log_vfs_inode, 1);
		filemap_write_and_wait(inode->i_mapping);
		/* Do NOT update xattr — safe_version stays old */
		panic("SCORW TEST Scenario C: bad CRC persisted, both files flushed");
	}

	// 3. Close transaction
	if(status == SET_NORMAL_WRITE){
		scorw_set_transaction(inode, file, UNSET_TRANSACTION);
	}
}
//1. This takes in struct inode* as opposed to struct scorw_inode* while writing to frnd file
//2. Kindly ensure that the write takes place in one block , else problems
// 1. Change the parameters to accept our binary struct and its size
void write_offset_log(struct inode *l_inode, loff_t offset, int len, void* ptr) 
{
	struct page* page = NULL;
	char* kaddr = NULL;
	int ret = 0;
	loff_t new_size;

	if(!ptr) {
		//Commentedprintk(KERN_ERR "Passed Null pointer in ptr in %s\n", __func__);
		return;
	}

	page = scorw_get_page(l_inode, (offset/PAGE_BYTES));
	if(page == NULL) {
		//Commentedprintk(KERN_ERR "Failed to get page\n");
		return;
	}

	if(!page_has_buffers(page)) {
		lock_page(page);
		wait_for_stable_page(page);
		ret = __block_write_begin(page_folio(page), page->index << PAGE_SHIFT, PAGE_SIZE, ext4_da_get_block_prep);
		if(ret < 0) {
			unlock_page(page);
			scorw_put_page(page);
			BUG_ON(ret<0);
		}
		unlock_page(page);
	}

	kaddr = kmap_atomic(page);

	// HAMARA FIX: We just copy the raw binary bytes of the struct directly!
	// No sprintf needed. This is lightning fast.
	memcpy(kaddr + (offset % PAGE_SIZE), ptr, len); 

	kunmap_atomic(kaddr);

	if(!PageDirty(page)) {
		lock_page(page);
		scorw_set_page_dirty(page);
		unlock_page(page);
	}
	scorw_put_page(page);

	// Update EOF (Your existing logic)
	new_size = offset + len;
	if (new_size > i_size_read(l_inode)) {

		i_size_write(l_inode, new_size);
		EXT4_I(l_inode)->i_disksize = new_size;
		mark_inode_dirty(l_inode);
	}
}

void scorw_cleanup_versions(struct scorw_inode *s_inode)
{
	int i;
	// Destroy all XArrays and free the structs
	for (i = 0; i < MAX_VERSIONS; i++) {
		if (s_inode->version_states[i] != NULL) {
			xa_destroy(&s_inode->version_states[i]->delta_map);
			kfree(s_inode->version_states[i]);
			s_inode->version_states[i] = NULL;
		}
	}

	s_inode->loaded_version_count = 0;
	s_inode->fifo_head = 0;

	// Release the Log File inode so Ext4 can clean it up
	if (s_inode->i_log_vfs_inode != NULL) {
		//iput(s_inode->i_log_vfs_inode);
		
		// MAHA_AARSH
		struct pending_log_sync *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (entry) {
			entry->log_inode = s_inode->i_log_vfs_inode;
			INIT_LIST_HEAD(&entry->list);
			mutex_lock(&log_sync_list_lock);
			list_add_tail(&entry->list, &pending_log_sync_list);
			mutex_unlock(&log_sync_list_lock);
		} else {
			// Fallback: if allocation fails, we must release to avoid leak
			iput(s_inode->i_log_vfs_inode);
		}
		s_inode->i_log_vfs_inode = NULL;
	}
}


struct scorw_version* scorw_get_or_create_version(struct scorw_inode *s_inode, int v_num) 
{
	int idx = v_num - 1;
	struct scorw_version *new_v;
	int evict_idx, evict_v_num;
	struct scorw_version *evict_v;

	if (idx < 0 || idx >= MAX_VERSIONS) return NULL;
	if (s_inode->version_states[idx] != NULL) return s_inode->version_states[idx]; // Already exists

	// Run FIFO eviction if we have 4 loaded versions
	if (s_inode->loaded_version_count == MAX_RAM_VERSIONS) {
		evict_idx = s_inode->fifo_head;
		evict_v_num = s_inode->loaded_versions[evict_idx];
		evict_v = s_inode->version_states[evict_v_num - 1];

		if (evict_v) {
			xa_destroy(&evict_v->delta_map);
			kfree(evict_v);
			s_inode->version_states[evict_v_num - 1] = NULL;
		}

		s_inode->loaded_versions[evict_idx] = v_num;
		s_inode->fifo_head = (s_inode->fifo_head + 1) % MAX_RAM_VERSIONS;
	} else {
		s_inode->loaded_versions[s_inode->loaded_version_count++] = v_num;
	}

	new_v = kzalloc(sizeof(struct scorw_version), GFP_KERNEL);
	if (!new_v) return NULL;

	new_v->version_num = v_num;

	// Initialize the lockless Radix Tree for this specific version!
	xa_init(&new_v->delta_map);

	s_inode->version_states[idx] = new_v;

	// Load exactly this version's blocks from the log on demand
	scorw_replay_log_version(s_inode, v_num);

	return new_v;
}

void scorw_replay_log_version(struct scorw_inode *s_inode, int target_version) 
{
	struct inode *log_inode = s_inode->i_log_vfs_inode;
	loff_t log_size, offset = 0;
	struct page *page;
	char *kaddr;
	struct scorw_log_record *record;
	struct scorw_version *v;
	int i;

	if (!log_inode) return; // No log file exists yet!

	log_size = i_size_read(log_inode);
	//Commentedprintk("Replaying log file for version %d\n", target_version);
	// Read the log file page by page
	while (offset < log_size) {
		pgoff_t index = offset >> PAGE_SHIFT;
		unsigned int page_offset = offset & (PAGE_SIZE - 1);
		unsigned int bytes_to_read = PAGE_SIZE - page_offset;

		if (offset + bytes_to_read > log_size)
			bytes_to_read = log_size - offset;

		// Grab the raw physical page from Ext4's mapping cache
		page = read_mapping_page(log_inode->i_mapping, index, NULL);
		if (IS_ERR(page)) {
			//Commentedprintk(KERN_ERR "SCORW: Failed to read log page!\n");
			break;
		}
		//Commentedprintk("[DEBUG] :: {%s} :: Read page_cache(%lu) for %lu\n" , __func__ , offset ,  log_inode->i_ino);
		// Map the page into kernel RAM (just like your copy loop!)
		kaddr = (char *)kmap_atomic(page);
		record = (struct scorw_log_record *)(kaddr + page_offset);

		// Loop through every 24-byte transaction record on this page
		while (bytes_to_read >= sizeof(struct scorw_log_record)) {

			// Safety check to avoid garbage data
			if (record->version_num == target_version) { 
				v = s_inode->version_states[target_version - 1];
				if (v) {
					// Store every block of this extent into the XArray!
					for (i = 0; i < record->len_blks; i++) {
						xa_store(&v->delta_map, 
								record->logical_start_blk + i,
								xa_mk_value(record->physical_start_blk + i), 
								GFP_KERNEL);
					}
				}
			}

			record++; // Move to the next 24-byte record
			bytes_to_read -= sizeof(struct scorw_log_record);
			offset += sizeof(struct scorw_log_record);
		}

		// Unmap and release the memory page so we don't leak RAM
		kunmap_atomic(kaddr);
		put_page(page); 
	}
}
/*
   void scorw_init_ram_and_log(struct inode *inode, struct scorw_inode *s_inode) {
   char log_name[64];
   struct file *log_file;
   struct dentry *dentry;
   int ret;

   if (!s_inode) return;

// 1. Get a dentry for this inode (vfs_getxattr needs a dentry)
dentry = d_find_alias(inode);
if (!dentry) {
//Commentedprintk("SCORW_DEBUG: HEAL FAIL - No dentry for Inode %lu\n", inode->i_ino);
return;
}

// 2. Fetch the log name directly from the xattr
// "user.scorw_log" is the attribute name you set in setxattr_generic
ret = vfs_getxattr(&nop_mnt_idmap, dentry, "user.scorw_log", log_name, 63);

// We are done with the dentry reference
dput(dentry);

if (ret <= 0) {
//Commentedprintk("SCORW_DEBUG: HEAL FAIL - Log xattr not found on Inode %lu\n", inode->i_ino);
return;
}
log_name[ret] = '\0'; // Ensure string is null-terminated

//Commentedprintk("SCORW_DEBUG: HEAL - Found log path '%s'\n", log_name);

// 3. Open the log file using the absolute path from your script
log_file = filp_open(log_name, O_RDONLY, 0);
if (IS_ERR(log_file)) {
//Commentedprintk("SCORW_DEBUG: HEAL FAIL - Cannot open '%s' (Error %ld)\n", log_name, PTR_ERR(log_file));
return;
}

s_inode->i_log_vfs_inode = file_inode(log_file);
//Commentedprintk("SCORW_DEBUG: HEAL SUCCESS - Log Inode is %lu\n", s_inode->i_log_vfs_inode->i_ino);

// We don't pre-fill the XArray unconditionally anymore
// scorw_replay_log_version will be called per-version on demand
}
 */
// Returns the Physical Block Number if modified, or '0' if it was never touched.
unsigned long scorw_lookup_physical_block(struct scorw_inode *s_inode, unsigned long target_logical_blk)
{
	struct scorw_inode *source = s_inode;
	int v;
	loff_t logical_size;

	if (!s_inode) return 0;

	if (s_inode->i_par_vfs_inode) {
		source = scorw_find_inode(s_inode->i_par_vfs_inode);
		if (!source) return 0;
	}

	for (v = s_inode->version; v >= 1; v--) {
		////Commentedprintk("[DEBUG] :: {%s} :: Seeing version=%d , i_ino=%lu , target_log_blk=%lu" , __func__ , v , s_inode->i_ino_num  , target_logical_blk);
		struct scorw_version *sv = scorw_get_or_create_version(source, v);

		if (sv) {
			void *found_blk = xa_load(&sv->delta_map, target_logical_blk);
			if (found_blk){
				//Commentedprintk("[DEBUG] :: {%s} found block = %lu , target logical block = %lu \n",__func__, xa_to_value(found_blk) , target_logical_blk); 
				 return xa_to_value(found_blk);
			} else {
			//	//Commentedprintk("not found verison checked = %d \n", v);
			}
		}
	}
	
	logical_size = scorw_get_original_parent_size(source->i_vfs_inode);
	logical_size /= PAGE_SIZE;
	if(target_logical_blk <	 logical_size){
		return target_logical_blk;
	}

	return BLK_NOT_FOUND;
}

int scorw_record_write(struct scorw_inode *s_inode, unsigned long logical_blk, unsigned long physical_blk, unsigned int len , int status, __u32 data_crc)
{
	struct scorw_version *active_v;
	struct scorw_log_record record;
	int i, curr_version;

	if (!s_inode) return -EINVAL;
	curr_version = s_inode -> version;
	if(status == SET_TRANSACTION){ 
		curr_version ++; /*We increase the version while END_TXN*/
	}
	// DEBUG: Let's see if we even enter the logger
	//Commentedprintk("SCORW_DEBUG: Attempting to log write for Inode %lu. Target Version: %d\n", 
			//s_inode->i_vfs_inode->i_ino, curr_version);

	// Ensure RAM state exists for the current parent version
	active_v = scorw_get_or_create_version(s_inode, curr_version);
	if (!active_v) {
		//Commentedprintk("SCORW_DEBUG: CRITICAL - Failed to create RAM version %d\n", s_inode->version);
		return -ENOMEM;
	}

	// Update XArray
	for (i = 0; i < len; i++) {
		xa_store(&active_v->delta_map, logical_blk + i, xa_mk_value(physical_blk + i), GFP_KERNEL);
		//Commentedprintk("SCORW_DEBUG: XA_STORE: Logical %lu -> Physical %lu (V%d)\n", 
			//	logical_blk + i, physical_blk + i, curr_version);
	}

	// Persist to Disk Log
	if (s_inode -> i_log_vfs_inode) {
		record.version_num = curr_version;
		record.logical_start_blk = logical_blk;
		record.physical_start_blk = physical_blk;
		record.len_blks = len;
		record.data_crc32c = data_crc;
		record.padding2 = 0;
		write_offset_log(s_inode->i_log_vfs_inode, i_size_read(s_inode->i_log_vfs_inode),sizeof(struct scorw_log_record), &record);	
	}

	return 0;
}
//MAHA_AARSH_end //

// Garbage Collection Structures and Helpers
struct gc_block_state {
    __u32 version;
    __u64 physical_block;
};

// Find the smallest active version >= target_ver. 
// Returns -1 if no active version >= target_ver.
static int scorw_gc_closest_active(int target_ver, int *active_vers, int num_active)
{
    int i;
    for (i = 0; i < num_active; i++) {
        if (active_vers[i] >= target_ver)
            return active_vers[i];
    }
    return -1;
}

// Helper to punch a hole for a contiguous range of blocks
void scorw_punch_hole_range(struct inode *inode, ext4_lblk_t start_lblk, ext4_lblk_t len_blks)
{
    ext4_lblk_t cur = start_lblk;
    ext4_lblk_t end = start_lblk + len_blks;
    int ret;
    struct ext4_map_blocks map;
    handle_t *handle;

    while (cur < end) {
        map.m_lblk = cur;
        map.m_len = end - cur;
        ret = ext4_map_blocks(NULL, inode, &map, 0);
        
        if (ret < 0) {
            printk(KERN_ERR "SCORW GC: ext4_map_blocks error %d\n", ret);
            break;
        }
        
        /* If it's a hole (not mapped), simply advance and skip */
        if (ret == 0 || !(map.m_flags & EXT4_MAP_MAPPED)) {
            cur += map.m_len ? map.m_len : 1;
            continue;
        }
        
        /* Mapped! Punch it securely using Ext4 internals. */
        down_write(&EXT4_I(inode)->i_data_sem);
        ext4_discard_preallocations(inode);

        ext4_es_remove_extent(inode, cur, map.m_len);

        if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)) {
            ret = ext4_ext_remove_space(inode, cur, cur + map.m_len - 1);
        } else {
            handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, ext4_blocks_for_truncate(inode));
            if (!IS_ERR(handle)) {
                ret = ext4_ind_remove_space(handle, inode, cur, cur + map.m_len);
                ext4_journal_stop(handle);
            } else {
                ret = PTR_ERR(handle);
            }
        }

        if (ret == 0)
            ext4_es_insert_extent(inode, cur, map.m_len, ~0, EXTENT_STATUS_HOLE, 0);
        
        up_write(&EXT4_I(inode)->i_data_sem);
        
        if (ret) {
            printk(KERN_ERR "SCORW GC: Hole punch failed at lblk %u len %u (err %d)\n", cur, map.m_len, ret);
            break;
        }

        cur += map.m_len;
    }
}

// Main logic for finding dynamically unneeded blocks and punching parent holes
// Find the smallest active version 'a' such that a >= target_ver.
int scorw_gc_find_next_active(int target_ver, int *active_vers, int num_active)
{
    int i;
    for (i = 0; i < num_active; i++) {
        if (active_vers[i] >= target_ver)
            return active_vers[i];
    }
    return -1;
}

void scorw_gc_blocks(struct scorw_inode *s_inode)
{
    struct inode *log_inode, *par_inode;
    int active_vers[SCORW_MAX_CHILDS + 2]; // +2 for Version 0 and Parent
    int num_active = 0;
    struct xarray state_map; // Maps Logical -> {Version, Physical}
    struct xarray garbage_blocks;
    struct page *page;
    loff_t offset = 0, log_size;

    if (!s_inode || !s_inode->i_log_vfs_inode) return;

    par_inode = s_inode->i_par_vfs_inode ? s_inode->i_par_vfs_inode : s_inode->i_vfs_inode;
    log_inode = s_inode->i_log_vfs_inode;

    // 1. COLLECT ALL ACTIVE VERSIONS
    active_vers[num_active++] = 0; // Always include Version 0 (Base)

    for (int i = 0; i < SCORW_MAX_CHILDS; i++) {
        unsigned long child_ino = scorw_get_child_i_attr_val(par_inode, i);
        if (child_ino) {
            struct inode *c_inode = ext4_iget(par_inode->i_sb, child_ino, EXT4_IGET_NORMAL);
            if (!IS_ERR_OR_NULL(c_inode)) {
                // CHECK BOTH POSSIBLE XATTR NAMES TO BE SAFE
                unsigned long v = scorw_get_curr_version_attr_val(c_inode);
                if (v == 0) v = scorw_get_child_version_attr_val(c_inode);
                
                // Add to list if unique
                bool found = false;
                for(int k=0; k<num_active; k++) if(active_vers[k] == v) found = true;
                if(!found) active_vers[num_active++] = (int)v;
                
                iput(c_inode);
            }
        }
    }
    // Add parent's current version
    bool p_found = false;
    for(int k=0; k<num_active; k++) if(active_vers[k] == s_inode->version) p_found = true;
    if(!p_found) active_vers[num_active++] = s_inode->version;

    // Sort active versions (Selection Sort)
    for (int i = 0; i < num_active - 1; i++) {
        for (int j = i + 1; j < num_active; j++) {
            if (active_vers[i] > active_vers[j]) {
                int tmp = active_vers[i];
                active_vers[i] = active_vers[j];
                active_vers[j] = tmp;
            }
        }
    }

    xa_init(&state_map);
    xa_init(&garbage_blocks);
    log_size = i_size_read(log_inode);

    // 2. SCAN LOG
    while (offset < log_size) {
        pgoff_t index = offset >> PAGE_SHIFT;
        unsigned int page_off = offset & (PAGE_SIZE - 1);
        page = read_mapping_page(log_inode->i_mapping, index, NULL);
        if (IS_ERR(page)) break;

        char *kaddr = kmap_atomic(page);
        struct scorw_log_record *rec = (struct scorw_log_record *)(kaddr + page_off);
        unsigned int bytes_left = min_t(loff_t, PAGE_SIZE - page_off, log_size - offset);

        while (bytes_left >= sizeof(struct scorw_log_record)) {
            if (rec->version_num > 0) {
                for (int k = 0; k < rec->len_blks; k++) {
                    unsigned long L = rec->logical_start_blk + k;
                    unsigned long P_new = rec->physical_start_blk + k;
                    
                    struct gc_block_state *old_state = xa_load(&state_map, L);
                    int v_prev = old_state ? old_state->version : 0;
                    unsigned long p_prev = old_state ? old_state->physical_block : L;

                    // CORE LOGIC: Find if anyone is still using p_prev
                    // They use p_prev if their version 'a' is: v_prev <= a < rec->version_num
                    int a_next = scorw_gc_find_next_active(v_prev, active_vers, num_active);
                    
                    if (a_next == -1 || a_next >= rec->version_num) {
                        // NO ONE is in the version range that needs p_prev. It's garbage!
                        // Special check: Only punch if p_prev is an appended block (P >= OriginalSize)
                        // to avoid destroying the base file if that's your policy.
                        xa_store(&garbage_blocks, p_prev, xa_mk_value(1), GFP_KERNEL);
                    }

                    // Update the state map to the new version/physical location
                    if (!old_state) old_state = kmalloc(sizeof(*old_state), GFP_KERNEL);
                    if (old_state) {
                        old_state->version = rec->version_num;
                        old_state->physical_block = P_new;
                        xa_store(&state_map, L, old_state, GFP_KERNEL);
                    }
                }
            }
            bytes_left -= sizeof(*rec);
            offset += sizeof(*rec);
            rec++;
        }
        kunmap_atomic(kaddr);
        put_page(page);
    }

    // 3. PUNCH HOLES
    unsigned long p_idx;
    void *entry;
    xa_for_each(&garbage_blocks, p_idx, entry) {
        scorw_punch_hole_range(par_inode, p_idx, 1);
    }

    // Cleanup
    unsigned long idx;
    struct gc_block_state *s;
    xa_for_each(&state_map, idx, s) kfree(s);
    xa_destroy(&state_map);
    xa_destroy(&garbage_blocks);
}

int scorw_internal_copy_blocks(struct file *file, loff_t src_pos, loff_t dest_pos, size_t len)
{
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	int status = 0;

	/* ALL VARIABLES DECLARED AT THE TOP TO SATISFY KERNEL C STANDARDS */
	unsigned long dest_offset;
	unsigned long src_offset;
	unsigned long bytes;
	pgoff_t src_index;
	struct folio *src_folio;
	struct folio *dest_folio;
	void *fsdata;
	struct page *src_page;
	struct page *dest_page;
	char *src_addr;
	char *dest_addr;

	// Loop until we have copied all requested bytes
	while (len > 0) {
		// Initialize pointers to NULL for safety at the start of each loop
		dest_folio = NULL;
		fsdata = NULL;

		// Calculate how much we can copy in this single 4KB page
		dest_offset = dest_pos & (PAGE_SIZE - 1);
		src_offset = src_pos & (PAGE_SIZE - 1);
		bytes = min_t(unsigned long, len, PAGE_SIZE - dest_offset);

		// 1. FETCH OLD DATA: Read the source page from the old version
		src_index = src_pos >> PAGE_SHIFT;
		src_folio = read_cache_folio(mapping, src_index, NULL, NULL);
		if (IS_ERR(src_folio)) {
			//Commentedprintk("scorw: Error reading source block at index %lu\n", src_index);
			return PTR_ERR(src_folio);
		}

		// 2. ALLOCATE NEW DATA: Ask Ext4 to prepare a new block at the end of the file
		status = a_ops->write_begin(file, mapping, dest_pos, bytes, &dest_folio, &fsdata);
		if (unlikely(status < 0)) {
			folio_put(src_folio); // Free the read block on error
			break;
		}

		// 3. MAP AND COPY: Map both pages into kernel memory
		src_page = &src_folio->page;
		dest_page = &dest_folio->page;

		src_addr = (char *)kmap_atomic(src_page);
		dest_addr = (char *)kmap_atomic(dest_page);

		// Do the actual byte-for-byte copy in RAM
		memcpy(dest_addr + dest_offset, src_addr + src_offset, bytes);

		// Unmap in reverse order (critical for kmap_atomic!)
		kunmap_atomic(dest_addr);
		kunmap_atomic(src_addr);

		// 4. COMMIT NEW DATA: Tell Ext4 the new block is ready
		status = a_ops->write_end(file, mapping, dest_pos, bytes, bytes, dest_folio, fsdata);

		// Let go of the old source folio so RAM doesn't fill up
		folio_put(src_folio);

		if (unlikely(status < 0))
			break;

		// Move our pointers forward for the next loop iteration
		src_pos += bytes;
		dest_pos += bytes;
		len -= bytes;
	}

	return status;
}

// Helper: Safely copy kernel buffer to page cache
static int scorw_write_kernel_data(struct file *file, loff_t dest_pos, void *kbuf, size_t len) 
{
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	struct folio *folio;
	void *fsdata = NULL;
	char *kaddr;
	int status;

	status = a_ops->write_begin(file, mapping, dest_pos, len, &folio, &fsdata);
	if (status < 0) return status;

	kaddr = (char *)kmap_atomic(&folio->page);
	memcpy(kaddr + (dest_pos & (PAGE_SIZE - 1)), kbuf, len);
	kunmap_atomic(kaddr);

	status = a_ops->write_end(file, mapping, dest_pos, len, len, folio, fsdata);
	return status;
}

// Main IOCTL Handler
long scorw_ioctl_see_thru_writev(struct file *file, unsigned long arg) 
{
	struct inode *inode = file_inode(file);
	struct scorw_inode *p_inode = scorw_find_inode(inode);
	struct scorw_writev_args args;
	struct scorw_io_vec *vecs = NULL;
	char *kbuf = NULL;
	loff_t append_pos;
	int i, ret = 0;
	int new_version;
	struct scorw_version *active_v;
	struct scorw_log_record record;

	if (!p_inode || !p_inode->is_see_thru_ro) 
		return -EINVAL;

	if (copy_from_user(&args, (void __user *)arg, sizeof(args))) 
		return -EFAULT;

	if (args.count == 0 || args.count > 1024) 
		return -EINVAL; 

	vecs = memdup_user(args.vecs, args.count * sizeof(struct scorw_io_vec));
	if (IS_ERR(vecs)) return PTR_ERR(vecs);

	kbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!kbuf) {
		ret = -ENOMEM;
		goto out_free_vecs;
	}

	inode_lock(inode);

	append_pos = (i_size_read(inode) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	new_version = p_inode->version + 1;

	/* --- PHASE 1: Write Data (CoW) --- */
	for (i = 0; i < args.count; i++) {
		unsigned long target_logical_blk = vecs[i].logical_offset / PAGE_SIZE;

		if (vecs[i].len > PAGE_SIZE) {
			ret = -EINVAL;
			goto out_unlock;
		}

		if (copy_from_user(kbuf, vecs[i].buf, vecs[i].len)) {
			ret = -EFAULT;
			goto out_unlock;
		}

		ret = scorw_internal_copy_blocks(file, target_logical_blk * PAGE_SIZE, append_pos, PAGE_SIZE);
		if (ret < 0) goto out_unlock;

		ret = scorw_write_kernel_data(file, append_pos + (vecs[i].logical_offset % PAGE_SIZE), 
				kbuf, vecs[i].len);
		if (ret < 0) goto out_unlock;

		append_pos += PAGE_SIZE; 
	}

	/* --- PHASE 2: Update RAM and Write Log Records --- */
	append_pos = (i_size_read(inode) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	active_v = scorw_get_or_create_version(p_inode, new_version);
	if (!active_v) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	for (i = 0; i < args.count; i++) {
		unsigned long target_logical_blk = vecs[i].logical_offset / PAGE_SIZE;
		unsigned long appended_ext4_blk = (append_pos + (i * PAGE_SIZE)) / PAGE_SIZE;
		__u32 crc;

		// Calculate CRC of the merged 4KB block from the page cache
		crc = calc_page_cache_crc(inode->i_mapping, appended_ext4_blk, 1);

		// Scenario C: Poison the CRC so recovery detects a mismatch
		if (unlikely(scorw_crash_with_bad_crc))
			crc = 0xDEADBEEF;

		// 1. Store in RAM XArray
		xa_store(&active_v->delta_map, target_logical_blk, xa_mk_value(appended_ext4_blk), GFP_KERNEL);

		// 2. Append to Disk Log
		if (p_inode->i_log_vfs_inode) {
			record.version_num = new_version;
			record.logical_start_blk = target_logical_blk;
			record.physical_start_blk = appended_ext4_blk;
			record.len_blks = 1; 
			record.data_crc32c = crc;
			record.padding2 = 0;

			write_offset_log(p_inode->i_log_vfs_inode, i_size_read(p_inode->i_log_vfs_inode), 
					sizeof(record), &record);
		}
	}

	/* --- PHASE 3: Commit (Atomicity Point) --- */
	p_inode->version = new_version;
	//scorw_set_curr_version_attr_val(inode, new_version);

	//Commentedprintk("SCORW_DEBUG: WRITEV COMMITTED. Inode: %lu, New Version: %d, Blocks: %u\n", 
			//inode->i_ino, new_version, args.count);

	/* ---- CRASH INJECTION POINT A (writev path) ---- */
	if (unlikely(scorw_crash_after_log_write)) {
		printk(KERN_CRIT "SCORW TEST [Scenario A / writev]: Flushing log inode %lu, "
			"then panicking. Parent data inode %lu NOT flushed.\n",
			p_inode->i_log_vfs_inode->i_ino, inode->i_ino);
		filemap_write_and_wait(p_inode->i_log_vfs_inode->i_mapping);
		sync_inode_metadata(p_inode->i_log_vfs_inode, 1);
		panic("SCORW TEST Scenario A / writev: crash after log write, before data flush");
	}

	/* ---- CRASH INJECTION POINT B (writev path) ---- */
	if (unlikely(scorw_crash_after_data_write)) {
		printk(KERN_CRIT "SCORW TEST [Scenario B / writev]: Flushing parent data inode %lu, "
			"then panicking. Log inode %lu NOT flushed.\n",
			inode->i_ino, p_inode->i_log_vfs_inode->i_ino);
		filemap_write_and_wait(inode->i_mapping);
		panic("SCORW TEST Scenario B / writev: crash after data write, before log flush");
	}

	/* ---- CRASH INJECTION POINT C (writev path) ---- */
	if (unlikely(scorw_crash_with_bad_crc)) {
		printk(KERN_CRIT "SCORW TEST [Scenario C / writev]: Bad CRC 0xDEADBEEF written. "
			"Flushing BOTH log inode %lu and parent data inode %lu, "
			"then panicking. xattr safe_version NOT updated.\n",
			p_inode->i_log_vfs_inode->i_ino, inode->i_ino);
		filemap_write_and_wait(p_inode->i_log_vfs_inode->i_mapping);
		sync_inode_metadata(p_inode->i_log_vfs_inode, 1);
		filemap_write_and_wait(inode->i_mapping);
		panic("SCORW TEST Scenario C / writev: bad CRC persisted, both files flushed");
	}

	ret = args.count;

out_unlock:
	inode_unlock(inode);
	kfree(kbuf);
out_free_vecs:
	kfree(vecs);
	return ret;
}


/*
static int writeback_inode(struct inode * inode){
	int ret;
	struct address_space *mapping = inode->i_mapping;
	//Commentedprintk("[SCORW_DEBUG] :: {%s} i_ino = %lu\n" , __func__ , inode->i_ino);
	ret = __filemap_fdatawrite_range(mapping , 0 , LLONG_MAX , WB_SYNC_ALL);	
	if(ret != -EIO){
		__filemap_fdatawait_range(mapping , 0 , LLONG_MAX);
	}
	//Commentedprintk("[SCORW_DEBUG] :: Returning from %s" , __func__);
	return ret;
}
*/

static void scorw_help_write_and_wait(struct inode* inode , struct file * file){
	/*char * err_msg = "WEIRD_ERROR";
	int ret;

	ret = filemap_fdatawrite(inode->i_mapping);
	if(ret){
		//Commentedprintk("filemap_fdatawrite failed\n");
	}
	//Commentedprintk("Congrats Part1 passed\n");
	
	ret = filemap_fdatawait_range(inode->i_mapping , 0 , LLONG_MAX);
	//Commentedprintk("Congrats Part2 passed\n");

	//Commentedprintk("Calling ext4_sync_file\n");
	ret = ext4_sync_file(file , 0 , LLONG_MAX , 0);
	if(ret){
		//Commentedprintk("Ooopsss..... some error\n");
		return;
	} 
	//Commentedprintk("Yayyyyyyyyyyyyyyyy , file synced\n");
*/
	return;
}

/*calling with val=SET_TRANSACTION increases scorw_inode->version by 1*/
long scorw_set_transaction(struct inode* inode , struct file * file , int val){
	int ret;
	struct scorw_inode * scorw_inode;
	struct Transaction_locks * t_locks;
	struct inode* log_inode;

	scorw_inode = inode->i_scorw_inode;
	log_inode = scorw_inode -> i_log_vfs_inode;
	if(!scorw_inode){
		return -1;
	}
	t_locks = &(scorw_inode->t_locks);


	if(val == SET_TRANSACTION || val == SET_NORMAL_WRITE){
		if(scorw_self_transaction_status(inode , file) == val){/*avoids deadlocks*/
			return -25;
		}	
		ret = mutex_lock_interruptible( &(t_locks->transaction_lock ));
		if(ret){
			return -67; /*error case when ioctl fails*/
		}
		t_locks->owner = file;
		t_locks->transaction = val;
	}
 
	else if(val == UNSET_TRANSACTION){
		if(!mutex_is_locked(&(t_locks->transaction_lock)) || (t_locks->owner != file  )){
			return -25;
		}
		
		if(t_locks->transaction == SET_TRANSACTION){
		//	scorw_help_write_and_wait(inode , file); /*Par sync*/
			/*Syncing log file*/
		//	filemap_write_and_wait(log_inode->i_mapping);
           	//	sync_inode_metadata(log_inode, 1); 
	
			

			__sync_fetch_and_add(&(scorw_inode -> version) , 1);
			//scorw_set_curr_version_attr_val(inode, scorw_inode->version);
			////Commentedprintk("[DEBUG] :: {%s} updated version from %d -> %d" , __func__ , (scorw_inode->version) - 1 , scorw_inode->version);
		}

		t_locks->owner = NULL;
		t_locks->transaction = val;
		mutex_unlock( &(t_locks->transaction_lock ));	
	}

	//Commentedprintk("[DEBUG] :: Set the scorw_inode->transaction=%d\n" , t_locks->transaction);

	return 0;
}



/*Returns -1 if someone else has acquired the lock or if it isn't locked.
  Returns the status if it is locked by itself struct file* file        */
int scorw_self_transaction_status(struct inode *inode , struct file* file){
	struct scorw_inode* scorw_inode;
	struct Transaction_locks *t_locks;
	scorw_inode = inode->i_scorw_inode;
	if(!scorw_inode){
		return -1;
	} 
	t_locks = &(scorw_inode->t_locks);
	if(!mutex_is_locked(&(t_locks->transaction_lock))){
		return -1;	
	}
	if( !(t_locks->owner) || (t_locks->owner) != file){ /*Lock has been acquired by some other process*/
		return -1;
	}
	return t_locks->transaction;
}


long scorw_set_transaction_error(struct inode *inode, struct file *file)
{
	struct scorw_inode *scorw_inode;
	struct Transaction_locks *t_locks;

	if (!inode){
		return -EINVAL;
	}

	scorw_inode = inode->i_scorw_inode;
	if (!scorw_inode){
		return -EINVAL;
	}
	t_locks = &(scorw_inode->t_locks);

	/* Optional ownership check */
	if (!mutex_is_locked(&(t_locks->transaction_lock))){
		return -EPERM;
	}
	if (t_locks->owner != file){
		return -EPERM;
	}


	t_locks->owner = NULL;
	t_locks->transaction = UNSET_TRANSACTION;

	mutex_unlock(&(t_locks->transaction_lock));

	return 0;
}



void init_Transaction_locks(struct scorw_inode * scorw_inode){
	struct Transaction_locks *t_locks;
	t_locks = &(scorw_inode->t_locks);
	mutex_init(&(t_locks->transaction_lock));
	t_locks->owner = NULL;
	t_locks->transaction = -1;
	return;		
}


// HAMARA CODE END //
