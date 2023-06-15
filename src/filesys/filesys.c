#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
char* separate_file_name(const char* path_name);
struct dir* absolute_path(const char* path_name);
struct dir *relative_path (const char *path);
bool curr_or_parent (const struct dir *dir, const char *name, struct inode 
**inode, struct dir_entry *e);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done (void) { free_map_close (); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create (const char *path, off_t initial_size, bool dir)
{
  /* Abhijit driving */
  block_sector_t inode_sector = 0;
  struct dir *directory = absolute_path(path);
  bool success = (directory != NULL && free_map_allocate (1, &inode_sector) && 
                  inode_create (inode_sector, initial_size, dir) && 
                  dir_add (directory, separate_file_name(path), inode_sector, 
                  dir));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (directory);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *filesys_open (const char *name)
{
  /* Justin driving */
  if (strcmp(name, "") == 0)
  {
    return NULL;
  }
  int l = strlen(name);
  struct dir *dir = absolute_path(name);
  char* file_name = separate_file_name(name);
  struct dir_entry e;
  if (dir == NULL) 
  {
    return NULL;
  }
  struct inode *inode = NULL;
  if (strcmp(file_name, "") != 0) 
  {
    if (!curr_or_parent(dir, file_name, inode, &e))
    {
      dir_lookup (dir, file_name, &inode);
    }
  }
  else 
  { 
    inode = dir_get_inode (dir);
  }
  dir_close (dir);
  if (!inode || inode->removed)
  {
    return NULL;
  }

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove (const char *name)
{
  /* Pranav driving */
  struct dir *dir = absolute_path(name);
  bool success = (dir != NULL && dir_remove (dir, separate_file_name(name)));
  dir_close (dir);

  return success;
}

/* Formats the file system. */
static void do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/* Student helper functions */
struct dir *absolute_path(const char* path_name)
{
  /* Pranav driving */
  int length = strlen(path_name) + 1;
  char path[length];
  struct dir_entry e;
  memcpy(path, path_name, length);
  bool root = !thread_current()->curr_dir || path[0] == '/';
  struct dir* dir = root ? dir_open_root() : 
    dir_reopen(thread_current()->curr_dir);
  char *save_ptr;
  char *prev = strtok_r(path, "/", &save_ptr);
  char *token = strtok_r(NULL, "/", &save_ptr);
  while (token != NULL)
  {
    struct inode *inode;
    bool close = true;
    if (prev == '.') 
    {
      prev = token;
      token = strtok_r(NULL, "/", &save_ptr);
      continue;
    }
    else if (!curr_or_parent(dir, prev, inode, &e) && 
      !dir_lookup(dir, prev, &inode))
    {
      goto fail;
    }
    if(inode->data.dir)
    {
      dir_close(dir);
      dir = dir_open(inode);
      close = false;
    }
    else 
    {
      if (!dir_open(inode)) {
        goto fail;
      }
      dir_close(dir);
      dir = dir_open(inode);
    }
    if (close) 
    {
      inode_close(inode);
    }
    prev = token;
    token = strtok_r(NULL, "/", &save_ptr);
  }
  return dir;

  fail:
  dir_close(dir);
  return NULL;
}

char *separate_file_name(const char* path_name)
{
  /* Abhijit driving */
  int length = strlen(path_name) + 1;  
  char *name = malloc(length);
  memcpy(name, path_name, length);
  char *save_ptr;
  char *token = strtok_r(name, "/", &save_ptr);
  char *prev = "";
  while (token != NULL)
  {
    length = strlen(token) + 1;
    prev = token;
    token = strtok_r(NULL, "/", &save_ptr);
  }
  name = malloc(length);
  memcpy(name, prev, length);
  return name;
}

struct dir *relative_path(const char *path)
{
  /* Justin driving */
  int length = strlen(path) + 1;
  char temp[length];
  struct dir_entry e;
  strlcpy(temp, path, length);
  bool root = (path[0] == '/' || thread_current()->curr_dir == NULL);
  struct dir *dir = root ? dir_open_root() : 
    dir_reopen(thread_current()->curr_dir);
  struct inode *inode = NULL;
  char *save_ptr;
  char *token = strtok_r(temp, "/", &save_ptr);
  while (token != NULL) 
  {
    if (!curr_or_parent(dir, path, inode, &e))
    {
      if (!dir_lookup(dir, token, &inode) || !dir_open(inode)) 
      {
        goto fail;
      }
    }
    dir = dir_open(inode);
    token = strtok_r(NULL, "/", &save_ptr);
  }
  if ((dir_get_inode(dir))->removed)
  {
    goto fail;
  }
  return dir;
  fail:
  dir_close(dir);
  return NULL;
}

bool curr_or_parent (const struct dir *dir, const char *name, struct inode 
  **inode, struct dir_entry *e)
{
  /* Pranav driving */
  if (strcmp (name, ".") == 0) 
  {
    *inode = inode_reopen (dir->inode);
    return true;
  }
  else if (strcmp (name, "..") == 0) 
  {
    inode_read_at (dir->inode, &e, sizeof e, 0);
    *inode = inode_open (e->inode_sector);
    return true;
  }
  return false;
}