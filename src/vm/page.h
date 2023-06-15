#include <hash.h>

struct hash page_table;

struct pt_entry
{
   void *upage; /* address of the page */
   void *kpage; /* address of the frame occupied by this page */
   struct hash_elem p_elem; /* represents this entry in the table */
   bool dirty; /* indicates whether page has been modified */
   bool accessed; /* indicates whether or not the page has been accessed */
   struct file *f; /* file associated with page table */
}

