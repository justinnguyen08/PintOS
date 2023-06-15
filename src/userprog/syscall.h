#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "userprog/process.h"
#include "filesys/directory.h"

void syscall_init (void);

void syscall_exit (int);

/* File descriptor */
struct file_d
{
  int fid;
  struct list_elem elem;
  struct file* file;
  struct directory* dir;
};

#endif /* userprog/syscall.h */