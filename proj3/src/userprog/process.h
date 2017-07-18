#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "filesys/off_t.h"
#include "threads/thread.h"

#define FILENO_START 2

/* Map region identifier. */
typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

struct file_descriptor
{
  int fileno;
  struct file *file;
  struct list_elem elem;
};

struct mmap_fd
{
  mapid_t mapid;
  struct file *file;
  struct list spte_list;           /* List of pages memory mapped to the file. */
  struct list_elem elem;
};

tid_t process_execute (const char *cmdline);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
