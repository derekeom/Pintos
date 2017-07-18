#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "vm/frame.h"
#include "vm/page.h"

static void syscall_handler (struct intr_frame *);
static void halt (void);
static pid_t exec (const char *file);
static int wait (pid_t pid);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static int filesize (int fd); 
static int read (int fd, void *buffer, unsigned size);
static int write (int fd, void *buffer, unsigned size);
static void seek (int fd, unsigned position);
static void close (int fd);
static mapid_t mmap (int fd, void *addr);
static void munmap (mapid_t mapid);
static void kill_on_bad_uaddr (void *uaddr);
static struct file_descriptor *get_fildes (int fileno);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  ASSERT (!lock_held_by_current_thread (&fs_lock));
  unsigned *sp = (unsigned *)f->esp;
  
  kill_on_bad_uaddr (sp);

  unsigned sys_code = *sp;
  unsigned arg0 = (unsigned)*(sp + 1);
  unsigned arg1 = (unsigned)*(sp + 2);
  unsigned arg2 = (unsigned)*(sp + 3);

  switch (sys_code)
  {
    case SYS_HALT:      halt (); break;
    case SYS_EXIT:      kill_on_bad_uaddr (sp + 1); syscall_exit (arg0); break;
    case SYS_EXEC:      kill_on_bad_uaddr (sp + 1); f->eax = exec ((char *)arg0); break;
    case SYS_WAIT:      kill_on_bad_uaddr (sp + 1); f->eax = wait (arg0); break;
    case SYS_CREATE:    kill_on_bad_uaddr (sp + 2); f->eax = create ((char *)arg0, arg1); break;
    case SYS_REMOVE:    kill_on_bad_uaddr (sp + 1); f->eax = remove ((char *)arg0); break;
    case SYS_OPEN:      kill_on_bad_uaddr (sp + 1); f->eax = open ((char *)arg0); break;
    case SYS_FILESIZE:  kill_on_bad_uaddr (sp + 1); f->eax = filesize (arg0); break;
    case SYS_READ:      kill_on_bad_uaddr (sp + 3); f->eax = read (arg0, (void *)arg1, arg2); break;
    case SYS_WRITE:     kill_on_bad_uaddr (sp + 3); f->eax = write (arg0, (void *)arg1, arg2); break;
    case SYS_SEEK:      kill_on_bad_uaddr (sp + 2); seek (arg0, arg1); break;
    case SYS_CLOSE:     kill_on_bad_uaddr (sp + 1); close (arg0); break;
    case SYS_MMAP:      kill_on_bad_uaddr (sp + 2); f->eax = mmap (arg0, (void *)arg1); break;
    case SYS_MUNMAP:    kill_on_bad_uaddr (sp + 1); munmap (arg0); break;
    default:            printf("syscall.c: Unknown syscall code.\n"); thread_exit (); break;
  }
}

static void
halt (void)
{
  shutdown_power_off ();
}

void
syscall_exit (int status)
{
  thread_current ()->exit_status = status;
  printf ("%s: exit(%d)\n", thread_current ()->name, status);
  thread_exit ();
}

static pid_t
exec (const char *file)
{
  kill_on_bad_uaddr ((void *) file);
  
  frame_pin_string (file);
  pid_t tid = process_execute (file);
  frame_unpin_string (file);

  bool load_success = false;
  struct list *child_list = &thread_current ()->child_list;
  for (struct list_elem *e = list_begin (child_list);
       e != list_end (child_list); e = list_next (e))
  {
    struct thread *child = list_entry (e, struct thread, child_elem);
    if (child->tid == tid)
    {
      sema_down (&child->loaded);
      load_success = child->load_status;
      break;
    }
  }

  if (load_success)
    return tid;

  return PID_ERROR;
}

static int
wait (pid_t pid)
{
  return process_wait (pid);
}

static bool
create (const char *file, unsigned initial_size)
{
  kill_on_bad_uaddr ((void *) file);

  frame_pin_string (file);
  lock_acquire (&fs_lock);
  bool success = filesys_create (file, initial_size);
  lock_release (&fs_lock);
  frame_unpin_string (file);

  return success;
}

static bool
remove (const char *file)
{
  kill_on_bad_uaddr ((void *) file);

  frame_pin_string (file);
  lock_acquire (&fs_lock);
  bool success = filesys_remove (file);
  lock_release (&fs_lock);
  frame_unpin_string (file);

  return success;
}

static int
open (const char *file)
{
  kill_on_bad_uaddr ((void *) file);

  frame_pin_string (file);
  lock_acquire (&fs_lock);
  struct file *f = filesys_open ((void *)file);
  lock_release (&fs_lock);
  frame_unpin_string (file);

  if (!f)
    return ERROR;

  struct list *fd_list = &thread_current ()->fd_list;
  struct file_descriptor *fd = malloc (sizeof (struct file_descriptor));
  fd->file = f;
  if (list_empty (fd_list))
    fd->fileno = FILENO_START;
  else
    fd->fileno = list_entry (list_back (fd_list), struct file_descriptor, elem)->fileno + 1;
  list_push_back (fd_list, &fd->elem);
  return fd->fileno;
}

static int
filesize (int fd)
{
  struct file_descriptor *fildes = get_fildes(fd);
  if (!fildes)
    return ERROR;

  lock_acquire (&fs_lock);
  int len = file_length (fildes->file);
  lock_release (&fs_lock);
  return len;
}

static int
read (int fd, void *buffer, unsigned size)
{
  kill_on_bad_uaddr ((void *) buffer);

  struct file_descriptor *fildes = get_fildes(fd);
  if (!fildes)
    return ERROR;

  /* Disallow writing to the code segment. */
  struct spte *spte = page_get_spte (buffer);
  if (spte && spte->page_type == FILE && !spte->file_page.writable)
    syscall_exit (ERROR);

  frame_pin_buffer (buffer, size);
  lock_acquire (&fs_lock);
  int read = file_read (fildes->file, buffer, size);
  lock_release (&fs_lock);
  frame_unpin_buffer (buffer, size);

  return read;
}

static int
write (int fd, void *buffer, unsigned size)
{
  kill_on_bad_uaddr ((void *) buffer);

  if (fd == STDOUT_FILENO)
  {
    frame_pin_buffer (buffer, size);
    putbuf (buffer, size);
    frame_unpin_buffer (buffer, size);
    return size;
  }

  struct file_descriptor *fildes = get_fildes(fd);
  if (!fildes)
    syscall_exit (ERROR);

  frame_pin_buffer (buffer, size);
  lock_acquire (&fs_lock);
  int write = file_write (fildes->file, buffer, size);
  lock_release (&fs_lock);
  frame_unpin_buffer (buffer, size);

  return write;
}

static void
seek (int fd, unsigned position)
{
  struct file_descriptor *fildes = get_fildes(fd);
  if (fildes)
  {
    lock_acquire (&fs_lock);
    file_seek (fildes->file, position);
    lock_release (&fs_lock);
  }
}

static void
close (int fd)
{
  struct file_descriptor *fildes = get_fildes(fd);
  if (fildes)
  {
    lock_acquire (&fs_lock);
    file_close (fildes->file);
    lock_release (&fs_lock);
    list_remove (&fildes->elem);
    free (fildes);
  }
}

static mapid_t
mmap (int fd, void *addr)
{
  struct file_descriptor *fildes = get_fildes(fd);

  if (!fildes || pg_ofs (addr) || !addr || !file_length (fildes->file))
    return MAP_FAILED;
  
  lock_acquire (&fs_lock);
  struct file *file = file_reopen (fildes->file);
  int len = file_length (file);
  lock_release (&fs_lock);

  return page_add_mmap_lazily (addr, file, len);
}

static void
munmap (mapid_t mapid)
{
  munmap_pages (mapid);
}

static void
kill_on_bad_uaddr (void *uaddr)
{
  if (!is_user_vaddr (uaddr) || !page_get_spte (uaddr))
    syscall_exit (ERROR);
}

static struct file_descriptor *
get_fildes (int fileno)
{
  struct list *fd_list = &thread_current ()->fd_list;
  for (struct list_elem *e = list_begin (fd_list);
       e != list_end (fd_list); e = list_next (e))
  {
    struct file_descriptor *fildes = list_entry (e, struct file_descriptor, elem);
    if (fildes->fileno == fileno)
      return fildes;
  }
  return NULL;
}
