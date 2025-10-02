#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

//static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    if(flags & 0x4)
      perm |= PTE_R;
    return perm;
}

//
// the implementation of the exec() system call
//
// In kernel/exec.c -> Please use this as the final version

int
kexec(char *path, char **argv)
{
  printf("hi\n");
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  uint64 oldsz;
  struct proc *p = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Release the old executable, if any, and get a new reference.
  if(p->executable)
    iput(p->executable);
  p->executable = idup(ip);

  // Set up lazy-loaded pages for the new executable.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;

    // Correctly calculate the new process size.
    uint64 new_sz = ph.vaddr + ph.memsz;
    if(new_sz > sz)
       sz = new_sz;

    uint64 va;
    for(va = PGROUNDDOWN(ph.vaddr); va < ph.vaddr + ph.memsz; va += PGSIZE) {
      pte_t *pte = walk(pagetable, va, 1);
      if(pte == 0)
        goto bad;
      
      *pte = PTE_U | PTE_ONDEMAND;
      if(ph.flags & ELF_PROG_FLAG_READ)   *pte |= PTE_R;
      if(ph.flags & ELF_PROG_FLAG_WRITE)  *pte |= PTE_W;
      if(ph.flags & ELF_PROG_FLAG_EXEC)   *pte |= PTE_X;
      *pte &= ~PTE_V;
    }
  }

  iunlockput(ip);
  end_op();
  ip = 0;

  oldsz = p->sz;
  oldpagetable = p->pagetable;
  sz = PGROUNDUP(sz);
  
  // Allocate the user stack.
  if((sz = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  // Push arguments to the stack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG) goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;
    if(sp < stackbase) goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase) goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0) goto bad;
  
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
  
  // Commit to the new user image.
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;
  p->trapframe->sp = sp;
  p->trapframe->a1 = sp;

  proc_freepagetable(oldpagetable, oldsz);

  return argc;

bad:
  printf("[pid %d] kexec failed for path '%s'. Entered bad label.\n", p->pid, path);

  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  // If we get here, release the new executable reference we might have taken.
  if(p->executable) {
    iput(p->executable);
    p->executable = 0;
  }
  return -1;
}
// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
/*static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
*/