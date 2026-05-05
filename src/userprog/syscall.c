#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include <string.h>

static struct lock filesys_lock;
static void syscall_handler (struct intr_frame *);

static int get_user (const uint8_t *uaddr);
static bool valid_ptr (const void *ptr);
static bool valid_str (const char *str);
static bool valid_buf (const void *buf, unsigned size, bool write);
static uint32_t get_arg (struct intr_frame *f, int n);

void
syscall_init (void)
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

static bool
valid_ptr (const void *ptr)
{
  if (ptr == NULL)
    return false;
  if (!is_user_vaddr (ptr))
    return false;
  if (get_user ((const uint8_t *) ptr) == -1)
    return false;
  return true;
}

static bool
valid_str (const char *str)
{
  if (!valid_ptr (str))
    return false;
  while (true)
    {
      int c = get_user ((const uint8_t *) str);
      if (c == -1)
        return false;
      if (c == '\0')
        return true;
      str++;
    }
}

static bool
valid_buf (const void *buf, unsigned size, bool write UNUSED)
{
  unsigned i;
  if (size == 0)
    return true;
  if (buf == NULL)
    return false;
  for (i = 0; i < size; i++)
    {
      const uint8_t *p = (const uint8_t *) buf + i;
      if (!is_user_vaddr (p))
        return false;
      if (pagedir_get_page (thread_current ()->pagedir, p) == NULL)
        return false;
    }
  return true;
}

static uint32_t
get_arg (struct intr_frame *f, int n)
{
  uint32_t *ptr = (uint32_t *) f->esp + n;
  if (!valid_ptr (ptr))
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }
  return *ptr;
}

static struct file *
get_file (int fd)
{
  struct thread *t = thread_current ();
  struct list_elem *e;
  struct fd_entry *entry;
  for (e = list_begin (&t->fd_table); e != list_end (&t->fd_table); e = list_next (e))
    {
      entry = list_entry (e, struct fd_entry, elem);
      if (entry->fd == fd)
        return entry->file;
    }
  return NULL;
}

static void
syscall_handler (struct intr_frame *f)
{
  if (!valid_ptr (f->esp))
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }

  int syscall_num = *(int *) f->esp;

  switch (syscall_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      {
        int status = (int) get_arg (f, 1);
        thread_current ()->exit_status = status;
        thread_exit ();
        break;
      }

    case SYS_EXEC:
      {
        const char *cmd = (const char *) get_arg (f, 1);
        if (!valid_str (cmd))
          {
            thread_current ()->exit_status = -1;
            thread_exit ();
          }
        tid_t tid = process_execute (cmd);
        f->eax = tid;
        break;
      }

    case SYS_WAIT:
      {
        tid_t pid = (tid_t) get_arg (f, 1);
        f->eax = process_wait (pid);
        break;
      }

    case SYS_CREATE:
      {
        const char *file = (const char *) get_arg (f, 1);
        unsigned size = (unsigned) get_arg (f, 2);
        if (!valid_str (file))
          {
            thread_current ()->exit_status = -1;
            thread_exit ();
          }
        lock_acquire (&filesys_lock);
        bool ok = filesys_create (file, size);
        lock_release (&filesys_lock);
        f->eax = ok;
        break;
      }

    case SYS_REMOVE:
      {
        const char *file = (const char *) get_arg (f, 1);
        if (!valid_str (file))
          {
            thread_current ()->exit_status = -1;
            thread_exit ();
          }
        lock_acquire (&filesys_lock);
        bool ok = filesys_remove (file);
        lock_release (&filesys_lock);
        f->eax = ok;
        break;
      }

    case SYS_OPEN:
      {
        const char *file = (const char *) get_arg (f, 1);
        if (!valid_str (file))
          {
            thread_current ()->exit_status = -1;
            thread_exit ();
          }
        lock_acquire (&filesys_lock);
        struct file *fp = filesys_open (file);
        lock_release (&filesys_lock);
        if (fp == NULL)
          {
            f->eax = -1;
            break;
          }
        struct thread *t = thread_current ();
        struct fd_entry *entry = malloc (sizeof (struct fd_entry));
        if (entry == NULL)
          {
            file_close (fp);
            f->eax = -1;
            break;
          }
        entry->fd = t->next_fd++;
        entry->file = fp;
        list_push_back (&t->fd_table, &entry->elem);
        f->eax = entry->fd;
        break;
      }

    case SYS_FILESIZE:
      {
        int fd = (int) get_arg (f, 1);
        lock_acquire (&filesys_lock);
        struct file *fp = get_file (fd);
        int size = fp ? (int) file_length (fp) : -1;
        lock_release (&filesys_lock);
        f->eax = size;
        break;
      }

    case SYS_READ:
      {
        int fd = (int) get_arg (f, 1);
        void *buf = (void *) get_arg (f, 2);
        unsigned size = (unsigned) get_arg (f, 3);
        if (!valid_buf (buf, size, true))
          {
            thread_current ()->exit_status = -1;
            thread_exit ();
          }
        if (fd == 0)
          {
            unsigned i;
            for (i = 0; i < size; i++)
              ((uint8_t *) buf)[i] = input_getc ();
            f->eax = size;
          }
        else
          {
            lock_acquire (&filesys_lock);
            struct file *fp = get_file (fd);
            int bytes = fp ? (int) file_read (fp, buf, size) : -1;
            lock_release (&filesys_lock);
            f->eax = bytes;
          }
        break;
      }

    case SYS_WRITE:
      {
        int fd = (int) get_arg (f, 1);
        const void *buf = (const void *) get_arg (f, 2);
        unsigned size = (unsigned) get_arg (f, 3);
        if (!valid_buf (buf, size, false))
          {
            thread_current ()->exit_status = -1;
            thread_exit ();
          }
        if (fd == 1)
          {
            putbuf (buf, size);
            f->eax = size;
          }
        else
          {
            lock_acquire (&filesys_lock);
            struct file *fp = get_file (fd);
            int bytes = fp ? (int) file_write (fp, buf, size) : -1;
            lock_release (&filesys_lock);
            f->eax = bytes;
          }
        break;
      }

    case SYS_SEEK:
      {
        int fd = (int) get_arg (f, 1);
        unsigned pos = (unsigned) get_arg (f, 2);
        lock_acquire (&filesys_lock);
        struct file *fp = get_file (fd);
        if (fp)
          file_seek (fp, pos);
        lock_release (&filesys_lock);
        break;
      }

    case SYS_TELL:
      {
        int fd = (int) get_arg (f, 1);
        lock_acquire (&filesys_lock);
        struct file *fp = get_file (fd);
        unsigned pos = fp ? (unsigned) file_tell (fp) : 0;
        lock_release (&filesys_lock);
        f->eax = pos;
        break;
      }

    case SYS_CLOSE:
      {
        int fd = (int) get_arg (f, 1);
        struct thread *t = thread_current ();
        struct list_elem *e;
        for (e = list_begin (&t->fd_table); e != list_end (&t->fd_table); e = list_next (e))
          {
            struct fd_entry *entry = list_entry (e, struct fd_entry, elem);
            if (entry->fd == fd)
              {
                lock_acquire (&filesys_lock);
                file_close (entry->file);
                lock_release (&filesys_lock);
                list_remove (&entry->elem);
                free (entry);
                break;
              }
          }
        break;
      }

    default:
      thread_current ()->exit_status = -1;
      thread_exit ();
      break;
    }
}
