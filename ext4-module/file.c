// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 */

#include "acl.h"
#include "corw_sparse.h"
#include "ext4.h"
#include "ext4_jbd2.h"
#include "truncate.h"
#include "xattr.h"
#include <linux/backing-dev.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/pagevec.h>
#include <linux/path.h>
#include <linux/quotaops.h>
#include <linux/time.h>
#include <linux/uio.h>


///////// scorw start //////////
int scorw_dquot_file_open(struct inode *inode, struct file *file);
extern void ext4_es_print_tree(struct inode *inode);
// Add this so file.c can "see" the read hook
extern ssize_t scorw_read_from_parent(struct scorw_inode *scorw_inode,
                                      struct kiocb *iocb, struct iov_iter *to,
                                      unsigned batch_start_blk,
                                      unsigned batch_end_blk);
ssize_t scorw_generic_perform_write(struct file *file, struct iov_iter *i,
                                    loff_t pos, int write_to_par);
///////// scorw end //////////

/*
 * Returns %true if the given DIO request should be attempted with DIO, or
 * %false if it should fall back to buffered I/O.
 *
 * DIO isn't well specified; when it's unsupported (either due to the request
 * being misaligned, or due to the file not supporting DIO at all), filesystems
 * either fall back to buffered I/O or return EINVAL.  For files that don't use
 * any special features like encryption or verity, ext4 has traditionally
 * returned EINVAL for misaligned DIO.  iomap_dio_rw() uses this convention too.
 * In this case, we should attempt the DIO, *not* fall back to buffered I/O.
 *
 * In contrast, in cases where DIO is unsupported due to ext4 features, ext4
 * traditionally falls back to buffered I/O.
 *
 * This function implements the traditional ext4 behavior in all these cases.
 */
static bool ext4_should_use_dio(struct kiocb *iocb, struct iov_iter *iter) {
  struct inode *inode = file_inode(iocb->ki_filp);
  u32 dio_align = ext4_dio_alignment(inode);

  if (dio_align == 0)
    return false;

  if (dio_align == 1)
    return true;

  return IS_ALIGNED(iocb->ki_pos | iov_iter_alignment(iter), dio_align);
}

static ssize_t ext4_dio_read_iter(struct kiocb *iocb, struct iov_iter *to) {
  ssize_t ret;
  struct inode *inode = file_inode(iocb->ki_filp);

  if (iocb->ki_flags & IOCB_NOWAIT) {
    if (!inode_trylock_shared(inode))
      return -EAGAIN;
  } else {
    inode_lock_shared(inode);
  }

  if (!ext4_should_use_dio(iocb, to)) {
    inode_unlock_shared(inode);
    /*
     * Fallback to buffered I/O if the operation being performed on
     * the inode is not supported by direct I/O. The IOCB_DIRECT
     * flag needs to be cleared here in order to ensure that the
     * direct I/O path within generic_file_read_iter() is not
     * taken.
     */
    iocb->ki_flags &= ~IOCB_DIRECT;
    return generic_file_read_iter(iocb, to);
  }

  ret = iomap_dio_rw(iocb, to, &ext4_iomap_ops, NULL, 0, NULL, 0);
  inode_unlock_shared(inode);

  file_accessed(iocb->ki_filp);
  return ret;
}

#ifdef CONFIG_FS_DAX
static ssize_t ext4_dax_read_iter(struct kiocb *iocb, struct iov_iter *to) {
  struct inode *inode = file_inode(iocb->ki_filp);
  ssize_t ret;

  if (iocb->ki_flags & IOCB_NOWAIT) {
    if (!inode_trylock_shared(inode))
      return -EAGAIN;
  } else {
    inode_lock_shared(inode);
  }
  /*
   * Recheck under inode lock - at this point we are sure it cannot
   * change anymore
   */
  if (!IS_DAX(inode)) {
    inode_unlock_shared(inode);
    /* Fallback to buffered IO in case we cannot support DAX */
    return generic_file_read_iter(iocb, to);
  }
  ret = dax_iomap_rw(iocb, to, &ext4_iomap_ops);
  inode_unlock_shared(inode);

  file_accessed(iocb->ki_filp);
  return ret;
}
#endif

static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to) {
  struct inode *inode = file_inode(iocb->ki_filp);
  struct scorw_inode *s_inode;
  int is_child_file;

    if (unlikely(ext4_forced_shutdown(inode->i_sb)))
        return -EIO;

  if (!iov_iter_count(to))
    return 0;

  ///////////// SCORW START /////////////
  s_inode = scorw_find_inode(inode);
  is_child_file = scorw_is_child_file(inode, 0);

    if (s_inode) {
        // Self-heal parent logs if needed
         if (is_child_file && s_inode->i_par_vfs_inode) {
            if (! (s_inode->i_par_vfs_inode->i_scorw_inode) ) {
		//Commentedprintk("Called at MARK1\n");
                scorw_get_inode(s_inode->i_par_vfs_inode, 0, 0); // TODO : FIXbug REMOVE THIS XXX
            }
        }

    // Route versioned files
    if (is_child_file || s_inode->i_log_vfs_inode != NULL) {
      if (!(iocb->ki_flags & IOCB_DIRECT)) {
        struct address_space *orig_mapping = iocb->ki_filp->f_mapping;
        ssize_t ret;

        // THE HIJACK: Point to Parent's RAM if it's a child
        if (is_child_file && s_inode->i_par_vfs_inode) {
          iocb->ki_filp->f_mapping = s_inode->i_par_vfs_inode->i_mapping;
        }

        ret = scorw_read_from_parent(s_inode, iocb, to, 0, 0);

        // RESTORE: Prevent kernel panics
        iocb->ki_filp->f_mapping = orig_mapping;
        return ret;
      } else {
        return 0; // Abort direct IO
      }
    }
  }
  ///////////// SCORW END /////////////

#ifdef CONFIG_FS_DAX
  if (IS_DAX(inode))
    return ext4_dax_read_iter(iocb, to);
#endif
  if (iocb->ki_flags & IOCB_DIRECT)
    return ext4_dio_read_iter(iocb, to);

  return generic_file_read_iter(iocb, to);
}

static ssize_t ext4_file_splice_read(struct file *in, loff_t *ppos,
                                     struct pipe_inode_info *pipe, size_t len,
                                     unsigned int flags) {
  struct inode *inode = file_inode(in);

  if (unlikely(ext4_forced_shutdown(inode->i_sb)))
    return -EIO;
  return filemap_splice_read(in, ppos, pipe, len, flags);
}

/*
 * Called when an inode is released. Note that this is different
 * from ext4_file_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static int ext4_release_file(struct inode *inode, struct file *filp)
{
	
        ////////// scorw start //////////
        //scorw_print_inode_list();
	long err;
	
	//Commentedprintk("[DEBUG] :: %s called with inode_number = %ld" , __func__ , inode->i_ino);

  if (scorw_is_child_file(inode, 0)) {
    // This lock makes sure that open(par),close(par) and creation of par scorw
    // inodes inside special_open() happens atomically
    mutex_lock(&(inode->i_vfs_inode_open_close_lock));
    atomic_sub(1, &(inode->i_vfs_inode_open_count));

    scorw_put_inode(inode->i_scorw_inode->i_par_vfs_inode, 0, 0, 0); // XXX

                mutex_unlock(&(inode->i_vfs_inode_open_close_lock));
        }
        else if(scorw_is_par_file(inode, 0))
        {
        	//Commentedprintk("%s : Entered scorw_is_par_file\n" , __func__);	
	        //This lock makes sure that open(par),close(par) and creation of par scorw inodes inside special_open() happens atomically
            mutex_lock(&(inode->i_vfs_inode_open_close_lock));
			atomic_sub(1, &(inode->i_vfs_inode_open_count));
			/*Incase some malicious user never calls Begin_txn but tries to exit without End_txn*/
			if(scorw_self_transaction_status(inode , filp) == SET_TRANSACTION){
				err = scorw_set_transaction_error(inode, filp);
				if(err){
					printk("You're cooked, Transaction_lock for i_ino=%lu has been acquired forever\n" , inode->i_ino);
					return -25;
				}
			}
		
            scorw_put_inode(inode, 0, 0, 0);

            mutex_unlock(&(inode->i_vfs_inode_open_close_lock));
        }

  ////////// scorw end //////////

  if (ext4_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
    ext4_alloc_da_blocks(inode);
    ext4_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
  }
  /* if we are the last writer on the inode, drop the block reservation */
  if ((filp->f_mode & FMODE_WRITE) &&
      (atomic_read(&inode->i_writecount) == 1) &&
      !EXT4_I(inode)->i_reserved_data_blocks) {
    down_write(&EXT4_I(inode)->i_data_sem);
    ext4_discard_preallocations(inode);
    up_write(&EXT4_I(inode)->i_data_sem);
  }
  if (is_dx(inode) && filp->private_data)
    ext4_htree_free_dir_info(filp->private_data);

  return 0;
}

/*
 * This tests whether the IO in question is block-aligned or not.
 * Ext4 utilizes unwritten extents when hole-filling during direct IO, and they
 * are converted to written only after the IO is complete.  Until they are
 * mapped, these blocks appear as holes, so dio_zero_block() will assume that
 * it needs to zero out portions of the start and/or end block.  If 2 AIO
 * threads are at work on the same unwritten block, they must be synchronized
 * or one thread will zero the other's data, causing corruption.
 */
static bool ext4_unaligned_io(struct inode *inode, struct iov_iter *from,
                              loff_t pos) {
  struct super_block *sb = inode->i_sb;
  unsigned long blockmask = sb->s_blocksize - 1;

  if ((pos | iov_iter_alignment(from)) & blockmask)
    return true;

  return false;
}

static bool ext4_extending_io(struct inode *inode, loff_t offset, size_t len) {
  if (offset + len > i_size_read(inode) ||
      offset + len > EXT4_I(inode)->i_disksize)
    return true;
  return false;
}

/* Is IO overwriting allocated or initialized blocks? */
static bool ext4_overwrite_io(struct inode *inode, loff_t pos, loff_t len,
                              bool *unwritten) {
  struct ext4_map_blocks map;
  unsigned int blkbits = inode->i_blkbits;
  int err, blklen;

  if (pos + len > i_size_read(inode))
    return false;

  map.m_lblk = pos >> blkbits;
  map.m_len = EXT4_MAX_BLOCKS(len, pos, blkbits);
  blklen = map.m_len;

  err = ext4_map_blocks(NULL, inode, &map, 0);
  if (err != blklen)
    return false;
  /*
   * 'err==len' means that all of the blocks have been preallocated,
   * regardless of whether they have been initialized or not. We need to
   * check m_flags to distinguish the unwritten extents.
   */
  *unwritten = !(map.m_flags & EXT4_MAP_MAPPED);
  return true;
}

static ssize_t ext4_generic_write_checks(struct kiocb *iocb,
                                         struct iov_iter *from) {
  struct inode *inode = file_inode(iocb->ki_filp);
  ssize_t ret;

  if (unlikely(IS_IMMUTABLE(inode)))
    return -EPERM;

  ret = generic_write_checks(iocb, from);
  if (ret <= 0)
    return ret;

  /*
   * If we have encountered a bitmap-format file, the size limit
   * is smaller than s_maxbytes, which is for extent-mapped files.
   */
  if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
    struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

    if (iocb->ki_pos >= sbi->s_bitmap_maxbytes)
      return -EFBIG;
    iov_iter_truncate(from, sbi->s_bitmap_maxbytes - iocb->ki_pos);
  }

  return iov_iter_count(from);
}

static ssize_t ext4_write_checks(struct kiocb *iocb, struct iov_iter *from) {
  ssize_t ret, count;

  count = ext4_generic_write_checks(iocb, from);
  if (count <= 0)
    return count;

  ret = file_modified(iocb->ki_filp);
  if (ret)
    return ret;
  return count;
}

static ssize_t ext4_buffered_write_iter(struct kiocb *iocb,
                                        struct iov_iter *from) {
  ssize_t ret;
  struct inode *inode = file_inode(iocb->ki_filp);
  struct uncopied_block *uncopied_block = NULL;
  int write_to_child = 0;
  int error = 0;
  loff_t offset = iocb->ki_pos;
  size_t len = iov_iter_count(from);
  int write_to_par = 0;
#ifndef USE_OLD_RANGE
  struct sharing_range_info shr = {
      .initialized = false,
      .partial_cow = false,
      .start_block = 0,
      .end_block = 0,
  };
  struct inode *p_inode = NULL;
  struct address_space *mapping = NULL;
#endif

  if (iocb->ki_flags & IOCB_NOWAIT)
    return -EOPNOTSUPP;

  // inode_lock(inode);       :we lock later
  ret = ext4_write_checks(iocb, from);
  if (ret <= 0)
    goto out;

  // ret = generic_perform_write(iocb, from);
  //////////////////////// scorw start /////////////////////////
  //// 	Idea obtained from generic_file_buffered_read()	/////
  ////////////////////////////////////////////////////////////
  // inode_lock(inode);	//<--------------- Remove it after debugging

	if(scorw_is_child_file(inode, 0))
	{	
		write_to_child = 1;
		////Commentedprintk("ext4_buffered_write_iter: Writing to child file\n");
#ifdef USE_OLD_RANGE
    error = scorw_write_child_blocks_begin(inode, offset, len,
                                           (void *)(&(uncopied_block)));
#else
    error = scorw_write_child_blocks_begin(inode, offset, len,
                                           (void *)(&(uncopied_block)), &shr);
#endif
    if (error) {
      return error;
    }
#ifndef USE_OLD_RANGE
	      if(shr.initialized || shr.partial_cow){
	           unsigned long last_offset = offset + len - 1; 
		   unsigned long last_blk_allowed = shr.end_block;
		   unsigned long last_offset_allowed = (last_blk_allowed << PAGE_SHIFT) + 4095;
		   ////Commentedprintk(KERN_INFO "Before: offset: %lld, last_offset: %lld, last_offset_allowed: %lld, len: %lld\n", offset, last_offset, last_offset_allowed, len);
		   if(last_offset_allowed < last_offset){
			  //len -= last_offset - last_offset_allowed; 
			  len = last_offset_allowed - offset + 1; 
			  iov_iter_truncate(from, len);
		   }
		   ////Commentedprintk(KERN_INFO "After: offset: %lld, last_offset: %lld, last_offset_allowed: %lld, len: %lld\n", offset, last_offset, last_offset_allowed, len);
	      }	      
	      if(shr.initialized){
		      struct scorw_inode* scorw_inode = scorw_find_inode(inode);
                      BUG_ON(!scorw_inode || scorw_inode->i_vfs_inode != inode);
                      p_inode = scorw_inode->i_par_vfs_inode;
		      mapping = iocb->ki_filp->f_mapping;
		      ////Commentedprintk(KERN_INFO "Writing shared at %lld of len %lld\n", offset, len);
	              inode_lock(p_inode);
                      ////Commentedprintk("[pid: %u] ext4_buffered_write_iter: locking par inode at line 417 : %lu\n", current->pid, p_inode->i_ino);
		      iocb->ki_filp->f_mapping = p_inode->i_mapping; 
	              //current->backing_dev_info = inode_to_bdi(inode); Rajan: commented this
	              ret = scorw_generic_perform_write(iocb->ki_filp, from, iocb->ki_pos, write_to_par);
		      iocb->ki_filp->f_mapping = mapping; 
	              //current->backing_dev_info = NULL; Rajan: commented this
	              inode_unlock(p_inode);
		      goto finalize_out;
	      }	      
#endif
	}
	//Note: scorw_write_par_blocks is now being called from scorw_generic_perform_write 
	else if(scorw_is_par_file(inode, 0))
	{
		////Commentedprintk("ext4_buffered_write_iter: Writing to parent file\n");
		write_to_par = 1;
		
		// error = scorw_write_par_blocks(inode, offset, len);
		// if(error)
		// {
		// 	return error;
		// }
		
	}
	/*
	else
	{
		//Commentedprintk("ext4_buffered_write_iter: Writing to neither child nor parent file\n");
	}
	*/
	
	
        ////////// scorw end//////////
	inode_lock(inode);	//<--------------- Uncomment it after debugging
	////Commentedprintk("[pid: %u] ext4_buffered_write_iter: after locking inode at line 452: %lu\n", current->pid, inode->i_ino);

  // Apart from checks, inode time is also updated within this function.
  // In inode time updation fn file_update_time,following comment is found:
  //	Update the mtime and ctime members of an inode and mark the inode for
  //writeback. It internally calls,  mnt_want_write, which has a comment that
  // tells what marking inode for writeback means: This tells the low-level
  // filesystem that a write is about to be performed to  it, and makes sure
  // that writes are allowed (mount is read-write, filesystem  is not frozen)
  // before returning success.  When the write operation is finished,
  // mnt_drop_write() must be called.  This is effectively a refcount.

  // current->backing_dev_info = inode_to_bdi(inode); Rajan: commented this

	
	

	//Does write_begin, copying data from userspace to page in page cache, write_end. 
	//Writeback is done periodically number of pages dirty for current process are 
	//greater than certain limit.
	//
	//returns amount of bytes written on success
	//
	//scorw start
	//
	//ret = generic_perform_write(iocb->ki_filp, from, iocb->ki_pos);
	////Commentedprintk("[pid: %u] ext4_buffered_write_iter: calling generic_perform_write \n", current->pid);
	ret = scorw_generic_perform_write(iocb->ki_filp, from, iocb->ki_pos, write_to_par);
	//scorw end
	//current->backing_dev_info = NULL; Rajan: commented this

  // out:
  inode_unlock(inode);

#ifndef USE_OLD_RANGE
finalize_out:
#endif	
	//scorw start
	if(write_to_child)
	{	
		////Commentedprintk("ext4_buffered_write_iter: Writing to child file\n");
		//BUG_ON(uncopied_block == NULL);	//It can be null, such as during pure append operation
#ifdef USE_OLD_RANGE
    error = scorw_write_child_blocks_end(inode, offset, len, uncopied_block);
#else
    error = scorw_write_child_blocks_end(inode, offset, len, uncopied_block,
                                         shr.initialized);
#endif
    if (error) {
      return error;
    }
  }
  // scorw end
  if (likely(ret > 0)) {
    iocb->ki_pos += ret;

    // From comment above generic_write_sync
    /*
     * Sync the bytes written if this was a synchronous write.  Expect ki_pos
     * to already be updated for the write, and will return either the amount
     * of bytes passed in, or an error if syncing the file failed.
     */
    // Relevant statement is
    // if (iocb->ki_flags & IOCB_DSYNC)
    // Assuming this flag is not set.
    //

    ret = generic_write_sync(iocb, ret);
  }

  return ret;

out:
  // inode_unlock(inode);
  if (unlikely(ret <= 0))
    return ret;
  return generic_write_sync(iocb, ret);
}

static ssize_t ext4_handle_inode_extension(struct inode *inode, loff_t offset,
                                           ssize_t written, ssize_t count) {
  handle_t *handle;

  lockdep_assert_held_write(&inode->i_rwsem);
  handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
  if (IS_ERR(handle))
    return PTR_ERR(handle);

  if (ext4_update_inode_size(inode, offset + written)) {
    int ret = ext4_mark_inode_dirty(handle, inode);
    if (unlikely(ret)) {
      ext4_journal_stop(handle);
      return ret;
    }
  }

  if ((written == count) && inode->i_nlink)
    ext4_orphan_del(handle, inode);
  ext4_journal_stop(handle);

  return written;
}

/*
 * Clean up the inode after DIO or DAX extending write has completed and the
 * inode size has been updated using ext4_handle_inode_extension().
 */
static void ext4_inode_extension_cleanup(struct inode *inode, bool need_trunc) {
  lockdep_assert_held_write(&inode->i_rwsem);
  if (need_trunc) {
    ext4_truncate_failed_write(inode);
    /*
     * If the truncate operation failed early, then the inode may
     * still be on the orphan list. In that case, we need to try
     * remove the inode from the in-memory linked list.
     */
    if (inode->i_nlink)
      ext4_orphan_del(NULL, inode);
    return;
  }
  /*
   * If i_disksize got extended either due to writeback of delalloc
   * blocks or extending truncate while the DIO was running we could fail
   * to cleanup the orphan list in ext4_handle_inode_extension(). Do it
   * now.
   */
  if (!list_empty(&EXT4_I(inode)->i_orphan) && inode->i_nlink) {
    handle_t *handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);

    if (IS_ERR(handle)) {
      /*
       * The write has successfully completed. Not much to
       * do with the error here so just cleanup the orphan
       * list and hope for the best.
       */
      ext4_orphan_del(NULL, inode);
      return;
    }
    ext4_orphan_del(handle, inode);
    ext4_journal_stop(handle);
  }
}

static int ext4_dio_write_end_io(struct kiocb *iocb, ssize_t size, int error,
                                 unsigned int flags) {
  loff_t pos = iocb->ki_pos;
  struct inode *inode = file_inode(iocb->ki_filp);

  if (!error && size && flags & IOMAP_DIO_UNWRITTEN)
    error = ext4_convert_unwritten_extents(NULL, inode, pos, size);
  if (error)
    return error;
  /*
   * Note that EXT4_I(inode)->i_disksize can get extended up to
   * inode->i_size while the I/O was running due to writeback of delalloc
   * blocks. But the code in ext4_iomap_alloc() is careful to use
   * zeroed/unwritten extents if this is possible; thus we won't leave
   * uninitialized blocks in a file even if we didn't succeed in writing
   * as much as we intended. Also we can race with truncate or write
   * expanding the file so we have to be a bit careful here.
   */
  if (pos + size <= READ_ONCE(EXT4_I(inode)->i_disksize) &&
      pos + size <= i_size_read(inode))
    return size;
  return ext4_handle_inode_extension(inode, pos, size, size);
}

static const struct iomap_dio_ops ext4_dio_write_ops = {
    .end_io = ext4_dio_write_end_io,
};

/*
 * The intention here is to start with shared lock acquired then see if any
 * condition requires an exclusive inode lock. If yes, then we restart the
 * whole operation by releasing the shared lock and acquiring exclusive lock.
 *
 * - For unaligned_io we never take shared lock as it may cause data corruption
 *   when two unaligned IO tries to modify the same block e.g. while zeroing.
 *
 * - For extending writes case we don't take the shared lock, since it requires
 *   updating inode i_disksize and/or orphan handling with exclusive lock.
 *
 * - shared locking will only be true mostly with overwrites, including
 *   initialized blocks and unwritten blocks. For overwrite unwritten blocks
 *   we protect splitting extents by i_data_sem in ext4_inode_info, so we can
 *   also release exclusive i_rwsem lock.
 *
 * - Otherwise we will switch to exclusive i_rwsem lock.
 */
static ssize_t ext4_dio_write_checks(struct kiocb *iocb, struct iov_iter *from,
                                     bool *ilock_shared, bool *extend,
                                     bool *unwritten, int *dio_flags) {
  struct file *file = iocb->ki_filp;
  struct inode *inode = file_inode(file);
  loff_t offset;
  size_t count;
  ssize_t ret;
  bool overwrite, unaligned_io;

restart:
  ret = ext4_generic_write_checks(iocb, from);
  if (ret <= 0)
    goto out;

  offset = iocb->ki_pos;
  count = ret;

  unaligned_io = ext4_unaligned_io(inode, from, offset);
  *extend = ext4_extending_io(inode, offset, count);
  overwrite = ext4_overwrite_io(inode, offset, count, unwritten);

  /*
   * Determine whether we need to upgrade to an exclusive lock. This is
   * required to change security info in file_modified(), for extending
   * I/O, any form of non-overwrite I/O, and unaligned I/O to unwritten
   * extents (as partial block zeroing may be required).
   *
   * Note that unaligned writes are allowed under shared lock so long as
   * they are pure overwrites. Otherwise, concurrent unaligned writes risk
   * data corruption due to partial block zeroing in the dio layer, and so
   * the I/O must occur exclusively.
   */
  if (*ilock_shared && ((!IS_NOSEC(inode) || *extend || !overwrite ||
                         (unaligned_io && *unwritten)))) {
    if (iocb->ki_flags & IOCB_NOWAIT) {
      ret = -EAGAIN;
      goto out;
    }
    inode_unlock_shared(inode);
    *ilock_shared = false;
    inode_lock(inode);
    goto restart;
  }

  /*
   * Now that locking is settled, determine dio flags and exclusivity
   * requirements. We don't use DIO_OVERWRITE_ONLY because we enforce
   * behavior already. The inode lock is already held exclusive if the
   * write is non-overwrite or extending, so drain all outstanding dio and
   * set the force wait dio flag.
   */
  if (!*ilock_shared && (unaligned_io || *extend)) {
    if (iocb->ki_flags & IOCB_NOWAIT) {
      ret = -EAGAIN;
      goto out;
    }
    if (unaligned_io && (!overwrite || *unwritten))
      inode_dio_wait(inode);
    *dio_flags = IOMAP_DIO_FORCE_WAIT;
  }

  ret = file_modified(file);
  if (ret < 0)
    goto out;

  return count;
out:
  if (*ilock_shared)
    inode_unlock_shared(inode);
  else
    inode_unlock(inode);
  return ret;
}

static ssize_t ext4_dio_write_iter(struct kiocb *iocb, struct iov_iter *from) {
  ssize_t ret;
  handle_t *handle;
  struct inode *inode = file_inode(iocb->ki_filp);
  loff_t offset = iocb->ki_pos;
  size_t count = iov_iter_count(from);
  const struct iomap_ops *iomap_ops = &ext4_iomap_ops;
  bool extend = false, unwritten = false;
  bool ilock_shared = true;
  int dio_flags = 0;

  /*
   * Quick check here without any i_rwsem lock to see if it is extending
   * IO. A more reliable check is done in ext4_dio_write_checks() with
   * proper locking in place.
   */
  if (offset + count > i_size_read(inode))
    ilock_shared = false;

  if (iocb->ki_flags & IOCB_NOWAIT) {
    if (ilock_shared) {
      if (!inode_trylock_shared(inode))
        return -EAGAIN;
    } else {
      if (!inode_trylock(inode))
        return -EAGAIN;
    }
  } else {
    if (ilock_shared)
      inode_lock_shared(inode);
    else
      inode_lock(inode);
  }

  /* Fallback to buffered I/O if the inode does not support direct I/O. */
  if (!ext4_should_use_dio(iocb, from)) {
    if (ilock_shared)
      inode_unlock_shared(inode);
    else
      inode_unlock(inode);
    return ext4_buffered_write_iter(iocb, from);
  }

  /*
   * Prevent inline data from being created since we are going to allocate
   * blocks for DIO. We know the inode does not currently have inline data
   * because ext4_should_use_dio() checked for it, but we have to clear
   * the state flag before the write checks because a lock cycle could
   * introduce races with other writers.
   */
  ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);

  ret = ext4_dio_write_checks(iocb, from, &ilock_shared, &extend, &unwritten,
                              &dio_flags);
  if (ret <= 0)
    return ret;

  offset = iocb->ki_pos;
  count = ret;

  if (extend) {
    handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
    if (IS_ERR(handle)) {
      ret = PTR_ERR(handle);
      goto out;
    }

    ret = ext4_orphan_add(handle, inode);
    if (ret) {
      ext4_journal_stop(handle);
      goto out;
    }

    ext4_journal_stop(handle);
  }

  if (ilock_shared && !unwritten)
    iomap_ops = &ext4_iomap_overwrite_ops;
  ret = iomap_dio_rw(iocb, from, iomap_ops, &ext4_dio_write_ops, dio_flags,
                     NULL, 0);
  if (ret == -ENOTBLK)
    ret = 0;
  if (extend) {
    /*
     * We always perform extending DIO write synchronously so by
     * now the IO is completed and ext4_handle_inode_extension()
     * was called. Cleanup the inode in case of error or race with
     * writeback of delalloc blocks.
     */
    WARN_ON_ONCE(ret == -EIOCBQUEUED);
    ext4_inode_extension_cleanup(inode, ret < 0);
  }

out:
  if (ilock_shared)
    inode_unlock_shared(inode);
  else
    inode_unlock(inode);

  if (ret >= 0 && iov_iter_count(from)) {
    ssize_t err;
    loff_t endbyte;

    offset = iocb->ki_pos;
    err = ext4_buffered_write_iter(iocb, from);
    if (err < 0)
      return err;

    /*
     * We need to ensure that the pages within the page cache for
     * the range covered by this I/O are written to disk and
     * invalidated. This is in attempt to preserve the expected
     * direct I/O semantics in the case we fallback to buffered I/O
     * to complete off the I/O request.
     */
    ret += err;
    endbyte = offset + err - 1;
    err =
        filemap_write_and_wait_range(iocb->ki_filp->f_mapping, offset, endbyte);
    if (!err)
      invalidate_mapping_pages(iocb->ki_filp->f_mapping, offset >> PAGE_SHIFT,
                               endbyte >> PAGE_SHIFT);
  }

  return ret;
}

#ifdef CONFIG_FS_DAX
static ssize_t ext4_dax_write_iter(struct kiocb *iocb, struct iov_iter *from) {
  ssize_t ret;
  size_t count;
  loff_t offset;
  handle_t *handle;
  bool extend = false;
  struct inode *inode = file_inode(iocb->ki_filp);

  if (iocb->ki_flags & IOCB_NOWAIT) {
    if (!inode_trylock(inode))
      return -EAGAIN;
  } else {
    inode_lock(inode);
  }

  ret = ext4_write_checks(iocb, from);
  if (ret <= 0)
    goto out;

  offset = iocb->ki_pos;
  count = iov_iter_count(from);

  if (offset + count > EXT4_I(inode)->i_disksize) {
    handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
    if (IS_ERR(handle)) {
      ret = PTR_ERR(handle);
      goto out;
    }

    ret = ext4_orphan_add(handle, inode);
    if (ret) {
      ext4_journal_stop(handle);
      goto out;
    }

    extend = true;
    ext4_journal_stop(handle);
  }

  ret = dax_iomap_rw(iocb, from, &ext4_iomap_ops);

  if (extend) {
    ret = ext4_handle_inode_extension(inode, offset, ret, count);
    ext4_inode_extension_cleanup(inode, ret < (ssize_t)count);
  }
out:
  inode_unlock(inode);
  if (ret > 0)
    ret = generic_write_sync(iocb, ret);
  return ret;
}
#endif

ssize_t
ext4_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret;
	struct inode *inode = file_inode(iocb->ki_filp);
	//Commentedprintk("=============================Begin of Writes for i_ino=%ld==========================\n" , inode->i_ino);
	
	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;

#ifdef CONFIG_FS_DAX
  if (IS_DAX(inode)) {
    ret = ext4_dax_write_iter(iocb, from);
    return ret;
  }
#endif
	if (iocb->ki_flags & IOCB_DIRECT){
		ret = ext4_dio_write_iter(iocb, from);
	}
	else {
		ret = ext4_buffered_write_iter(iocb, from);
	}
	//Commentedprintk("============================End of Writes for i_ino=%ld=============================\n" , inode->i_ino);
	return ret;
}

#ifdef CONFIG_FS_DAX
static vm_fault_t ext4_dax_huge_fault(struct vm_fault *vmf,
                                      unsigned int order) {
  int error = 0;
  vm_fault_t result;
  int retries = 0;
  handle_t *handle = NULL;
  struct inode *inode = file_inode(vmf->vma->vm_file);
  struct super_block *sb = inode->i_sb;

  /*
   * We have to distinguish real writes from writes which will result in a
   * COW page; COW writes should *not* poke the journal (the file will not
   * be changed). Doing so would cause unintended failures when mounted
   * read-only.
   *
   * We check for VM_SHARED rather than vmf->cow_page since the latter is
   * unset for order != 0 (i.e. only in do_cow_fault); for
   * other sizes, dax_iomap_fault will handle splitting / fallback so that
   * we eventually come back with a COW page.
   */
  bool write =
      (vmf->flags & FAULT_FLAG_WRITE) && (vmf->vma->vm_flags & VM_SHARED);
  struct address_space *mapping = vmf->vma->vm_file->f_mapping;
  pfn_t pfn;

  if (write) {
    sb_start_pagefault(sb);
    file_update_time(vmf->vma->vm_file);
    filemap_invalidate_lock_shared(mapping);
  retry:
    handle = ext4_journal_start_sb(sb, EXT4_HT_WRITE_PAGE,
                                   EXT4_DATA_TRANS_BLOCKS(sb));
    if (IS_ERR(handle)) {
      filemap_invalidate_unlock_shared(mapping);
      sb_end_pagefault(sb);
      return VM_FAULT_SIGBUS;
    }
  } else {
    filemap_invalidate_lock_shared(mapping);
  }
  result = dax_iomap_fault(vmf, order, &pfn, &error, &ext4_iomap_ops);
  if (write) {
    ext4_journal_stop(handle);

    if ((result & VM_FAULT_ERROR) && error == -ENOSPC &&
        ext4_should_retry_alloc(sb, &retries))
      goto retry;
    /* Handling synchronous page fault? */
    if (result & VM_FAULT_NEEDDSYNC)
      result = dax_finish_sync_fault(vmf, order, pfn);
    filemap_invalidate_unlock_shared(mapping);
    sb_end_pagefault(sb);
  } else {
    filemap_invalidate_unlock_shared(mapping);
  }

  return result;
}

static vm_fault_t ext4_dax_fault(struct vm_fault *vmf) {
  return ext4_dax_huge_fault(vmf, 0);
}

static const struct vm_operations_struct ext4_dax_vm_ops = {
    .fault = ext4_dax_fault,
    .huge_fault = ext4_dax_huge_fault,
    .page_mkwrite = ext4_dax_fault,
    .pfn_mkwrite = ext4_dax_fault,
};
#else
#define ext4_dax_vm_ops ext4_file_vm_ops
#endif

static const struct vm_operations_struct ext4_file_vm_ops = {
    .fault = filemap_fault,
    .map_pages = filemap_map_pages,
    .page_mkwrite = ext4_page_mkwrite,
};

static int ext4_file_mmap(struct file *file, struct vm_area_struct *vma) {
  struct inode *inode = file->f_mapping->host;
  struct dax_device *dax_dev = EXT4_SB(inode->i_sb)->s_daxdev;

  if (unlikely(ext4_forced_shutdown(inode->i_sb)))
    return -EIO;

  /*
   * We don't support synchronous mappings for non-DAX files and
   * for DAX files if underneath dax_device is not synchronous.
   */
  if (!daxdev_mapping_supported(vma, dax_dev))
    return -EOPNOTSUPP;

  file_accessed(file);
  if (IS_DAX(file_inode(file))) {
    vma->vm_ops = &ext4_dax_vm_ops;
    vm_flags_set(vma, VM_HUGEPAGE);
  } else {
    vma->vm_ops = &ext4_file_vm_ops;
  }
  return 0;
}

static int ext4_sample_last_mounted(struct super_block *sb,
                                    struct vfsmount *mnt) {
  struct ext4_sb_info *sbi = EXT4_SB(sb);
  struct path path;
  char buf[64], *cp;
  handle_t *handle;
  int err;

  if (likely(ext4_test_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED)))
    return 0;

  if (sb_rdonly(sb) || !sb_start_intwrite_trylock(sb))
    return 0;

  ext4_set_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED);
  /*
   * Sample where the filesystem has been mounted and
   * store it in the superblock for sysadmin convenience
   * when trying to sort through large numbers of block
   * devices or filesystem images.
   */
  memset(buf, 0, sizeof(buf));
  path.mnt = mnt;
  path.dentry = mnt->mnt_root;
  cp = d_path(&path, buf, sizeof(buf));
  err = 0;
  if (IS_ERR(cp))
    goto out;

  handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
  err = PTR_ERR(handle);
  if (IS_ERR(handle))
    goto out;
  BUFFER_TRACE(sbi->s_sbh, "get_write_access");
  err = ext4_journal_get_write_access(handle, sb, sbi->s_sbh, EXT4_JTR_NONE);
  if (err)
    goto out_journal;
  lock_buffer(sbi->s_sbh);
  strtomem_pad(sbi->s_es->s_last_mounted, cp, 0);
  ext4_superblock_csum_set(sb);
  unlock_buffer(sbi->s_sbh);
  ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
out_journal:
  ext4_journal_stop(handle);
out:
  sb_end_intwrite(sb);
  return err;
}

// scorw start//
int scorw_dquot_file_open(struct inode *inode, struct file *file) {
  int error;

  error = generic_file_open(inode, file);
  // if (!error && (file->f_mode & FMODE_WRITE))
  if (!error)
    error = dquot_initialize(inode);
  return error;
}
// scorw end//

static int ext4_file_open(struct inode *inode, struct file *filp) {
  // scorw start//

  struct scorw_inode *p_scorw_inode = NULL;
  // struct scorw_inode *c_scorw_inode = NULL;
  int p_ino_num = -1;
  struct inode *p_inode = NULL;
  // scorw end//

  int ret;

  if (unlikely(ext4_forced_shutdown(inode->i_sb)))
    return -EIO;

  ret = ext4_sample_last_mounted(inode->i_sb, filp->f_path.mnt);
  if (ret)
    return ret;

  ret = fscrypt_file_open(inode, filp);
  if (ret)
    return ret;

  ret = fsverity_file_open(inode, filp);
  if (ret)
    return ret;

  /*
   * Set up the jbd2_inode if we are opening the inode for
   * writing and the journal is present
   */
  //	if (filp->f_mode & FMODE_WRITE) {
  ret = ext4_inode_attach_jinode(inode);
  if (ret < 0)
    return ret;
  //	}

  //////////// scorw start //////////

  // print info about inodes in scorw inodes list
  // scorw_print_inode_list();

        ////Commentedprintk("ext4_file_open: checking whether opened file (%lu) is a scorw file\n", inode->i_ino);

  if (scorw_is_child_file(inode, 1)) {
    // This lock makes sure that open(par),close(par) and creation of par scorw
    // inodes inside special_open() happens atomically
    mutex_lock(&(inode->i_vfs_inode_open_close_lock));
    atomic_add(1, &(inode->i_vfs_inode_open_count));

                ////Commentedprintk("ext4_file_open: Yes! opened file is a child file\n");

		///MAHA_AARSH_(testv_)
		p_ino_num = scorw_get_parent_attr_val(inode);
		p_inode = ext4_iget(inode->i_sb , p_ino_num , EXT4_IGET_NORMAL);
		if(IS_ERR_VALUE(p_inode)) {
			//Commentedprintk("[WIERD] :: stored parent_attr_val for c_ino_num=%lu is invalid\n" , inode->i_ino);
		}
		p_scorw_inode = scorw_get_inode(p_inode , 0 , 0);
		iput(p_inode);
		///(testv__)

                //c_scorw_inode = scorw_get_inode(inode, 1, 0); // XXX
		//Commentedprintk("Returned to file.c for i_ino=%lu\n" , inode->i_ino);
	
                mutex_unlock(&(inode->i_vfs_inode_open_close_lock));
        }
        else if(scorw_is_par_file(inode, 1))
        {
                //This lock makes sure that open(par),close(par) and creation of par scorw inodes inside special_open() happens atomically
                mutex_lock(&(inode->i_vfs_inode_open_close_lock));
                atomic_add(1, &(inode->i_vfs_inode_open_count));

                ////Commentedprintk("ext4_file_open: Yes! opened file is a parent file\n");
                p_scorw_inode = scorw_get_inode(inode, 0, 0);

    mutex_unlock(&(inode->i_vfs_inode_open_close_lock));
  }

  //////////// scorw end //////////


	filp->f_mode |= FMODE_NOWAIT;// | FMODE_CAN_ODIRECT; edit made
	//return dquot_file_open(inode, filp);
	ret = scorw_dquot_file_open(inode, filp);
	//Commentedprintk("[DEBUG] :: returning from %s\n" , __func__);
	return ret;
}

/*
 * ext4_llseek() handles both block-mapped and extent-mapped maxbytes values
 * by calling generic_file_llseek_size() with the appropriate maxbytes
 * value for each.
 */
loff_t ext4_llseek(struct file *file, loff_t offset, int whence) {
  struct inode *inode = file->f_mapping->host;
  loff_t maxbytes = ext4_get_maxbytes(inode);

  switch (whence) {
  default:
    return generic_file_llseek_size(file, offset, whence, maxbytes,
                                    i_size_read(inode));
  case SEEK_HOLE:
    inode_lock_shared(inode);
    offset = iomap_seek_hole(inode, offset, &ext4_iomap_report_ops);
    inode_unlock_shared(inode);
    break;
  case SEEK_DATA:
    inode_lock_shared(inode);
    offset = iomap_seek_data(inode, offset, &ext4_iomap_report_ops);
    inode_unlock_shared(inode);
    break;
  }

  if (offset < 0)
    return offset;
  return vfs_setpos(file, offset, maxbytes);
}

ssize_t scorw_generic_perform_write(struct file *file, struct iov_iter *i,
                                    loff_t pos, int write_to_par) {
  struct address_space *mapping = file->f_mapping;
  const struct address_space_operations *a_ops = mapping->a_ops;
  long status = 0;
  ssize_t written = 0;
  // unsigned int flags = 0; -unused var -Rajan removed
  int error = 0;
  // scorw start//
  struct inode *inode = mapping->host;
  int is_append_op =
      0; // optimization. Skip scorw operations for append to a parent file.
  unsigned blk_num = 0;
  unsigned last_block_eligible_for_copy = 0;
  struct uncopied_block *uncopied_block[SCORW_MAX_CHILDS] = {0};
  // scorw end//

  // HAMARA CODE START//

	// check if it is parent file, and it is in see thru ro mode
	// if yes, then call a spefic function to handle write, and return
	// if not, then continue
	struct scorw_inode* scorw_inode = inode->i_scorw_inode;
	/*if(scorw_inode == NULL){
		//Commentedprintk("scorw_generic_perform_write: inode->i_scorw_inode is NULL. This should not happen. Returning error\n");
		return -EIO;
	}*/

	
	// check if it is a parent file first
	if(scorw_inode && scorw_is_par_file(inode, 0))
	{
		//Commentedprintk("entered the is par inode check\n");
		// Now check if it is in see thru ro mode, by fetching ranges
		// loop over all child scorw_inodes and even if one has is_see_thru_ro set, then it is true
		/*		
		int i_1;
		int is_see_thru_ro = 0;
		for(i_1=0; i_1<=scorw_inode->i_last_child_index; i_1++)
		{
			if(scorw_inode->i_child_scorw_inode[i_1]->is_see_thru_ro)
			{
				//Commentedprintk("Child %d is see through\n", i_1);
				is_see_thru_ro = 1;
				break;
			}
		}
		*/
		if(scorw_inode->is_see_thru_ro){
			pos = scorw_write_see_thru_ro(file , i , pos);
			if(pos < 0 ) {
				return pos;
			}
			
		}
	}
	//HAMARA CODE END//


  do {
    struct page *page;
    unsigned long offset; /* Offset into pagecache page */
    unsigned long bytes;  /* Bytes to write to page */
    size_t copied;        /* Bytes copied from user */
    void *fsdata;
    is_append_op =
        0; // optimization. Skip scorw operations for append to a parent file.
    blk_num = 0;
    last_block_eligible_for_copy = 0;

    offset = (pos & (PAGE_SIZE - 1));
    bytes = min_t(unsigned long, PAGE_SIZE - offset, iov_iter_count(i));

  again:
    /*
     * Bring in the user page that we will copy from _first_.
     * Otherwise there's a nasty deadlock on copying from the
     * same page as we're writing to, without it being marked
     * up-to-date.
     *
     * Not only is this an optimisation, but it is also required
     * to check that the address is actually valid, when atomic
     * usercopies are used, below.
     */
    // if (unlikely(iov_iter_fault_in_readable(i, bytes))) {
    // 	status = -EFAULT;
    // 	break;
    // }. Removed Rajan

    if (fatal_signal_pending(current)) {
      status = -EINTR;
      break;
    }

		//////////////// scorw start ///////////////
		////Commentedprintk("scorw_generic_perform_write: Writing to a file. write_to_par: %d\n", write_to_par);
		if(write_to_par)
		{
			blk_num = (offset >> PAGE_SHIFT);
			last_block_eligible_for_copy  = ((inode->i_size-1) >> PAGE_SHIFT);

      // This write is purely append operation. Nothing to be done by us.
      if (blk_num > last_block_eligible_for_copy) {
        is_append_op = 1;
      } else {
        scorw_read_barrier_begin(scorw_find_inode(inode), blk_num,
                                 uncopied_block);
      }
    }
    //////////////// scorw end ///////////////

    struct folio *folio;
    status = a_ops->write_begin(file, mapping, pos, bytes, &folio,
                                &fsdata); // Rajan folio patch
    page = &(folio->page);
    if (unlikely(status < 0))
      break;

		//char* addr = (char*)kmap_atomic(page);
		////Commentedprintk("After write begin: %c%c%c%c%c\n", addr[0], addr[1], addr[2], addr[3], addr[4]);
		//kunmap_atomic((void *)addr);

		//////////////// scorw start ///////////////
		if(write_to_par && !is_append_op)
		{
			////Commentedprintk("[pid: %u] scorw_generic_perform_write: Writing to parent file. start offset: %lu, len: %d\n", current->pid, pos, bytes);
			error = scorw_write_par_blocks(inode, pos, bytes, page);
			BUG_ON(error);
		}
		//////////////// scorw end ///////////////
		//char* addr = (char*)kmap_atomic(page);
		////Commentedprintk("After scorw write par: %c%c%c%c%c\n", addr[0], addr[1], addr[2], addr[3], addr[4]);
		//kunmap_atomic((void *)addr);

    if (mapping_writably_mapped(mapping))
      flush_dcache_page(page);

    // copied = iov_iter_copy_from_user_atomic(page, i, offset, bytes); (patched
    // Rajan)
    copied = copy_page_from_iter_atomic(page, offset, bytes, i);
    flush_dcache_page(page);

		////Commentedprintk("After write iter: %c%c%c%c%c\n", addr[0], addr[1], addr[2], addr[3], addr[4]);
		//kunmap_atomic((void *)addr);
		
		status = a_ops->write_end(file, mapping, pos, bytes, copied,
						folio, fsdata); //Rajan folio patch
		//////////////// scorw start ///////////////
		if(write_to_par && !is_append_op)
		{
			scorw_read_barrier_end(scorw_find_inode(inode), blk_num, uncopied_block);
		}
		//////////////// scorw end ///////////////
		if (unlikely(status < 0))
			break;
		copied = status;

    cond_resched();

    // iov_iter_advance(i, copied);
    if (unlikely(copied == 0)) {
      /*
       * If we were unable to copy any data at all, we must
       * fall back to a single segment length write.
       *
       * If we didn't fallback here, we could livelock
       * because not all segments in the iov can be copied at
       * once without a pagefault.
       */
      bytes = min_t(unsigned long, PAGE_SIZE - offset,
                    iov_iter_single_seg_count(i));
      goto again;
    }
    pos += copied;
    written += copied;

    balance_dirty_pages_ratelimited(mapping);
  } while (iov_iter_count(i));

  return written ? written : status;
}
//// scorw end////

const struct file_operations ext4_file_operations = {
    .llseek = ext4_llseek,
    .read_iter = ext4_file_read_iter,
    .write_iter = ext4_file_write_iter,
    .iopoll = iocb_bio_iopoll,
    .unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = ext4_compat_ioctl,
#endif
    .mmap = ext4_file_mmap,
    .open = ext4_file_open,
    .release = ext4_release_file,
    .fsync = ext4_sync_file,
    .get_unmapped_area = thp_get_unmapped_area,
    .splice_read = ext4_file_splice_read,
    .splice_write = iter_file_splice_write,
    .fallocate = ext4_fallocate,
    .fop_flags = FOP_MMAP_SYNC | FOP_BUFFER_RASYNC | FOP_DIO_PARALLEL_WRITE,
};

const struct inode_operations ext4_file_inode_operations = {
    .setattr = ext4_setattr,
    .getattr = ext4_file_getattr,
    .listxattr = ext4_listxattr,
    .get_inode_acl = ext4_get_acl,
    .set_acl = ext4_set_acl,
    .fiemap = ext4_fiemap,
    .fileattr_get = ext4_fileattr_get,
    .fileattr_set = ext4_fileattr_set,
};
