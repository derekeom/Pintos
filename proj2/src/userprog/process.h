#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

#define FILENO_START 2

/* Process identifier. */
typedef int pid_t;

struct file_descriptor
{
  int fileno;
  struct file *file;
  struct list_elem elem;
};

tid_t process_execute (const char *cmdline);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
