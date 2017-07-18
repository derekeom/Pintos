#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "filesys/off_t.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "vm/frame.h"

#define STACK_BOUNDARY (PHYS_BASE - 0x800000)  // Max stack size of 4MB

enum page_type
{
  FILE = 2,
  MMAP = 1,
  ZERO = 0
};

struct file_page
{
  struct file *file;
  uint16_t offset;                /* Offset in PGSIZE (4096 byte) increments. */
  uint16_t read_bytes;            /* Number of bytes to read up to 4096 bytes. */
  bool writable;
};

struct mmap_page
{
  struct mmap_fd *mmap_fd;        /* Memory mapped file descriptor. */
  uint16_t offset;                /* File offset in PGSIZE (4096 byte) increments. */
  uint16_t read_bytes;            /* Number of bytes to read up to 4096 bytes. */
  struct list_elem elem;          /* List element for a memory mapped file descriptor. */
};

struct spte
{
  union
  {
    struct file_page file_page;   /* Supplemental file page info. */
    struct mmap_page mmap_page;   /* Supplemental mmap page info. */
  };

  void *upage;                    /* User virtual address. */
  struct fte *fte;                /* Frame table entry. */
  uint16_t swap_index;            /* Swap index. */
  uint8_t page_type;              /* Page type. */
  struct hash_elem elem;          /* Supplemental page table element. */
};

void page_add_zero (void *);
void page_add_zero_lazily (void *);
void page_add_file_lazily (void *, struct file *, off_t, off_t, bool);
mapid_t page_add_mmap_lazily (uint8_t *, struct file *, off_t);
bool page_load (void *);
void munmap_pages (mapid_t);
unsigned page_hash (const struct hash_elem *, void *);
bool page_less (const struct hash_elem *, const struct hash_elem *, void *);
void page_destructor (struct hash_elem *, void *);
struct spte *page_get_spte (void *);

#endif /* vm/page.h */
