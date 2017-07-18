#include "swap.h"
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define FRAME_SECTORS 8
#define SWAP_FREE false

static struct block *swap_block;
static struct bitmap *swap_table;

void
swap_init (void)
{
  ASSERT (FRAME_SECTORS * BLOCK_SECTOR_SIZE == PGSIZE);

  swap_block = block_get_role (BLOCK_SWAP);
  swap_table = bitmap_create (block_size (swap_block) / FRAME_SECTORS);
}

size_t
swap_out (uint8_t *kpage)
{
  size_t swap_index = bitmap_scan_and_flip (swap_table, 0, 1, SWAP_FREE);

  ASSERT (swap_index != (uint16_t) BITMAP_ERROR);

  block_sector_t sector = swap_index * FRAME_SECTORS;

  for (int offset = 0; offset < PGSIZE; offset += BLOCK_SECTOR_SIZE, sector++)
    block_write (swap_block, sector, kpage + offset);

  return swap_index;
}

void
swap_in (uint8_t *kpage, size_t swap_index)
{
  block_sector_t sector = swap_index * FRAME_SECTORS;

  for (int offset = 0; offset < PGSIZE; offset += BLOCK_SECTOR_SIZE, sector++)
    block_read (swap_block, sector, kpage + offset);

  bitmap_reset (swap_table, swap_index);
}

void
swap_free_index (size_t swap_index)
{
  bitmap_reset (swap_table, swap_index);
}

bool
swap_test_index (size_t swap_index)
{
  return bitmap_test (swap_table, swap_index);
}
