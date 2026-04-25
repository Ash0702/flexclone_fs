#ifndef _LINUX_CORW_SPARSE_H
#define _LINUX_CORW_SPARSE_H

#define SCORW_MAX_CHILDS 64    // maximum children allowed for a parent
#define MAX_RANGES_SUPPORTED 4 // maximum ranges allowed for a child
#define HASH_TABLE_SIZE_2 1048576
#define HASH_TABLE_SIZE_BITS_2 20
#define MIN_HASH_TABLE_SIZE 16 // 16 linked lists in hash table
#define MAX_VERSIONS 32
// Assumption: Info about largest chunk's copy status will fit into a page

#define PAGE_BYTES (PAGE_SIZE)
#define PAGE_BITS (PAGE_BYTES << 3)
#define PAGE_BLOCKS (PAGE_BITS)

#define CHILD_NAME_LEN 16
#define CHILD_RANGE_LEN 16

// MAHA_AARSH_start
#include <linux/xarray.h>
#define BLK_NOT_FOUND ~0UL
struct kiocb;
struct iov_iter;
// MAHA_AARSH_end

#ifdef USE_OLD_RANGE
struct child_range {
  long long start; // start block num
  long long
      end; // end block num
           // Note: range members are of type long long and not unsigned because
           // we store -1 in range start, end when range is invalid
};
#else

typedef enum {
  SNAPX_FLAG_COW,    // Defaault, need not be passed
  SNAPX_FLAG_COW_RO, // XXX Does it make sense in some use-case?
                     // On write to parent, copy of block will be created for
                     // child. However, write to child is disallowed.
  SNAPX_FLAG_SEE_TH, // Note: Write to parent blks marked as see through leads
                     // to modification of these blks in parent(cow doesn't
                     // happen in this case). However, write to child blks
                     // marked as see through leads to cow of blks i.e. child
                     // gets its own copy of original blks and modifications are
                     // performed on these blks.
  SNAPX_FLAG_SEE_TH_RO,
  SNAPX_FLAG_SHARED,
  SNAPX_FLAG_SHARED_RO, // XXX Does it make sense in some use-case?
  SNAPX_FLAG_MAX
} WriteCh;

struct child_range {
  long start;             // start block num
  long end;               // end block num
  WriteCh snapx_behavior; // control flags to decide write behavior
};

struct sharing_range_info {
  bool initialized;
  bool partial_cow;
  unsigned start_block;
  unsigned end_block;
};

struct child_range_xattr {
  u32 start;
  u32 end;
  u16 snap_behavior;
} __attribute__((__aligned__(2)));

#endif

// Reminder: LIST_HEAD(scorw_inodes_list) is also added
// inode: unsigned long
// size: long long
// num blocks in file: unsigned
// loff_t: long long

// MAHA_VERSION_AARSH_start
/* 1. DISK STRUCTURE: A single transaction record appended to your Log File.
      Because it is a fixed 24 bytes, appending it is incredibly fast.    */
struct scorw_log_record {
  __u32 version_num;        // e.g., Version 2
  __u32 logical_start_blk;  // Where the user wrote it (e.g., Block 5)
  __u64 physical_start_blk; // Where Ext4 actually put it on disk
  __u32 len_blks;           // How many blocks were written contiguously
  __u32 padding;            // Ensures 64-bit memory alignment
  __u64 padding2;           // Ensures 32bytes , which can divide 4Kb
};
/* Helper struct for syncying par and log*/
struct scorw_gc_sync_par_log {
  __u32 version_num;
  loff_t log_end_offset;
  loff_t par_latest_logical_block;
};

// 2. RAM STRUCTURE: The fast lookup tree for a single version.
struct scorw_version {
  int version_num;

  // Lockless Radix Tree: Maps Logical Block -> Physical Block
  struct xarray delta_map;
};

struct scorw_io_vec {
  __u64 logical_offset;
  __u32 len;
  void __user *buf;
};

struct scorw_writev_args {
  __u32 count;
  struct scorw_io_vec __user *vecs;
};

struct Transaction_locks {
  struct mutex transaction_lock;
  struct file *owner;
  int transaction;
};

// MAHA_VERSION_AARSH_end
struct scorw_inode {
  // inode number of inode this inode corresponds to
  unsigned long i_ino_num;

  // how many times this file is opened by processes?
  // atomic64_t i_process_usage_count;
  unsigned long i_process_usage_count;
  spinlock_t i_process_usage_count_lock;

  // number of threads using this inode.
  atomic64_t i_thread_usage_count;

  // vfs inode this inode corresponds to
  struct inode *i_vfs_inode;

  // vfs inode of parent of this inode
  struct inode *i_par_vfs_inode;

  // vfs inode of friend of this inode
  struct inode *i_frnd_vfs_inode;

  // size of parent when parent child relationship was established between this
  // child and its parent.
  long long i_copy_size;

  // num of blocks of data yet to be copied from src to dest
  atomic64_t i_pending_copy_pages;

  // linked list of scorw inodes
  struct list_head i_list;

  // lock that protects an scorw inode
  // For eg: protects i_child_scorw_inode[] of par
  struct rw_semaphore i_lock;

  // scorw inodes of children of a parent
  struct scorw_inode *i_child_scorw_inode[SCORW_MAX_CHILDS];
  int i_last_child_index; // last valid index of i_child_scorw_inode

  // list of chunks of child that are fully yet to be copied from parent to this
  // child
  struct list_head i_uncopied_extents_list;

  // lock that protects fully uncopied chunks list
  struct mutex i_uncopied_extents_list_lock;

  // maintain info about blocks that are currently operated on by a thread
  struct hlist_head *i_uncopied_blocks_list;
  // struct hlist_head *i_uncopied_blocks_list;

  // size of i_uncopied_blocks_list hash table i.e. number of linked lists to
  // create in this hash table Number of i_uncopied_blocks_lock is also
  // dependent on this value because we create one lock per linked list in the
  // hash table
  unsigned i_hash_table_size;

  // locks that protects being processed blocks list
  // Each lock protects a list in the hash array
  struct spinlock *i_uncopied_blocks_lock;

  // whether inode corresponding this scorw inode has been unlinked or not?
  unsigned i_ino_unlinked;

  // At which index in i_child_scorw_inode[] (array of children attached to par)
  // is the current child present?
  int i_at_index;

  // how many ranges are present corresponding a child?
  int i_num_ranges;

  // range of blocks corresponding which static snapshot of par file is to be
  // maintained
  struct child_range i_range[MAX_RANGES_SUPPORTED];

  // number of pagecopy structures added by the parent
  // and removed by the pagecopy thread
  unsigned long added_to_page_copy;
  unsigned long removed_from_page_copy;

  // MAHA_VERSION_AARSH_start
  int is_see_thru;
  int is_see_thru_ro;
  int version; // stores parent version for parent and child version for child

  struct scorw_version *version_states[MAX_VERSIONS]; // stores version states.

  int loaded_versions[4]; // FIFO queue of loaded version numbers
  int loaded_version_count;
  int fifo_head;

  struct inode *i_log_vfs_inode;
  unsigned long orig_size; // TODO : look at this

  struct Transaction_locks
      t_locks; /* Imagine this case , 2 process open the same file.
                  one of them calls BeginTransaction and the other
                  EndTransaction. Ideally it should error , but it won't without
                  a lock. We can put some unique file_pointer type shit
               */

  bool is_log_initialized; // to check if log file and ram structures have been
                           // initialized for this inode. This is needed because
                           // we want to delay initialization of log file and
                           // ram structures until we are sure that they are
                           // needed. We don't want to initialize them for all
                           // inodes that can potentially become parent because
                           // it will lead to too much overhead.
  /// MAHA_VERSION_AARSH_end
};

#define NOP 0x0
#define READING 0x1
#define COPYING_EXCL                                                           \
  0x10 // only single copying can happen. No read or other copying can happen in
       // parallel. Example usage: When write happens to parent such that block
       // needs to be copied from parent to child.
#define COPYING                                                                \
  0x100 // only single copying. Read can happen in parallel. Other copying
        // cannot happen in parallel. Example usage: When child is written such
        // that block needs to be copied from parent to child.

// No need to create this block for the case when this block has been already
// copied.
struct uncopied_block {
  unsigned block_num;
  int processing_type;
  int num_readers;
  int num_waiting;
  wait_queue_head_t wait_queue;
  struct hlist_node node;
};

// Note: FoR + Copying can happen in parallel
struct uncopied_extent {
  unsigned start_block_num;
  unsigned len;
  struct list_head list;
};

struct page_copy {
  unsigned block_num;
  struct scorw_inode *par; // scorw inode of parent. Useful to find parent inode
                           // num, children inodes
  struct page *data_page;  // page to store data
  // struct mutex lock;
  int data_page_loaded;     // Has data been loaded into data_page?
  struct list_head ll_node; // linked list node
  struct hlist_node h_node; // hash table node
  unsigned char
      is_target_child[SCORW_MAX_CHILDS]; // Flags to indicate the children which
                                         // are the target of this page copy
};

// structure used to perform child/frnd version information processing
struct wait_for_commit {
  struct inode *frnd; // frnd inode corresponding child inode waiting for commit
  int child_staged_for_commit; // has child inode been chosen for commit in or
                               // before current commit() round?
  struct list_head list;
};

// structure used to perform child/frnd version information processing
struct pending_frnd_version_cnt_inc {
  struct inode *child; // child inode corresponding which frnd inode is waiting
                       // for version update
  struct inode *frnd;  // frnd inode waiting for version update
  unsigned long
      iter_id; // asynch thread scans all queued requests and sets an identifier
               // to each processed requests. This helps asynch thread to know
               // that it has reached end of processing of all queued requests
               // in current iteration
  struct list_head list;
};

struct inode_policy {
  struct scorw_inode *(*select_inode)(struct inode_policy *);
  int (*inode_added)(struct inode_policy *, struct scorw_inode *);
  int (*inode_removed)(struct inode_policy *, struct scorw_inode *);
  void *private; // stores scorw inode currently processed by scorw thread
};

struct extent_policy {
  struct uncopied_extent *(*select_extent)(struct scorw_inode *,
                                           struct extent_policy *);
  void *private;
};

// size of length i.e. datatype of extent length is diff. in extent tree and
// extent status tree. Thus, creating our own extent structure to handle both
// and fix code with min. changes.
struct scorw_extent {
  __u32 ee_block; /* first logical block extent covers */
  __u32 ee_len;   /* number of blocks covered by extent */
};

//////////////////////
// error messages
//////////////////////
#define SCORW_ERR_MAX_CHILDS_ALREADY_SET                                       \
  -8000 // parent already has max childs allowed in extended attributes
#define SCORW_ERR_NON_EMPTY_FILE                                               \
  -8001 // Trying to convert non-empty file into corw sparse file.

#define SCORW_OUT_OF_MEMORY -8002
#define SCORW_PARENT_NOT_FOUND -8003
#define SCORW_PARENT_ERROR -8004

/*MAHA_AARSH_START*/
#define MAX_RAM_VERSIONS 4
#define SET_NORMAL_WRITE 2
#define SET_TRANSACTION 1
#define UNSET_TRANSACTION 0
/*MAHA_AARSH_END*/
///////////////////////
// Information messages (Return values that try to convey something to caller
// other than error)
//////////////////////
#define SCORW_PERFORM_ORIG_READ -9000 // return

/////////////
// API's
/////////////
int scorw_is_child_file(struct inode *inode, int consult_extended_attributes);
int scorw_is_par_file(struct inode *inode, int consult_extended_attributes);
int scorw_is_child_inode(struct scorw_inode *scorw_inode);

void scorw_prepare_child_inode(struct scorw_inode *scorw_inode,
                               struct inode *vfs_inode, int new_sparse);
void scorw_unprepare_child_inode(struct scorw_inode *scorw_inode);

void scorw_prepare_par_inode(struct scorw_inode *scorw_inode,
                             struct inode *par_vfs_inode);
void scorw_unprepare_par_inode(struct scorw_inode *scorw_inode);

int scorw_init_friend_file(struct inode *frnd_inode, long long child_copy_size);

void scorw_add_inode_list(struct scorw_inode *scorw_inode);
void scorw_remove_inode_list(struct scorw_inode *scorw_inode);

void scorw_set_block_copied_bit(struct scorw_inode *scorw_inode, loff_t lblk);
int scorw_get_block_copied_bit(struct scorw_inode *scorw_inode, loff_t lblk);

struct scorw_inode *scorw_alloc_inode(void);
void scorw_free_inode(struct scorw_inode *scorw_inode);

void scorw_print_inode_list(void);
struct scorw_inode *scorw_search_inode_list(unsigned long ino_num);
void scorw_print_uncopied_blocks_list(struct scorw_inode *scorw_inode);

unsigned long scorw_get_child_i_attr_val(struct inode *inode, int child_i);
void scorw_remove_child_i_attr(struct inode *inode, int child_i);
unsigned long scorw_get_parent_attr_val(struct inode *inode);

void scorw_inc_usage_count(struct scorw_inode *scorw_inode);
void scorw_dec_usage_count(struct scorw_inode *scorw_inode);
int scorw_get_usage_count(struct scorw_inode *scorw_inode);

#ifdef USE_OLD_RANGE
int scorw_write_child_blocks_begin(struct inode *c_inode, loff_t offset,
                                   size_t len, void **uncopied_block);
#else
int scorw_write_child_blocks_begin(struct inode *c_inode, loff_t offset,
                                   size_t len, void **uncopied_block,
                                   struct sharing_range_info *shr_info);
#endif

#ifdef USE_OLD_RANGE
int scorw_write_child_blocks_end(struct inode *c_inode, loff_t offset,
                                 size_t len,
                                 struct uncopied_block *uncopied_block);
#else
int scorw_write_child_blocks_end(struct inode *c_inode, loff_t offset,
                                 size_t len,
                                 struct uncopied_block *uncopied_block,
                                 bool shared);
#endif
int scorw_write_par_blocks(struct inode *c_inode, loff_t offset, size_t len,
                           struct page *page);
// int scorw_copy_on_read_child_blocks(struct inode* c_inode, loff_t offset,
// size_t len);
ssize_t scorw_follow_on_read_child_blocks(struct inode *c_inode,
                                          struct kiocb *iocb,
                                          struct iov_iter *to);

int scorw_unlink_child_file(struct inode *inode);
int scorw_unlink_par_file(struct inode *inode);

int scorw_put_inode(struct inode *inode, int is_child_inode,
                    int is_thread_putting, int is_par_putting);
struct scorw_inode *scorw_get_inode(struct inode *inode, int is_child_inode,
                                    int new_sparse);
struct scorw_inode *scorw_find_inode(struct inode *inode);

int scorw_thread_init(void);
int scorw_thread_exit(void);

void scorw_init(void);
void scorw_exit(void);

int scorw_copy_page_to_page_copy(struct page_copy *page_copy, int is_4KB_write,
                                 struct page *par_data_page);
void special_open(struct inode *par_inode, struct inode *child_inode,
                  int index);

void scorw_read_barrier_begin(struct scorw_inode *p_scorw_inode,
                              unsigned block_num,
                              struct uncopied_block **uncopied_block);
void scorw_read_barrier_end(struct scorw_inode *p_scorw_inode,
                            unsigned block_num,
                            struct uncopied_block **uncopied_block);

// MAHA_VERSION_AARSH_start
// extern int scorw_get_log_file_name_attr(struct inode *inode, char *name);
extern ssize_t ext4_file_write_iter(struct kiocb *iocb, struct iov_iter *from);
loff_t scorw_write_see_thru_ro(struct file *file, struct iov_iter *i,
                               loff_t pos);
void write_offset_log(struct inode *l_inode, loff_t offset, int len, void *ptr);
int scorw_internal_copy_blocks(struct file *file, loff_t src_pos,
                               loff_t dest_pos, size_t len);
unsigned long scorw_get_curr_version_attr_val(struct inode *inode);
void scorw_set_curr_version_attr_val(struct inode *inode,
                                     unsigned long val /*Value to be set*/);
unsigned long scorw_get_original_parent_size(struct inode *p_inode);
int update_version(struct inode *c_inode);
unsigned long scorw_get_log_attr_val(struct inode *inode);
void scorw_init_ram_and_log(struct inode *vfs_inode,
                            struct scorw_inode *scorw_inode);
struct scorw_version *scorw_get_or_create_version(struct scorw_inode *s_inode,
                                                  int v_num);
void scorw_cleanup_versions(struct scorw_inode *s_inode);
void scorw_replay_log_version(struct scorw_inode *s_inode, int target_version);
unsigned long scorw_lookup_physical_block(struct scorw_inode *s_inode,
                                          unsigned long target_logical_blk);
int scorw_record_write(struct scorw_inode *s_inode, unsigned long logical_blk,
                       unsigned long physical_blk, unsigned int len,
                       int status);
long scorw_ioctl_see_thru_writev(struct file *file, unsigned long arg);
long scorw_set_transaction(struct inode * inode , struct file * file , int val);
int scorw_self_transaction_status(struct inode * inode , struct file* file);
long scorw_set_transaction_error(struct inode *inode, struct file *file);
void init_Transaction_locks(struct scorw_inode * scorw_inode);
int scorw_get_see_thru_attr_val(struct inode *inode);
void scorw_replay_log_version(struct scorw_inode *s_inode, int target_version);
u32 scorw_get_last_open_time(struct inode *inode);
u32 scorw_set_last_open_time(struct inode *inode);
int scorw_is_opened_first_time(struct inode *inode);
void scorw_punch_hole_range(struct inode *inode, __u32 start_lblk,
                            __u32 len_blks);
void scorw_gc_blocks(struct scorw_inode *s_inode);
int scorw_gc_find_next_active(int target_ver, int *active_vers, int num_active);
int scorw_gc_parent_log(struct scorw_inode* scorw_inode);
void scorw_truncate_file(struct inode* inode , loff_t new_size);
// MAHA_VERSION_AARSH_end
#endif
