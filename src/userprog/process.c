#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <float.h>
#include <stdlib.h>
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
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#include "lib/kernel/list.h"
#include "userprog/syscall.h"

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
static struct lock process_threads_lock;
bool setup_thread(void** esp, int thread_id);


/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;
  sema_init(&temporary, 0);
  lock_init(&process_threads_lock);

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;


  /* Kill the kernel if we did not succeed */
  ASSERT(success);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  /* Project 1 Modification: <file_name> string may contain command line arguments.
  Need to parse out just the name of the thread. ("args-single" test)*/
  char* rest; 
  size_t len = strlen(file_name);
  char filename_copy[len + 1];
  strlcpy(filename_copy, file_name, len + 1);
  char* extracted_name = strtok_r(filename_copy, " ", &rest);
  /* This parsing used to be in thread_create(), moved here in project 2, since process_execute
  is only called when executing a user program, so <file_name> string contains cmdline args,
  but thread_create is can also be called by tests. */ 

  /* Create a new thread to execute FILE_NAME. 
  Note: newly created thread will run <start_process> function with argument <fn_copy>,
  a copy of the command line input string. (This is again parsed in start_process --> load )*/
  tid = thread_create(extracted_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    palloc_free_page(fn_copy);
  
  sema_down(&thread_current()->child_sema);

  if (!thread_current()->execution){
    return TID_ERROR;
  }

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* file_name_) {
  char* file_name = (char*)file_name_;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;
  uint32_t fpu_temp[27];

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;
  list_init(&new_pcb->process_threads);

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);

    list_init(&t->pcb->file_desc_entry_list); /* Need to initialize the Pintos list representing the file table.*/
    t->pcb->next_available_fd = 2; /* fds 0 and 1 are reserved for STDIN an STDOUT respectively.  */
    list_init(&t->pcb->user_locks); /* Need to initialize the user locks Pintos list. */
    list_init(&t->pcb->user_semaphores); /* Need to initialize the user semaphores Pintos list. */
    lock_init(&t->pcb->syscall_lock);
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    fpu_init(&if_.fpu, &fpu_temp);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(file_name, &if_.eip, &if_.esp);
  }

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    printf("%s: exit(%d)\n",t->pcb->process_name , -1);
    t->pcb = NULL;
    free(pcb_to_free);
  }

  /* Clean up. Exit on failure or jump to userspace */
  palloc_free_page(file_name);
  if (!success) {
    thread_current()->parent->execution = false;
    thread_current()->self->exit = -1;
    sema_up(&thread_current()->parent->child_sema);
    thread_exit();
  }
  thread_current()->parent->execution = true;
  sema_up(&thread_current()->parent->child_sema);
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid UNUSED) {
  struct list_elem *start = list_begin(&thread_current()->childs_status_lst);
  struct child_status *c;
  while(start != list_end(&thread_current()->childs_status_lst)){
    c = list_entry(start, struct child_status, elem);
    if((c->tid == child_pid) && (!c->success)){
        c->success = true;
        sema_down (&c->wait_sema);
        break;
    }
    if (c->tid == child_pid){
      return -1;
    }
    start = list_next(start);
  }
  if (start == list_end (&thread_current()->childs_status_lst)) {
    return -1;
  }
  list_remove(start);
  int exit = c->exit;
  free(c);
  return exit;
}

/* Free the current process's resources. */
void process_exit(void) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  file_close(cur->pcb->exec);


  /* Freeing the file descriptor table entries. */
  while (!list_empty(&cur->pcb->file_desc_entry_list)) {
    struct list_elem *e = list_pop_front(&cur->pcb->file_desc_entry_list);
    struct file_desc_entry *f = list_entry(e, struct file_desc_entry, elem);
    file_close(f->fptr);    // frees the (struct file) embeded inside file_desc_entry
    free(f);
  }

  /* Freeing the user locks and semaphore lists. */
  while (!list_empty(&cur->pcb->user_locks)) {
    struct list_elem *e = list_pop_front(&cur->pcb->user_locks);
    struct user_lock_entry *f = list_entry(e, struct user_lock_entry, elem);
    free(f);
  }

  while (!list_empty(&cur->pcb->user_semaphores)) {
    struct list_elem *e = list_pop_front(&cur->pcb->user_semaphores);
    struct user_lock_entry *f = list_entry(e, struct user_sema_entry, elem);
    free(f);
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }
    {
    struct list_elem *elem, *next;
    for (elem = list_begin(&cur->pcb->process_threads); elem != list_end(&cur->pcb->process_threads);
         elem = next) {
      next = list_next(elem);
      struct process_thread* process_thread = list_entry(elem, struct process_thread, process_thread_elem);

      list_remove(&process_thread->process_thread_elem);
      free(process_thread);
    }
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);

  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(int argc, char** argv, int total_bytes, void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Parses the command line input string into tokens using strtok_r. 
   Returns argv array of arguments, argv[0] = executable filename,
   argv[argc] = NULL. Assigns num_args to argc. */
char** parse_cmd(char* cmdline, int* num_args, int* num_bytes) {
  int argc = 1;
  size_t len = strlen(cmdline);

  /* First, scan through the string to get the number of argumetns: argc */
  for (int i = 0; i < len; i++) {
    if (cmdline[i] == ' ' && cmdline[i+1] != '\0' && cmdline[i+1] != ' ') {
        argc += 1;
      }
  }
  *num_args = argc;

  /* argv: array of argument strings (char *), argv[argc] = NULL pointer. */
  char** argv = (char **) malloc(sizeof(char*) * (argc + 1));
  if (argv == NULL) {
    return NULL;
  }

  /* Since strtok_r() makes modification to the given string, make a copy. */
  char input_str[len + 1];
  strlcpy(input_str, cmdline, len + 1);
  char* rest;

  /* Total number of bytes of argument string: (each word) + '\0' */
  int total_bytes = 0;

  /* Using strtok_r() function to break the command line input string into words.*/
  char * token = strtok_r(input_str, " ", &rest);
  int i = 0;
  size_t n;
  char* tok_str;
  while (token != NULL) {
    /* Allocate memory for the argument token string, copy the token into that memory,
       and store the address of that memory into argv[i] */
    n = strlen(token);
    tok_str = (char *) malloc(n + 1);
    if (tok_str == NULL) {
      /* If malloc fails, free the previously malloc-ed strings in argv. 
      Currently, about to store argument i, so free all args up to i, using free_args*/
      free_args(i, argv);
      return NULL;
    }
    strlcpy(tok_str, token, n + 1);
    argv[i++] = tok_str;
    total_bytes += n + 1;
    token = strtok_r(NULL, " ", &rest);
  }
  argv[argc] = NULL;

  /* Write the total number of bytes of the argument strings into num_bytes*/
  *num_bytes = total_bytes;

  return argv;
}

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Parse commandline input string into argv vector, where argv[0] = executable name. 
     Writes argc = number of arguments, total_bytes = total bytes of each argument string + \0 */
  int argc, total_bytes;
  char** argv = parse_cmd(file_name, &argc, &total_bytes);
  /* In case one of the malloc's in parse_cmd failed, */
  if (argv == NULL) {
    /* Modeling off of starter code, go to done section, with success set to false.*/
    printf("Malloc failed\n");
    goto done;
  }

  /* Open executable file. */
  file = filesys_open(argv[0]);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  file_deny_write(file);
  t->pcb->exec = file;

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up user stack: push command line arguments and set initial esp register value. */
  if (!setup_stack(argc, argv, total_bytes, esp))   
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */

  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Since each string argv[i] has been malloc-ed memory, need to free it*/
void free_args(int argc, char** argv) {
  for (int i = 0; i < argc; i++) {
    free(argv[i]);
  }
  free(argv);
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Pushes command line arguments onto the stack and returns final location of initail esp. */
void* push_args(int argc, char** argv, int total_bytes) {
  
  char* init_esp = PHYS_BASE;
  /* Accounts for the actual bytes of args strings + argv[i] + NULL ptr + argv + argc */
  init_esp -= (total_bytes + (argc+1)*sizeof(char *) + sizeof(int) + sizeof(char **));

  /* Searches for number of bytes for padding. */
  int padding = 0;
  while ( ((unsigned int)(init_esp - padding)) % 16 != 0) {
    padding++;
  }
  /* init_esp is at bottom of the user stack (final position) */
  init_esp -= padding;
  
  /* this pointer is used to push args onto the stack*/
  char* sp = (char *)init_esp;

  /* Pushes argc and argv = addres of beginning of the array of char* to the actual arg string bytes */
  *((int *) sp) = argc;
  sp += 4;
  *((char ***) sp) = ((char **) sp) + 1;
  sp += 4;

  /* Copies the actual bytes of each arg string onto user stack, and simultanously storinf the pointer to strings
  Keep 2 stack pointers:
  sp = (bottom) points to the address of the string's bytes copied on user stack
  sp_2 = (top) points to where the actual bytes of the argument is written to */
  char* sp_2 = sp + padding + (argc+1)*sizeof(char *);
  for (int i = 0; i < argc; i++) {
    /* Copies the args string from argv array in kernel memory onto user stack,
    and stores the address of those strings onto the user stacka as user's argv */
    strlcpy((char *) sp_2, argv[i], strlen(argv[i])+1);
    *((char **) sp) = sp_2;

    /* Increment the bottom sp by 4 bytes (go to next pointer)
    Increment top sp_2 by number of bytes of the copied string */
    sp += 4;
    sp_2 += strlen(argv[i]) + 1;
  }

  /* Last entry of the argv vector is a NULL pointer */
  *((char **) sp) = NULL;

  /* After pushing each argument string onto the user stack, we can free the memory
  allocated to store these arg strings and the argv vector itself. */
  free_args(argc, argv);
  
  // fake rip
  init_esp -= 4;
  return init_esp;
}


/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. 
   Added: push argument onto user stack. */
static bool setup_stack(int argc, char** argv, int total_bytes, void** esp) {
  
  /* Example of stack setup for the _start function: cmd = "/bin/ls -l foo bar" 
  "bar\0"           <--- PHYS_BASE
  "foo\0"
  "-l\0"
  "/bin/ls\0"
  (...
      padding
   ...)
  argv[4] = NULL
  argv[3] = &("bar\0") 
  argv[2] = &("foo\0") 
  argv[1] = &("-l\0") 
  argv[0] = &("/bin/ls\0") 
  argv = &(argv[0])
  argc               <--- (16 byte allign) 
  (fake rip)

  Therefore, need decrement initial esp by: 
    (the actual strings)  (each argv[i] + NULL)  (arv + argc)
          
  which must be 16 byte allign. And then finally, decrement by 4 byte for "fake return address"
  */

  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success) {

      void* init_esp = push_args(argc, argv, total_bytes);

      *esp = init_esp;
    }
    else {
      palloc_free_page(kpage);
    }
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  bool result = (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));

  return result;
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void** esp, int thread_id) {
  uint8_t* kpage;
  bool success = false;

  ASSERT(thread_id > 0);

  // int i = thread_id;

  /* This allocates physical pages from user pool pf physical memory (PAL_USER) */
  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  // if (kpage != NULL) {
  //   /* base is a pointer in virtual address, indicating which section of virtual address space
  //   will be mapped to the allocated physical memory.*/
  //   uint8_t* base = (uint8_t*)PHYS_BASE - (PGSIZE * i * 2); 

  //   // use base - PGSIZE because installation of a page goes in reverse way
  //   // as opposed to stack growth
  //   /* Install_page sets up the mapping from virtual address to physical address in page table. */
  //   success = install_page(base - PGSIZE, kpage, true);
  //   if (success)
  //     *esp = base;    // virtual address of stck pointer --> base
  //   else
  //     palloc_free_page(kpage);
  // }

  for (uint8_t* vaddr = ((uint8_t*)PHYS_BASE) - PGSIZE; vaddr > 0; vaddr -= PGSIZE) {
    success = install_page(vaddr, kpage, true);
    if (success) {
      *esp = vaddr + PGSIZE;
      break;
    }
  }

  if (!success) {
    palloc_free_page(kpage);
  }

  return success;
}

struct start_pthread_args {
  stub_fun sf;
  pthread_fun tf;
  void* arg;
  struct process* pcb;
  bool setup_failed;
  struct semaphore process_thread_setup_wait;
};


/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf, pthread_fun tf, void* arg) {
  tid_t tid;
  struct start_pthread_args* start_pthread_args = malloc(sizeof(struct start_pthread_args));
  if (start_pthread_args == NULL) {
    return TID_ERROR;
  }

  struct thread* cur = thread_current();

  start_pthread_args->sf = sf;
  start_pthread_args->tf = tf;
  start_pthread_args->arg = arg;
  start_pthread_args->pcb = cur->pcb;
  sema_init(&start_pthread_args->process_thread_setup_wait, 0);

  tid = thread_create(cur->name, PRI_DEFAULT, start_pthread, start_pthread_args);
  sema_down(&start_pthread_args->process_thread_setup_wait);
  if (tid == TID_ERROR) {
    free(start_pthread_args);
    return TID_ERROR;
  }

  free(start_pthread_args);

  return tid;
}

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* args_) {
  struct start_pthread_args* args = (struct start_pthread_args*)args_;
  struct intr_frame if_;
  bool success = false;
  struct thread* t = thread_current();

  struct process_thread* process_thread = malloc(sizeof(struct process_thread));
  if (process_thread == NULL) {
    args->setup_failed = true;
    sema_up(&args->process_thread_setup_wait);
    thread_exit();
  }

  t->pcb = args->pcb;
  process_activate();

  process_thread->tid = t->tid;
  process_thread->thread_exited = false;
  process_thread->thread_waiter = NULL;
  sema_init(&process_thread->exit_wait, 0);

  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  if_.eip = (void*)args->sf;      // set instruction pointer eip to stub_func 

  lock_acquire(&process_threads_lock);
  t->process_thread_id = list_size(&args->pcb->process_threads) + 1;
  list_push_back(&args->pcb->process_threads, &process_thread->process_thread_elem);
  lock_release(&process_threads_lock);

  /* Set up the stack for the newly created user pthread */
  success = setup_thread(&if_.esp,t->process_thread_id);

  if (!success) {
    free(process_thread);
    args->setup_failed = true;
    sema_up(&args->process_thread_setup_wait);
    thread_exit();
  }

  /* Notes: So far, we have create the thread stuct for the new user thread, and setup the user t hread's stack.
  Now, we need to invoke the sstub function, which will call the user requested pthread function (with pthread_exit() at the end)
  Therefore, just like process execute, we need to manually push the arguments required by stub func: <pthread func> and <args> onto the stack
  And jump-start the execution of stub func by returning to user space via intr_exit (asm violatile). */

  uint32_t* esp = (uint32_t*) if_.esp;

  esp -= 1;
  *esp= NULL; // stack align
  esp -= 1;
  *esp= NULL; // stack align
  esp -= 1;
  *esp = args->arg; // push arg
  esp -= 1;
  *esp = args->tf; // push tf
  esp -= 1;
  *esp = NULL; // push fake return address

  if_.esp = (void **) esp;

  args->setup_failed = false;
  sema_up(&args->process_thread_setup_wait);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid) {
  struct list_elem* e;
  struct thread* curr_thread = thread_current();
  struct process_thread* process_thread = NULL;

  lock_acquire(&process_threads_lock);

  /** Looking for current process thread*/
  for (e = list_begin(&curr_thread->pcb->process_threads); e != list_end(&curr_thread->pcb->process_threads);
      e = list_next(e)) {
    struct process_thread* current_process_thread =
        list_entry(e, struct process_thread, process_thread_elem);
    if (current_process_thread->tid == tid) {
      process_thread = current_process_thread;
      break;
    }
  }

 if (process_thread == NULL || process_thread->thread_waiter != NULL) {
    lock_release(&process_threads_lock);
    return TID_ERROR;
  }

  if (process_thread->thread_exited) {
    lock_release(&process_threads_lock);
    return tid;
  }

  process_thread->thread_waiter = curr_thread;

  lock_release(&process_threads_lock);

  /* Fixed deadlock: when pthread join calls sema_down to sleep and wait for target thread to exit,
  need to release the syscall_lock, so that the target thread's pthread_exit syscall can proceed. 
  Also, need to re-acquire syscall_lock, because in syscall_handler, after this function returns,
  the syscall_lock is released at the end.
  (create-simple test) */
  lock_release(&curr_thread->pcb->syscall_lock);
  sema_down(&process_thread->exit_wait);
  lock_acquire(&curr_thread->pcb->syscall_lock);
  return tid;
}

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {
  struct list_elem* e;
  struct thread* curr_thread = thread_current();

  struct process_thread* process_thread = NULL;

  lock_acquire(&process_threads_lock);

  for (e = list_begin(&curr_thread->pcb->process_threads); e != list_end(&curr_thread->pcb->process_threads);
       e = list_next(e)) {
    struct process_thread* current_process_thread =
        list_entry(e, struct process_thread, process_thread_elem);
    if (current_process_thread->tid == curr_thread->tid) {
      process_thread = current_process_thread;
      break;
    }
  }

  ASSERT(process_thread != NULL);

  process_thread->thread_exited = true;

  lock_release(&process_threads_lock);

  /* When pthread_exit finished exiting and calls sema_up to signal waiter (the thread that called join on me)
  need to relinquish the syscall_lock, because pthread_join re-acquires syscall_lock after waking up from sema_down.
  Since this function exists the thread and never returns, don't need to re-acquire lock. */
  lock_release(&curr_thread->pcb->syscall_lock);
  sema_up(&process_thread->exit_wait);

  thread_exit();
}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}
