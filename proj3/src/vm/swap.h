#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <bitmap.h>

void swap_init (void);
size_t swap_out (uint8_t *);
void swap_in (uint8_t *, size_t);
void swap_free_index (size_t);
bool swap_test_index (size_t);

#endif /* vm/swap.h */
