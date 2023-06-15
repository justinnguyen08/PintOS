#include "devices/shutdown.h"
#include "devices/input.h"
#include "userprog/syscall.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "lib/kernel/list.h"
#include "filesys/inode.h"

static void syscall_handler (struct intr_frame *);

static int32_t get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);

/* Student helper functions */
bool valid_addr (const void *);
struct file_d *search_list(struct thread *, int fd);
void syscall_exit (int);
struct child_proc *search_child(int tid);

// lock is used for mutual exclusion for file system calls
struct lock file_lock;

void syscall_init (void)
{
  lock_init(&file_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void syscall_handler (struct intr_frame *f)
{
  int *my_esp;
  my_esp = f->esp;
  // for loop checks if address space of each parameter is valid
  for (int i = 0; i < 4; i++)
  { 
    if (!valid_addr(my_esp + i))
    {
      syscall_exit (-1);
    }
  }
  /* Justin driving */ 
  int exit_status;
  char *cmdline;
  char *filename;
  tid_t tid;
  unsigned initial_size;
  char *file; 
  int fd;
  void *buffer;    
  unsigned size;
  unsigned position;
  int retcode; 
  struct file_d *file_d;
  int syscall_number = *my_esp;
  int cur_fd; /* 0 == STDIN_FILENO, 1 == STDOUT_FILENO */
  int start_fd = 2;
  const char *dir;
  switch (syscall_number)
    {
    case SYS_HALT:
      shutdown_power_off();
      break;
    case SYS_EXIT:
    // dereferences the stack pointer to get the exit status
      exit_status = *(my_esp + 1);
      syscall_exit(exit_status);
      break;
    case SYS_EXEC:
    // checks if the command line is at a virtual address space
      cmdline = (char *) *(my_esp + 1);
      valid_addr(cmdline); 
      tid_t tid = process_execute(cmdline);
      lock_acquire(&file_lock);
      struct child_proc *cp = search_child(tid);
      sema_down(&cp->load);
      if (cp->loaded == -1)
      {
        list_remove(&cp->elem); 
        tid = -1; 
      }
      lock_release(&file_lock);
      f->eax = tid;
      break;
    case SYS_WAIT:
      tid = *(my_esp + 1);
      f->eax = process_wait(tid);
      break;
    /* Abhijit driving */
    case SYS_CREATE:
      filename = (char *) *(my_esp + 1);
      initial_size = *(my_esp + 2);
      valid_addr(filename); 
      lock_acquire(&file_lock);
      f->eax = filesys_create(filename, initial_size, false);
      lock_release(&file_lock);
      break;
    case SYS_REMOVE:
      filename = (char *) *(my_esp + 1);
      valid_addr(filename);
      lock_acquire(&file_lock);
      f->eax = filesys_remove(filename);
      lock_release(&file_lock);
      break;
    case SYS_OPEN:
      file = (char *) *(my_esp + 1);
      valid_addr(file);
      lock_acquire(&file_lock);
      struct file* file_opened;
      // allocate memory for the file descriptors
      struct file_d *fd = palloc_get_page(0);
      if (fd != NULL) 
      {
        // open the files
        file_opened = filesys_open(file);
        if (!file_opened) 
        {
          palloc_free_page (fd);
          fd->fid = -1;
        }
        else 
        {
          fd->file = file_opened; 
          struct list *fd_list = &thread_current()->fds;
          if (!list_empty(fd_list))
          {
            // access the back of the current list's file descriptors
            cur_fd = (list_entry(list_back(fd_list), struct file_d, elem)->fid)
               + 1;
            // sets current file's fid
            fd->fid = cur_fd;
          }
          else 
          {
            fd->fid = start_fd;
          }
          // pushes the most recent fd to the list of file descriptors
          list_push_back(fd_list, &(fd->elem));
        }
      }
      else 
      {
        fd->fid = -1;
      }
      lock_release(&file_lock);
      f->eax = fd->fid;
      break;
    case SYS_FILESIZE:
      fd = *(my_esp + 1);
      lock_acquire(&file_lock);
      file_d = search_list(thread_current(), fd);
      if(file_d == NULL) 
      {
        return -1;
      }
      retcode = file_length(file_d->file);
      lock_release(&file_lock);
      f->eax = retcode;
      break;
    case SYS_READ:
      fd = *(my_esp + 1);
      buffer = (void *) *(my_esp + 2);
      size = *(my_esp + 3);
      // checks if beginning of buffer is velow PHYSBASE
      valid_addr(buffer);
      // checks if end of buffer is below PHYSBASE
      valid_addr((char *) buffer + size);
      lock_acquire(&file_lock);
      char *my_buffer = (char *)buffer;
      retcode = -1;
      // standard input 
      if(fd == 0) 
      { 
        for(int i = 0; i < size; i++) 
        {
          // reaches input from keyboard using input_getc()
          if(!put_user(my_buffer + i, input_getc()) )
            syscall_exit(-1); 
        }
        retcode = size;
      }
      else 
      {
        file_d = search_list(thread_current(), fd);
        if (file_d && file_d->file) 
        {
          retcode = file_read(file_d->file, buffer, size);
        }
      }
      lock_release(&file_lock);
      f->eax = retcode;
      break;
    /* Pranav driving */
    case SYS_WRITE:
      fd = *(my_esp + 1);
      buffer = (void *) *(my_esp + 2);
      size = *(my_esp + 3);
      valid_addr(buffer);
      valid_addr((char *) buffer + size);
      lock_acquire(&file_lock);
      retcode = -1;
      if(fd == 1) 
      { 
        putbuf(buffer, size);
        retcode = size;
      }
      else 
      {
        file_d = search_list(thread_current(), fd);
        if (file_d && file_d->file) 
        {
          retcode = file_write(file_d->file, buffer, size);
        }
      }
      lock_release(&file_lock);
      f->eax = retcode;
      break;
    case SYS_SEEK:
      fd = *(my_esp + 1);
      position = *(my_esp + 2);
      lock_acquire(&file_lock);
      file_d = search_list(thread_current(), fd);
      if(file_d && file_d->file) 
      {
        file_seek(file_d->file, position);
      }
      lock_release(&file_lock);
      break;
    case SYS_TELL:
      fd = *(my_esp + 1);
      /* Justin driving */
      lock_acquire(&file_lock);
      file_d = search_list(thread_current(), fd);
      retcode = (file_d && file_d->file) ? file_tell(file_d->file) : -1;
      lock_release(&file_lock);
      f->eax = retcode;
      break;
    case SYS_CLOSE:
      fd = *(my_esp + 1);
      lock_acquire(&file_lock);
      file_d = search_list(thread_current(), fd);
      if (file_d != NULL && file_d->file) 
      {
        file_close(file_d->file);
        list_remove(&(file_d->elem));
        palloc_free_page(file_d);
      }
      lock_release(&file_lock);
      break;
    /* Justin driving */
    case SYS_CHDIR:
      dir = *(my_esp + 1);
      valid_addr(dir);
      lock_acquire(&file_lock);
      struct dir *new_dir = relative_path (dir);
      
      if (new_dir == NULL) 
      {
        retcode = false;
      }
      else 
      {
        dir_close (thread_current()->curr_dir);
        thread_current()->curr_dir = new_dir;
        retcode = true;
      }
      lock_release(&file_lock);
      f->eax = retcode;
      break;
    case SYS_MKDIR:
      dir = *(my_esp + 1);
      valid_addr(dir);
      lock_acquire(&file_lock);
      retcode = filesys_create(dir, 0, true);
      lock_release(&file_lock);
      f->eax = retcode; 
      break;
    /* Pranav driving */
    case SYS_READDIR:
      fd = *(my_esp + 1);
      filename = *(my_esp + 2);
      retcode = false;
      lock_acquire (&file_lock);
      struct file_d *file_d = search_list(thread_current(), fd);
      if (file_d && file_d->file) 
      {
        if (file_d->file->inode && file_d->file->inode->data.dir)
        {
          retcode = dir_readdir (file_d->dir, filename);
        }
      }
      lock_release (&file_lock);
      f->eax = retcode;
      break;
    /* Abhijit driving */
    case SYS_ISDIR:
      fd = *(my_esp + 1);
      lock_acquire(&file_lock);
      retcode = search_list(thread_current(), fd)->file->inode->data.dir;
      lock_release (&file_lock);
      f->eax = retcode;
      break;
    case SYS_INUMBER:
      fd = *(my_esp + 1);
      lock_acquire (&file_lock);
      retcode = search_list(thread_current(), fd)->file->inode->sector;
      lock_release (&file_lock);
      f->eax = retcode;
      break;
    default:
      syscall_exit(-1);
      break;
    }
    
}

/* Abhijit driving */
/* Student helper functions */
void syscall_exit(int status) 
{
  printf("%s: exit(%d)\n", thread_current()->name, status);
  struct child_proc *cp = thread_current()->cp;
  if(cp != NULL) 
  {
    sema_up(&cp->exit);
    cp->exit_status = status;
  }
  thread_exit();
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int32_t get_user (const uint8_t *uaddr) 
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a" (result) : "m" (*uaddr));
  return result;
}


/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool put_user (uint8_t *udst, uint8_t byte) 
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

struct file_d *search_list(struct thread *t, int fd)
{
  ASSERT (t != NULL);

  if (list_empty(&t->fds) || fd < 2) {
    return NULL;
  }

  struct list_elem *e;

    for(e = list_begin(&t->fds);
        e != list_end(&t->fds); e = list_next(e))
    {
      struct file_d *desc = list_entry(e, struct file_d, elem);
      if(desc->fid == fd) {
        return desc;
      }
    }

  return NULL;
}

/* Pranav driving */
struct child_proc *search_child(int tid)
{
  struct thread *t = thread_current();
  struct list_elem *e;

    for(e = list_begin(&t->children);
        e != list_end(&t->children); e = list_next(e))
    {
      struct child_proc *cp = list_entry(e, struct child_proc, elem);
      if(cp->tid == tid) {
        return cp;
      }
    }

  return NULL; 
}

bool valid_addr(const void *vaddr)
{
  if (!is_user_vaddr(vaddr))
  {
    syscall_exit(-1);
  }
  struct thread *cur = thread_current();
  void *usr_ptr = pagedir_get_page (cur->pagedir, vaddr);
  if (!usr_ptr)
  {
    syscall_exit(-1);
  }
  check_bytes((uint8_t *) vaddr);
  return usr_ptr;
}

void check_bytes(uint8_t *byte_ptr) 
{
  for (int i = 0; i < 4; i++) 
  {
    if (get_user(byte_ptr + i) == -1)
    {
      syscall_exit(-1);
    }
  }
}
