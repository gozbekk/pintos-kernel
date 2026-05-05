#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

struct start_info
{
  char *cmd;
  struct child_info *ci;
};

tid_t
process_execute (const char *file_name)
{
  char *fn_copy;
  char *tmp;
  char *save_ptr;
  char *prog_name;
  tid_t tid;
  struct child_info *ci;
  struct start_info *si;

  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  tmp = palloc_get_page (0);
  if (tmp == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (tmp, file_name, PGSIZE);
  prog_name = strtok_r (tmp, " ", &save_ptr);

  ci = malloc (sizeof (struct child_info));
  if (ci == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (tmp);
      return TID_ERROR;
    }

  ci->tid = TID_ERROR;
  ci->my_thread_ref = TID_ERROR;
  ci->exit_status = -1;
  ci->exited = false;
  ci->waited = false;
  ci->load_success = false;
  sema_init (&ci->sema, 0);

  si = malloc (sizeof (struct start_info));
  if (si == NULL)
    {
      free (ci);
      palloc_free_page (fn_copy);
      palloc_free_page (tmp);
      return TID_ERROR;
    }

  si->cmd = fn_copy;
  si->ci = ci;

  tid = thread_create (prog_name, PRI_DEFAULT, start_process, si);
  palloc_free_page (tmp);

  if (tid == TID_ERROR)
    {
      free (si);
      free (ci);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  ci->tid = tid;
  ci->my_thread_ref = tid;
  list_push_back (&thread_current ()->children, &ci->elem);

  sema_down (&ci->sema);

  if (!ci->load_success)
    return TID_ERROR;

  return tid;
}

static void
start_process (void *file_name_)
{
  struct start_info *si = file_name_;
  char *file_name = si->cmd;
  struct intr_frame if_;
  bool success;

  thread_current ()->my_info = si->ci;
  free (si);

  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  if (thread_current ()->my_info != NULL)
    {
      thread_current ()->my_info->load_success = success;
      sema_up (&thread_current ()->my_info->sema);
    }

  palloc_free_page (file_name);
  if (!success)
    thread_exit ();

  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

int
process_wait (tid_t child_tid)
{
  struct thread *cur = thread_current ();
  struct child_info *ci = NULL;
  struct list_elem *e;

  for (e = list_begin (&cur->children); e != list_end (&cur->children); e = list_next (e))
    {
      struct child_info *c = list_entry (e, struct child_info, elem);
      if (c->tid == child_tid)
        {
          ci = c;
          break;
        }
    }

  if (ci == NULL || ci->waited)
    return -1;

  ci->waited = true;

  if (!ci->exited)
    sema_down (&ci->sema);

  int status = ci->exit_status;
  list_remove (&ci->elem);
  free (ci);
  return status;
}

void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  if (cur->pagedir != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  if (cur->exec_file != NULL)
    {
      file_allow_write (cur->exec_file);
      file_close (cur->exec_file);
    }

  struct list_elem *e = list_begin (&cur->fd_table);
  while (e != list_end (&cur->fd_table))
    {
      struct fd_entry *fe = list_entry (e, struct fd_entry, elem);
      e = list_next (e);
      file_close (fe->file);
      free (fe);
    }

  if (cur->my_info != NULL)
    {
      cur->my_info->exit_status = cur->exit_status;
      cur->my_info->exited = true;
      sema_up (&cur->my_info->sema);
    }

  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

void
process_activate (void)
{
  struct thread *t = thread_current ();

  if (init_page_dir == NULL)
    return;

  if (t->pagedir != NULL)
    pagedir_activate (t->pagedir);
  else
    pagedir_activate (NULL);

  tss_update ();
}

typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

#define PE32Wx PRIx32
#define PE32Ax PRIx32
#define PE32Ox PRIx32
#define PE32Hx PRIx16

struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp, const char *cmd_line);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  char *fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return false;
  strlcpy (fn_copy, file_name, PGSIZE);

  char *save_ptr;
  char *prog_name = strtok_r (fn_copy, " ", &save_ptr);

  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  file = filesys_open (prog_name);
  if (file == NULL)
    goto done;

  file_deny_write (file);
  t->exec_file = file;

  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    goto done;

  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  if (!setup_stack (esp, file_name))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  palloc_free_page (fn_copy);
  if (!success && file != NULL)
    {
      file_allow_write (file);
      file_close (file);
      t->exec_file = NULL;
    }
  return success;
}

static bool install_page (void *upage, void *kpage, bool writable);

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;
  if (phdr->p_memsz < phdr->p_filesz)
    return false;
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;
  if (phdr->p_vaddr < PGSIZE)
    return false;
  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool
setup_stack (void **esp, const char *cmd_line)
{
  uint8_t *kpage;
  bool success;
  char *args[128];
  char *arg_ptrs[128];
  int argc = 0;
  int i;
  char *copy;
  char *token;
  char *save_ptr;
  uint8_t *ksp;
  uintptr_t user_base;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
  if (!success)
    {
      palloc_free_page (kpage);
      return false;
    }

  copy = palloc_get_page (0);
  if (copy == NULL)
    return false;

  strlcpy (copy, cmd_line, PGSIZE);

  for (token = strtok_r (copy, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      if (argc >= 128)
        {
          palloc_free_page (copy);
          return false;
        }
      args[argc++] = token;
    }

  ksp = kpage + PGSIZE;
  user_base = (uintptr_t) PHYS_BASE - PGSIZE;

  for (i = argc - 1; i >= 0; i--)
    {
      int len = strlen (args[i]) + 1;
      ksp -= len;
      if (ksp < kpage)
        {
          palloc_free_page (copy);
          return false;
        }
      memcpy (ksp, args[i], len);
      arg_ptrs[i] = (char *) (user_base + (ksp - kpage));
    }

  while ((uintptr_t) (user_base + (ksp - kpage)) % 4 != 0)
    {
      ksp--;
      if (ksp < kpage)
        {
          palloc_free_page (copy);
          return false;
        }
      *ksp = 0;
    }

  ksp -= 4;
  *(char **) ksp = NULL;

  for (i = argc - 1; i >= 0; i--)
    {
      ksp -= 4;
      *(char **) ksp = arg_ptrs[i];
    }

  char *argv = (char *) (user_base + (ksp - kpage));

  ksp -= 4;
  *(char ***) ksp = (char **) argv;

  ksp -= 4;
  *(int *) ksp = argc;

  ksp -= 4;
  *(void **) ksp = NULL;

  *esp = (void *) (user_base + (ksp - kpage));

  palloc_free_page (copy);
  return true;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
