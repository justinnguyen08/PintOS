#include <stdio.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "lib/kernel/list.h"
#include "userprog/pagedir.h"
#include "threads/pte.h"
#include "vm/frame.h"

struct lock frame_lock; /* lock used for eviction and frame handling */
struct list frame_list; /* list used for page replacement algorithm */
struct list_elem *clock_hand; /* pointer used to handle page replacement */

unsigned frame_hash_func(struct hash_elem *elem, void *aux);
bool frame_less_func(struct hash_elem *a, struct hash_elem *b, void *aux);
void *alloc_frame(enum palloc_flags flags);

void init_ft() {
  list_init(&frame_list);
  lock_init(&frame_lock);
  clock_hand = NULL;
}

void *alloc_frame(enum palloc_flags flags) 
{
  void *kpage = NULL;
  if (flags & PAL_USER)
    {
      kpage = (flags & PAL_ZERO) ? palloc_get_page(PAL_USER | PAL_ZERO) : 
        palloc_get_page(PAL_USER);
    }
  // allocation failed, evict a page
  if (kpage == NULL) 
  {
    PANIC("allocation failed");
  }
  struct ft_entry *frame = malloc(sizeof(struct ft_entry));
  frame->kpage = kpage;
  frame->thr = thread_current();
  frame->occupied = true;
  frame->dirty = false;
  lock_acquire(&frame_lock);
  list_push_back(&frame_list, &frame->elem);
  lock_release(&frame_lock);
  return kpage;
}

void dealloc_frame(void *kpage, bool free_page) 
{
  struct ft_entry *curr_frame = find_frame(kpage);
  if (curr_frame != NULL)
  {
    lock_acquire(&frame_lock);
    list_remove(&curr_frame->elem);
    lock_release(&frame_lock);
    free(curr_frame);
    if (free_page)
    {
      palloc_free_page(kpage);  
    }
  }
}

struct ft_entry *find_frame(void *kpage) 
{
  struct ft_entry *curr_frame = NULL;
  for (struct list_elem *e = list_begin(&frame_list); e != list_end(&frame_list)
  ; e = list_next(e))
  {
    struct ft_entry *temp_frame = list_entry(e, struct ft_entry, elem);
    if (temp_frame->kpage == kpage)
    {
      curr_frame = temp_frame;
      break;
    }
  }
  return curr_frame;
}