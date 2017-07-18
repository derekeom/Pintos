#include "vm/page.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/swap.h"

#define NOT_IN_SWAP_PARTITION 0xFFFF

static struct spte *create_spte (void *, uint8_t);
static void load_zero_page (struct spte *);
static void load_file_page (struct spte *);
static void load_swap_page (struct spte *);
static void load_mmap_page (struct spte *);

static struct spte *
create_spte (void *uaddr, uint8_t page_type)
{
  ASSERT (uaddr && is_user_vaddr (uaddr));
  ASSERT (uaddr >= USER_VADDR_BOTTOM);

  struct spte *spte = malloc (sizeof (struct spte));
  spte->upage = pg_round_down (uaddr);
  spte->swap_index = NOT_IN_SWAP_PARTITION;
  spte->page_type = page_type;
  spte->fte = NULL;
  return spte;
}

void page_add_zero (void *upage)
{
  page_add_zero_lazily (upage);
  page_load (upage);
}

void page_add_zero_lazily (void *upage)
{
  ASSERT (upage && is_user_vaddr (upage));
  ASSERT (!page_get_spte (upage));

  struct spte *spte = create_spte ((upage), ZERO);

  ASSERT (!hash_insert (&thread_current ()->sup_page_table, &spte->elem));
}

void page_add_file_lazily (void *upage, struct file *file, off_t read_bytes, off_t offset, bool writable)
{
  ASSERT (upage && is_user_vaddr (upage));
  ASSERT (file);
  ASSERT (!pg_ofs ((void *) offset));
  ASSERT (offset < (0xFFFF << PGBITS));
  ASSERT (!page_get_spte (upage));

  struct spte *spte = create_spte ((upage), FILE);
  spte->file_page.file = file;
  spte->file_page.read_bytes = read_bytes;
  spte->file_page.offset = offset >> PGBITS;
  spte->file_page.writable = writable;

  ASSERT (!hash_insert (&thread_current ()->sup_page_table, &spte->elem));
}

mapid_t
page_add_mmap_lazily (uint8_t *upage, struct file *file, off_t read_bytes)
{
  ASSERT (upage && is_user_vaddr (upage) && !pg_ofs (upage));
  ASSERT (file);
  ASSERT (read_bytes < (0xFFFF << PGBITS));

  /* Verify user virtual address isn't already mapped. */
  for (uint8_t *pg = upage; pg < upage + read_bytes; pg += PGSIZE)
    if (page_get_spte (pg))
      return MAP_FAILED;

  /* Create mmap file descriptor for user process. */
  struct mmap_fd *mmap_fd = malloc (sizeof (struct mmap_fd));
  list_init (&mmap_fd->spte_list);

  /* Add the mmap file descriptor to thread's mmap file descriptor list. */
  struct list *mmap_list = &thread_current ()->mmap_list;
  if (list_empty (mmap_list))
    mmap_fd->mapid = 0;
  else
    mmap_fd->mapid = list_entry (list_back (mmap_list), struct mmap_fd, elem)->mapid + 1;
  list_push_back (mmap_list, &mmap_fd->elem);

  /* Map pages to the supplemental page table. */
  off_t last_page_offset = read_bytes & ~PGMASK;
  for (off_t offset = 0; offset < read_bytes; offset += PGSIZE)
  {
    struct spte *spte = create_spte (upage + offset, MMAP);
    spte->mmap_page.mmap_fd = mmap_fd;
    spte->mmap_page.offset = offset >> PGBITS;
    bool last_page = offset == last_page_offset;
    spte->mmap_page.read_bytes = last_page ? (read_bytes & PGMASK) : PGSIZE;
    list_push_back (&mmap_fd->spte_list, &spte->mmap_page.elem);

    /* Release resources on failure. */
    if (hash_insert (&thread_current ()->sup_page_table, &spte->elem))
    {
      munmap_pages (mmap_fd->mapid);
      return MAP_FAILED;
    }
  }

  mmap_fd->file = file;
  return mmap_fd->mapid;
}

bool
page_load (void *uaddr)
{
  struct spte *spte = page_get_spte (uaddr);

  if (!spte)
    return false;

  if (spte->swap_index != NOT_IN_SWAP_PARTITION)
  {
    load_swap_page (spte);
    return true;
  }
  
  switch (spte->page_type)
  {
    case (ZERO): load_zero_page (spte); break;
    case (FILE): load_file_page (spte); break;
    case (MMAP): load_mmap_page (spte); break;
    default:     PANIC ("Unknown page type!");
  }

  return true;
}

static void
load_zero_page (struct spte *spte)
{
  ASSERT (spte && spte->page_type == ZERO);

  frame_alloc (spte, PAL_USER | PAL_ZERO, true);
  frame_unpin_addr (spte->upage);
}

static void
load_file_page (struct spte *spte)
{
  ASSERT (!lock_held_by_current_thread (&fs_lock));

  void *kpage = frame_alloc (spte, PAL_USER, spte->file_page.writable)->kpage;

  struct file *file = spte->file_page.file;
  off_t offset = (off_t) spte->file_page.offset << PGBITS;
  off_t read_bytes = (off_t) spte->file_page.read_bytes;
  off_t zero_bytes = PGSIZE - read_bytes;

  /* Load the file to page. */
  lock_acquire (&fs_lock);
  ASSERT (file_read_at (file, kpage, read_bytes, offset) == read_bytes);
  lock_release (&fs_lock);
  memset (kpage + read_bytes, 0, zero_bytes);
}

static void
load_swap_page (struct spte *spte)
{
  ASSERT (swap_test_index (spte->swap_index));

  bool writable = (spte->page_type == FILE) ? spte->file_page.writable : true;
  struct fte *fte = frame_alloc (spte, PAL_USER, writable);
  swap_in (fte->kpage, spte->swap_index);
  spte->swap_index = NOT_IN_SWAP_PARTITION;
}

static void
load_mmap_page (struct spte *spte)
{
  ASSERT (!lock_held_by_current_thread (&fs_lock));

  void *kpage = frame_alloc (spte, PAL_USER, true)->kpage;

  struct file *file = spte->mmap_page.mmap_fd->file;
  off_t offset = (off_t) spte->mmap_page.offset << PGBITS;
  off_t read_bytes = (off_t) spte->mmap_page.read_bytes;
  off_t zero_bytes = PGSIZE - read_bytes;

  /* Load the file to page. */
  lock_acquire (&fs_lock);
  ASSERT (file_read_at (file, kpage, read_bytes, offset) == read_bytes);
  lock_release (&fs_lock);
  memset (kpage + read_bytes, 0, zero_bytes);
}

void
munmap_pages (mapid_t mapid)
{
  struct thread *t = thread_current ();

  /* Get mmap file descriptor. */
  struct mmap_fd *mmap_fd = NULL;
  for (struct list_elem *e = list_begin (&t->mmap_list);
       e != list_end (&t->mmap_list); e = list_next (e))
  {
    struct mmap_fd *mfd = list_entry (e, struct mmap_fd, elem);
    if (mfd->mapid == mapid)
    {
      mmap_fd = mfd;
      break;
    }
  }

  if (!mmap_fd)
    return;

  /* Free all pages mapped to file. */
  while (!list_empty (&mmap_fd->spte_list))
  {
    struct list_elem *e = list_pop_front (&mmap_fd->spte_list);
    struct spte *spte = list_entry (e, struct spte, mmap_page.elem);
    if (spte->fte)
      frame_free (spte->fte);
    hash_delete (&t->sup_page_table, &spte->elem);
    free (spte);
  }

  /* Free mmap file descriptor. */
  list_remove (&mmap_fd->elem);
  free (mmap_fd);
}

void
page_destructor (struct hash_elem *e, void *aux UNUSED)
{
  struct spte *spte = hash_entry (e, struct spte, elem);

  if (spte->fte)
    frame_free (spte->fte);
  else if (spte->swap_index != NOT_IN_SWAP_PARTITION)
    swap_free_index (spte->swap_index);

  free (spte);
}

struct spte *
page_get_spte (void *uaddr)
{
  ASSERT (is_user_vaddr (uaddr));

  struct thread *t = thread_current ();
  struct spte key;
  key.upage = pg_round_down (uaddr);
  struct hash_elem *found = hash_find (&t->sup_page_table, &key.elem);
  if (found)
  {
    struct spte *spte = hash_entry (found, struct spte, elem);

    /* Assert that fte exists iff pagedir has page. */
    // ASSERT (!spte->fte == !pagedir_get_page (t->pagedir, uaddr));
    
    return spte;
  }

  return NULL;
}

/* Returns a hash value for spte. */
unsigned
page_hash (const struct hash_elem *elem, void *aux UNUSED)
{
  const struct spte *spte = hash_entry (elem, struct spte, elem);
  return hash_bytes (&spte->upage, sizeof (spte->upage));
}

/* Returns true if spte a precedes spte b. */
bool
page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  const struct spte *spte_a = hash_entry (a, struct spte, elem);
  const struct spte *spte_b = hash_entry (b, struct spte, elem);
  return spte_a->upage < spte_b->upage;
}
