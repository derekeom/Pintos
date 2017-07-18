#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#define ERROR -1

/* Process identifier. */
typedef int pid_t;
#define PID_ERROR ((pid_t) -1)

/* Map region identifier. */
typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

void syscall_init (void);
void syscall_exit (int status);

#endif /* userprog/syscall.h */
