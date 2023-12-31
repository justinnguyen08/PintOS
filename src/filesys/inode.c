#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NUM_INDIRECT_PTRS 128
#define NUM_DBL_INDIRECT_PTRS 16384

struct indirect_block_ptr 
{
  block_sector_t block_ptrs[NUM_INDIRECT_PTRS];
};

static bool inode_alloc (struct inode_disk *disk_inode, off_t length);
static bool inode_dealloc (struct inode_disk *disk_inode);
static bool inode_alloc_iblock(block_sector_t *sector, 
                                 size_t sector_size, int height);
static void inode_dealloc_iblock(block_sector_t entry, 
                                   size_t sector_size, int height);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector (const struct inode *inode, off_t pos)
{
  int indirect_idx = NUM_DIRECT_BLOCKS + NUM_INDIRECT_PTRS;
  int double_indirect_idx = indirect_idx + NUM_DBL_INDIRECT_PTRS;
  ASSERT (inode != NULL);
  if (pos < inode->data.length) {
    // sector index
    pos = pos / BLOCK_SECTOR_SIZE;
    struct inode_disk *disk_inode = &inode->data;
    block_sector_t ret = -1;
    if (pos < NUM_DIRECT_BLOCKS) 
    {
      return disk_inode->direct_blocks[pos];
    }
    
    struct indirect_block_ptr *indirect_disk_inode = 
        malloc(sizeof(struct indirect_block_ptr));
    if (pos < indirect_idx) 
    {
      block_read (fs_device, disk_inode->indirect_block, indirect_disk_inode);
      ret = indirect_disk_inode->block_ptrs[pos - NUM_DIRECT_BLOCKS];
      free(indirect_disk_inode);
      return ret;
    }
    if (pos < double_indirect_idx) 
    {
      block_read (fs_device, disk_inode->doubly_indirect_block, 
        indirect_disk_inode);
      block_read (fs_device, indirect_disk_inode->block_ptrs
        [(pos - (indirect_idx)) / NUM_INDIRECT_PTRS], indirect_disk_inode);
      ret = indirect_disk_inode->block_ptrs
        [pos - (indirect_idx) % NUM_INDIRECT_PTRS];
      free(indirect_disk_inode);
      return ret;
    }
    free (indirect_disk_inode);
    return ret;
  }
  else
  {
    return -1;
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init (void) { list_init (&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->dir = is_dir;
      if (inode_alloc(disk_inode, disk_inode->length))
        {
          block_write (fs_device, sector, disk_inode);
          success = true;
        }
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk. (Does it?  Check code.)
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          free_map_release (inode->sector, 1);
          inode_dealloc (&inode->data);
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at (struct inode *inode, void *buffer_, off_t size,
                     off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                      off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  if (offset + size > inode_length(inode)) {
    if (inode_alloc (&inode->data, offset + size)) {
      inode->data.length = offset + size;
      block_write (fs_device, inode->sector, &inode->data);
    }
    else {
      return 0;
    }
  }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length (const struct inode *inode) { return inode->data.length; }

/* Student helper functions */
static bool inode_alloc(struct inode_disk *disk_inode, off_t length)
{
  /* Pranav driving */
  static char zeros[BLOCK_SECTOR_SIZE];
  size_t sector_size = bytes_to_sectors(length);
  int limit = sector_size < NUM_DIRECT_BLOCKS ? sector_size : NUM_DIRECT_BLOCKS;
  int index = 0;
  while (index < limit)
  {
    if (disk_inode->direct_blocks[index] == 0) 
    { 
      if (free_map_allocate(1, &disk_inode->direct_blocks[index]))
      {
        block_write (fs_device, disk_inode->direct_blocks[index], zeros);
      }
      else
      {
        return false;
      }
      sector_size--;
      if (sector_size <= 0) 
      { 
        return true;
      }
    }
    index++;
  }
  int indirect_limit = sector_size < NUM_INDIRECT_PTRS ? sector_size : 
    NUM_INDIRECT_PTRS;
  if (indirect_limit > 0)
  {
    if (!inode_alloc_iblock(&disk_inode->indirect_block, indirect_limit, 1))
    {
      return false;
    }
  }
  sector_size -= indirect_limit;
  if (sector_size <= 0) 
  {
    return true;
  }
  int dbl_indirect_limit = sector_size < NUM_DBL_INDIRECT_PTRS ? sector_size : 
    NUM_DBL_INDIRECT_PTRS;
  if (dbl_indirect_limit > 0)
  {
    if (!inode_alloc_iblock(&disk_inode->doubly_indirect_block, 
      dbl_indirect_limit, 2))
    {
      return false;
    }
  }
  sector_size -= dbl_indirect_limit;
  return sector_size <= 0;
}

static bool inode_dealloc(struct inode_disk *disk_inode)
{
  /* Justin driving */
  size_t sector_size = bytes_to_sectors(disk_inode->length);
  off_t file_length = disk_inode->length; 
  if (file_length < 0) 
  {
    return false;
  }
  int limit = sector_size < NUM_DIRECT_BLOCKS ? sector_size : NUM_DIRECT_BLOCKS;
  int index = 0;
  while (index < limit)
  {
    free_map_release (disk_inode->direct_blocks[index], 1);
    sector_size--;
    index++;
  }
  int indirect_limit = sector_size < NUM_INDIRECT_PTRS ? sector_size : 
    NUM_INDIRECT_PTRS;
  if(indirect_limit > 0) 
  {
    inode_dealloc_iblock (disk_inode->indirect_block, indirect_limit, 1);
  }
  sector_size -= indirect_limit;
  int dbl_indirect_limit = sector_size < NUM_DBL_INDIRECT_PTRS ? sector_size : 
    NUM_DBL_INDIRECT_PTRS;
  if(dbl_indirect_limit > 0) 
  {
    inode_dealloc_iblock (disk_inode->doubly_indirect_block, 
      dbl_indirect_limit, 2);
  }
  sector_size -= dbl_indirect_limit;
  return true;
}

static bool inode_alloc_iblock(block_sector_t *sector, size_t sector_size, 
                               int height)
{
  /* Abhijit driving */
  static char zeros[BLOCK_SECTOR_SIZE];
  struct indirect_block_ptr indirect_block;
  int unit = 1;
  while (height > 0) 
  {
    if (*sector == 0) 
    {
      free_map_allocate(1, sector);
      block_write(fs_device, *sector, zeros);
    }
    block_read(fs_device, *sector, &indirect_block);
    if (height == 2)
    {
      unit = NUM_INDIRECT_PTRS;
    }
    int limit = sector_size / unit;
    for (int index = 0; index < limit; index++) 
    {
      size_t size = sector_size < unit ? sector_size : unit;
      bool allocate_new = false;
      if (height == 1) 
      {
        if (indirect_block.block_ptrs[index] == 0) {
          allocate_new = true;
        }
      } 
      else 
      {
        if (inode_alloc_iblock(&indirect_block.block_ptrs[index], size, 
          height - 1)) 
        {
          block_write(fs_device, *sector, &indirect_block);
          sector_size -= size;
          continue;
        } 
        else 
        {
          return false;
        }
      }
      if (allocate_new) 
      {
        if (free_map_allocate(1, &indirect_block.block_ptrs[index])) 
        {
          block_write(fs_device, indirect_block.block_ptrs[index], zeros);
        }
        else
        {
          return false;
        }
      }
      block_write(fs_device, *sector, &indirect_block);
      sector_size -= size;
    }
    height--;
  }
  return true;
}

static void inode_dealloc_iblock(block_sector_t entry, size_t sector_size, 
                                 int height)
{
  /* Justin driving */
  struct indirect_block_ptr indirect_block;
  int unit = 1;
  if (height == 1) 
  {
    free_map_release (entry, 1);
    return;
  }
  while (height > 1) 
  {
    block_read(fs_device, entry, &indirect_block);
    if (height == 2) 
    {
      unit = NUM_INDIRECT_PTRS;
    }
    int limit = sector_size / unit;
    for (int index = 0; index < limit; index++) 
    {
      size_t size = sector_size < unit ? sector_size : unit;
      inode_dealloc_iblock (indirect_block.block_ptrs[index], size,
                            height - 1);
      sector_size -= size;
    }
    free_map_release (entry, 1);
    height--;
  }
  block_read(fs_device, entry, &indirect_block);
  int limit = sector_size / unit;
  for (int index = 0; index < limit; index++) 
  {
    free_map_release (indirect_block.block_ptrs[index], 1);
  }
  free_map_release (entry, 1);
}

