#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/palloc.h"

struct fte
{
  bool pinned;              /* If pinned, don't evict. */
  uint8_t *kpage;           /* Kernel virtual address mapped to frame. */
  struct spte *spte;        /* Supplementary PTE mapped to virtual page. */
  struct thread *owner;     /* The thread that owns the page. */
  struct list_elem elem;    /* Frame table list element. */
};

void frame_init (void);
struct fte *frame_alloc (struct spte *, enum palloc_flags, bool);
void frame_free (struct fte *);
void frame_pin_addr (void *);
void frame_unpin_addr (void *);
void frame_pin_string (const char *);
void frame_unpin_string (const char *);
void frame_pin_buffer (void *, unsigned);
void frame_unpin_buffer (void *, unsigned);

#endif /* vm/frame.h */
