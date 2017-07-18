#include "vm/frame.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"

static struct list frame_table;
static struct lock ft_lock;

static void evict (void);
static void write_back (struct spte *);

void
frame_init (void)
{
  list_init (&frame_table);
  lock_init (&ft_lock);
}

struct fte *
frame_alloc (struct spte *spte, enum palloc_flags flags, bool writable)
{
  ASSERT (spte && spte->upage && is_user_vaddr (spte->upage));
  ASSERT (flags & PAL_USER);
  ASSERT (!lock_held_by_current_thread (&ft_lock));

  /* Obtain a free frame. */
  uint8_t *kpage = palloc_get_page (flags);
  while (!kpage)
  {
    evict ();
    kpage = palloc_get_page (flags);
  }

  /* Initialize frame table entry. */
  struct fte *fte = malloc (sizeof (struct fte));
  fte->spte = spte;
  fte->kpage = kpage;
  fte->owner = thread_current ();
  fte->pinned = true;

  /* Add frame to frame table. */
  lock_acquire (&ft_lock);
  list_push_back (&frame_table, &fte->elem);
  lock_release (&ft_lock);

  /* Install page to frame. */
  spte->fte = fte;
  pagedir_set_page (fte->owner->pagedir, fte->spte->upage, fte->kpage, writable);
  
  return fte;
}

static void
evict ()
{
  ASSERT (!list_empty (&frame_table));
  ASSERT (!lock_held_by_current_thread (&ft_lock));

  struct fte *fte;
  struct spte *spte;
  uint32_t *pagedir;
  void *upage;

  lock_acquire(&ft_lock);
  while (true)
  {
    /* Pop the head of the list. */
    fte = list_entry (list_pop_front (&frame_table), struct fte, elem);
    spte = fte->spte;
    pagedir = fte->owner->pagedir;
    upage = spte->upage;

    ASSERT (spte && pagedir && upage);
    ASSERT (pagedir_get_page (pagedir, upage));

    /* Skip pinned frames. */
    if (fte->pinned)
      goto second_chance;

    /* Evict page if it hasn't been recently accessed. */
    if (!pagedir_is_accessed (pagedir, upage))
      break;

    /* If page was modified, write back
     * to disk and clear dirty bit. */
    if (pagedir_is_dirty (pagedir, upage))
    {
      write_back (spte);
      pagedir_set_dirty (pagedir, upage, false);
    }
    else
    {
      /* Else clear accessed bit. */
      pagedir_set_accessed (pagedir, upage, false);
    }
    
  second_chance:
    /* Insert to the back of the list for a second chance. */
    list_push_back (&frame_table, &fte->elem);
  }

  /* Swap out to swap partition. */
  spte->swap_index = swap_out (fte->kpage);

  /* Invalidate page and free frame. */
  pagedir_clear_page (fte->owner->pagedir, upage);
  spte->fte = NULL;
  palloc_free_page (fte->kpage);
  lock_release (&ft_lock);

  /* Deallocate frame table entry. */
  free (fte);
}

void
frame_free (struct fte *fte)
{
  ASSERT (fte);
  ASSERT (!lock_held_by_current_thread (&ft_lock));

  struct spte *spte = fte->spte;

  /* Remove from frame table. */
  lock_acquire (&ft_lock);
  list_remove (&fte->elem);
  lock_release (&ft_lock);

  /* Write back on dirty. */
  if (pagedir_is_dirty (thread_current ()->pagedir, spte->upage))
    write_back (spte);

  /* Invalidate page and free frame. */
  pagedir_clear_page (thread_current ()->pagedir, spte->upage);
  palloc_free_page (fte->kpage);

  /* Deallocate frame table entry. */
  spte->fte = NULL;
  free (fte);
}

static void
write_back (struct spte *spte)
{
  ASSERT (!lock_held_by_current_thread (&fs_lock));

  struct file *file;
  off_t offset;

  switch (spte->page_type)
  {
    case FILE:  file = spte->file_page.file;
                offset = (off_t) spte->file_page.offset << PGBITS; break;
    case MMAP:  file = spte->mmap_page.mmap_fd->file;
                offset = (off_t) spte->mmap_page.offset << PGBITS; break;
    default:    return;
  }

  lock_acquire (&fs_lock);
  file_write_at (file, spte->fte->kpage, PGSIZE, offset);
  lock_release (&fs_lock);
}

void
frame_pin_addr (void *uaddr)
{
  struct spte *spte = page_get_spte (uaddr);

  ASSERT (spte);

  if (!spte->fte)
    page_load ((void *) spte->upage);
  spte->fte->pinned = true;
}

void
frame_unpin_addr (void *uaddr)
{
  struct spte *spte = page_get_spte (uaddr);

  ASSERT (spte && spte->fte);

  spte->fte->pinned = false;
}

void
frame_pin_string (const char *file)
{
  for (void *uaddr = (void *) file;
    uaddr <= (void *) file + strlen (file);
    uaddr += PGSIZE)
    frame_pin_addr (uaddr);
}

void
frame_unpin_string (const char *file)
{
  for (void *uaddr = (void *) file;
    uaddr <= (void *) file + strlen (file);
    uaddr += PGSIZE)
    frame_unpin_addr (uaddr);
}

void
frame_pin_buffer (void *buffer, unsigned size)
{
  for (void *uaddr = pg_round_down (buffer);
    uaddr < buffer + size;
    uaddr += PGSIZE)
    frame_pin_addr (uaddr);
}

void
frame_unpin_buffer (void *buffer, unsigned size)
{
  for (void *uaddr = pg_round_down (buffer);
    uaddr < buffer + size;
    uaddr += PGSIZE)
    frame_unpin_addr (uaddr);
}
