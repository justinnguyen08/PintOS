#include <hash.h>

struct ft_entry
{
  void *upage; /* page currently occupying this frame */
  void *kpage; /* this frame */
  struct list_elem elem; /* element of frame_list */
  bool occupied; /* indicates whether or not page is occupied */
  struct thread *thr; /* thread associated with the frame table */
  bool dirty; /* indicates whether or not page was written to */
};

void *alloc_frame(enum palloc_flags flags);
void init_frame();
void dealloc_frame(void *kpage, bool free_page);
struct ft_entry *find_frame(void *kpage);
