#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "memstat.h"

extern struct inode* create(char *path, short type, short major, short minor);
void mark_page_dirty(struct proc *p, uint64 va);

pagetable_t kernel_pagetable;
extern char etext[];
extern char trampoline[];

pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;
  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
  proc_mapstacks(kpgtbl);
  return kpgtbl;
}

void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

void
kvminithart()
{
  sfence_vma();
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");
  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");
  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;   
    if((*pte & PTE_V) == 0)
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      if(fifo_victim_selection() < 0) {
        uvmdealloc(pagetable, a, oldsz);
        return 0;
      }
      mem = kalloc();
      if (mem == 0) {
        panic("uvmalloc: kalloc failed after replacement");
      }
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

void
freewalk(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;

    if(*pte & PTE_V) {
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      if((mem = kalloc()) == 0)
        goto err;
      memmove(mem, (char*)pa, PGSIZE);
      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto err;
      }
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 15)) < 0) {
        return -1;
      }
      pa0 = walkaddr(pagetable, va0);
      if(pa0==0)
        return -1;
    }

    pte = walk(pagetable, va0, 0);
    if((*pte & PTE_W) == 0)
      return -1;
    mark_page_dirty(myproc(), va0);
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 13)) < 0) {
        return -1;
      }
      pa0 = walkaddr(pagetable, va0);
      if(pa0 == 0)
        return -1;
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

void
build_swapname_helper(struct proc *p, char *buf)
{
  int n = 0;
  buf[n++] = '/';
  buf[n++] = 'p'; buf[n++] = 'g'; buf[n++] = 's'; buf[n++] = 'w'; buf[n++] = 'p';
  int pid = p->pid;
  char tmp[16];
  int ti = 0;
  if (pid == 0) tmp[ti++] = '0';
  while (pid > 0) { tmp[ti++] = '0' + (pid % 10); pid /= 10; }
  for (int j = ti-1; j >= 0; j--) buf[n++] = tmp[j];
  buf[n] = 0;
}

static int
ensure_swapfile(struct proc *p)
{
  if (p->swapfile)
    return 0;

  char name[32];
  build_swapname_helper(p, name);

  begin_op();
  struct inode *ip = create(name, T_FILE, 0, 0);
  if (!ip) {
    end_op();
    return -1;
  }
  end_op();

  struct file *f = filealloc();
  if (f == 0) {
    iput(ip);
    return -1;
  }
  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = 1;
  f->writable = 1;

  p->swapfile = f;
  p->num_swap_used = 0;
  for (int i = 0; i < SWAP_MAX_PAGES; i++) {
    p->swap_slots[i].used = 0;
    p->swap_slots[i].va = 0;
  }
  printf("[pid %d] SWAPFILE created: %s\n", p->pid, name);
  return 0;
}

static int
find_free_swap_slot(struct proc *p)
{
  for (int i = 0; i < SWAP_MAX_PAGES; i++) {
    if (!p->swap_slots[i].used)
      return i;
  }
  return -1;
}

// Write page at pa to a free swap slot; returns slot index or -1 on error
static int
swap_out_page(struct proc *p, uint64 va, uint64 pa)
{
  if (ensure_swapfile(p) < 0)
    return -1;

  int slot = find_free_swap_slot(p);
  if (slot < 0) {
    return -1; // swap full
  }

  uint offset = slot * PGSIZE;
  begin_op();
  int written = writei(p->swapfile->ip, 0, pa, offset, PGSIZE);
  end_op();
  if (written != PGSIZE) {
    //printf("[pid %d] swap_out_page: writei returned %d, expected %d\n", p->pid, written, PGSIZE);
    return -1;
  }

  p->swap_slots[slot].used = 1;
  p->swap_slots[slot].va = va;
  p->num_swap_used++;
  printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, va, slot);
  return slot;
}

// Read page for va from swap into mem (must be allocated)
static int
swap_in_page(struct proc *p, uint64 va, char *mem)
{
  if (!p->swapfile)
    return -1;
  
  int slot = -1;
  for (int i = 0; i < SWAP_MAX_PAGES; i++) {
    if (p->swap_slots[i].used && p->swap_slots[i].va == va) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return -1;

  uint offset = slot * PGSIZE;
  
  int n = readi(p->swapfile->ip, 0, (uint64)mem, offset, PGSIZE);
  
  
  if (n != PGSIZE) {
    printf("[pid %d] swap_in_page: readi returned %d, expected %d\n", p->pid, n, PGSIZE);
    return -1;
  }

  p->swap_slots[slot].used = 0;
  p->swap_slots[slot].va = 0;
  p->num_swap_used--;

  return slot;
}
void mark_page_dirty(struct proc *p, uint64 va) {
  for (int i = 0; i < p->num_resident; i++) {
    if (p->resident_set[i].va == va) {
      p->resident_set[i].is_dirty = 1;
      return;
    }
  }
}

uint64
vmfault(pagetable_t pagetable, uint64 va, uint scause)
{
  struct proc *p = myproc();
  pte_t *pte;
  uint64 mem;

  char *access_type;
  if (scause == 12) access_type = "exec";
  else if (scause == 13) access_type = "read";
  else if (scause == 15 || scause == 7) access_type = "write";
  else access_type = "unknown";

  if (va >= MAXVA) {
    goto kill;
  }
  int is_write_fault = (scause == 15 || scause == 7);
  uint64 pa_va = PGROUNDDOWN(va);
  pte = walk(pagetable, pa_va, 0);
  if (is_write_fault) {
    mark_page_dirty(p, pa_va);
  }

  if(pte == 0 || (*pte & PTE_V) == 0) {

    // Check if page is in swap
    if (p->swapfile) {
      for (int si = 0; si < SWAP_MAX_PAGES; si++) {
        if (p->swap_slots[si].used && p->swap_slots[si].va == pa_va) {
          printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=swap\n", p->pid, va, access_type);
          if (p->num_resident >= MAX_RESIDENT_PAGES) {
            if (fifo_victim_selection() < 0) goto kill;
          }
          if ((mem = (uint64)kalloc()) == 0) panic("vmfault: kalloc failed");
          memset((void*)mem, 0, PGSIZE);

          int slot = swap_in_page(p, pa_va, (char*)mem);
          if (slot < 0) { kfree((void*)mem); goto kill; }

          if (mappages(pagetable, pa_va, PGSIZE, mem, PTE_R | PTE_W | PTE_U | PTE_V) != 0) { 
            kfree((void*)mem); 
            goto kill; 
          }
          printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, pa_va, slot);

          if (p->num_resident >= MAX_RESIDENT_PAGES) 
          panic("resident set overflow after replacement");
          p->resident_set[p->num_resident].va = pa_va;
          p->resident_set[p->num_resident].seq = p->next_fifo_seq;
          p->resident_set[p->num_resident].is_dirty = 0;
          p->num_resident++;
          printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, pa_va, p->next_fifo_seq);
          p->next_fifo_seq++;
          return 0;
        }
      }
    }

    // Check if it's an executable page
    int is_exec_page = 0;
    struct elfhdr elf;
    struct proghdr ph;

    if (p->executable) {
      if (readi(p->executable, 0, (uint64)&elf, 0, sizeof(elf)) == sizeof(elf)) {
        for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
          if (readi(p->executable, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
            break;
          if (ph.vaddr <= pa_va && pa_va < ph.vaddr + ph.memsz) {
            is_exec_page = 1;
            break;
          }
        }
      }
    }

    if (is_exec_page) {
      printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n", p->pid, va, access_type);
      if (p->num_resident >= MAX_RESIDENT_PAGES) {
        if(fifo_victim_selection() < 0) {
          goto kill;
        }
      }
      if((mem = (uint64)kalloc()) == 0) panic("vmfault: kalloc failed");
      memset((void*)mem, 0, PGSIZE);

      for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (readi(p->executable, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
          break;
        if (ph.vaddr <= pa_va && pa_va < ph.vaddr + ph.memsz) {
          uint flags = PTE_U | PTE_V;
          if(ph.flags & ELF_PROG_FLAG_READ) flags |= PTE_R;
          if(ph.flags & ELF_PROG_FLAG_WRITE) flags |= PTE_W;
          if(ph.flags & ELF_PROG_FLAG_EXEC) flags |= PTE_X;
          if(mappages(pagetable, pa_va, PGSIZE, mem, flags) != 0) { 
            kfree((void*)mem); 
            goto kill; 
          }
          p->resident_set[p->num_resident].va  = pa_va;
          p->resident_set[p->num_resident].seq = p->next_fifo_seq++;
          p->resident_set[p->num_resident].is_dirty = is_write_fault ? 1 : 0;  // usually 0 for exec/text
          p->num_resident++;

          uint64 file_offset = ph.off + (pa_va - ph.vaddr);
          uint read_sz = (ph.filesz > (pa_va - ph.vaddr)) ? (ph.filesz - (pa_va - ph.vaddr)) : 0;
          if(read_sz > PGSIZE) read_sz = PGSIZE;
          if (read_sz > 0 && readi(p->executable, 0, mem, file_offset, read_sz) != read_sz) { 
            kfree((void*)mem); 
            goto kill; 
          }
          break;
        }
      }
      printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, pa_va);
      return 0;
    } 
    else if (va >= p->trapframe->sp - PGSIZE && va < p->trapframe->sp) {
      printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=stack\n", p->pid, va, access_type);
      if (p->num_resident >= MAX_RESIDENT_PAGES) {
        if(fifo_victim_selection() < 0) {
          goto kill;
        }
      }
      if((mem = (uint64)kalloc()) == 0) panic("vmfault: kalloc failed");
      memset((void*)mem, 0, PGSIZE);
      printf("[pid %d] ALLOC va=0x%lx\n", p->pid, pa_va);
      if(mappages(pagetable, pa_va, PGSIZE, mem, PTE_R | PTE_W | PTE_U | PTE_V) != 0) { 
        kfree((void*)mem); 
        goto kill; 
      }
      p->resident_set[p->num_resident].va  = pa_va;
      p->resident_set[p->num_resident].seq = p->next_fifo_seq++;
      p->resident_set[p->num_resident].is_dirty = is_write_fault ? 1 : 0;  // usually 0 for exec/text
      p->num_resident++;

      printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, pa_va, p->next_fifo_seq-1);
      return 0;
    }
    else if (va < p->sz) {
      printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=heap\n", p->pid, va, access_type);
      if (p->num_resident >= MAX_RESIDENT_PAGES) {
        if(fifo_victim_selection() < 0) {
          goto kill;
        }
      }
      if((mem = (uint64)kalloc()) == 0) panic("vmfault: kalloc failed");
      memset((void*)mem, 0, PGSIZE);
      printf("[pid %d] ALLOC va=0x%lx\n", p->pid, pa_va);
      if(mappages(pagetable, pa_va, PGSIZE, mem, PTE_R | PTE_W | PTE_U | PTE_V) != 0) { 
        kfree((void*)mem); 
        goto kill; 
      }
      p->resident_set[p->num_resident].va  = pa_va;
      p->resident_set[p->num_resident].seq = p->next_fifo_seq++;
      p->resident_set[p->num_resident].is_dirty = is_write_fault ? 1 : 0;  // usually 0 for exec/text
      p->num_resident++;
      printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, pa_va, p->next_fifo_seq-1);
      return 0;
    }
    else {
      goto kill;
    }

    /*if (p->num_resident >= MAX_RESIDENT_PAGES) {
      int count=p->num_resident;
      printf("[pid %d] num_resident=%d MAX_RESIDENT_PAGES=%d\n", p->pid, count, MAX_RESIDENT_PAGES);
      panic("resident set overflow after replacement");
    }
    p->resident_set[p->num_resident].va = pa_va;
    p->resident_set[p->num_resident].seq = p->next_fifo_seq;
    p->num_resident++;
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, pa_va, p->next_fifo_seq);
    p->next_fifo_seq++;
    return 0;*/
  }
  
kill:
  printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n", p->pid, va, access_type);
  setkilled(p);
  return -1;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

// vm.c
int
fifo_victim_selection(void)
{
  struct proc *p = myproc();

  printf("[pid %d] MEMFULL\n", p->pid);
  if (p->num_resident == 0)
    return -1;

  for (;;) {
    // Find the oldest (min seq) among CURRENT entries
    int victim_idx = -1;
    int min_seq = -1;
    for (int i = 0; i < p->num_resident; i++) {
      if (victim_idx < 0 || p->resident_set[i].seq < min_seq) {
        min_seq = p->resident_set[i].seq;
        victim_idx = i;
      }
    }
    if (victim_idx < 0)
      return -1; // nothing left

    uint64 victim_va = p->resident_set[victim_idx].va;
    printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", p->pid, victim_va, min_seq);

    // Check current residency
    pte_t *pte = walk(p->pagetable, victim_va, 0);
    if (pte == 0 || (*pte & PTE_V) == 0) {
      // STALE entry: drop it from resident_set and retry
      for (int j = victim_idx; j < p->num_resident - 1; j++)
        p->resident_set[j] = p->resident_set[j+1];
      p->num_resident--;
      continue;
    }

    uint64 pa = PTE2PA(*pte);
    int is_dirty = 0;
    for (int k = 0; k < p->num_resident; k++) {
      if (p->resident_set[k].va == victim_va) {
        is_dirty = p->resident_set[k].is_dirty;
        break;
      }
    }
    printf("[pid %d] EVICT va=0x%lx state=%s\n", p->pid, victim_va, is_dirty ? "dirty" : "clean");

    if (!is_dirty) {
      // Clean page: just drop mapping + free frame
      printf("[pid %d] DISCARD va=0x%lx\n", p->pid, victim_va);
      if (pa) kfree((void*)pa);
      uvmunmap(p->pagetable, victim_va, 1, 0);
    } else {
      // Dirty page: write to swap, then drop mapping + free frame
      int slot = swap_out_page(p, victim_va, pa);
      if (slot < 0) {
        printf("[pid %d] SWAPFULL\n", p->pid);
        printf("[pid %d] KILL swap-exhausted\n", p->pid);
        setkilled(p);
        return -1;
      }
      // swap_out_page() already logs SWAPOUT
      if (pa) kfree((void*)pa);
      uvmunmap(p->pagetable, victim_va, 1, 0);
    }

    // Remove victim from resident_set (it’s no longer resident)
    for (int j = victim_idx; j < p->num_resident - 1; j++)
      p->resident_set[j] = p->resident_set[j+1];
    p->num_resident--;

    return 0;
  }
}
void
fill_memstat(struct proc_mem_stat *info)
{
  struct proc *p = myproc();
  memset(info, 0, sizeof(*info));

  info->pid = p->pid;
  info->next_fifo_seq = p->next_fifo_seq;
  info->num_resident_pages = p->num_resident;
  info->num_swapped_pages = p->num_swap_used;

  // total = resident + swapped + holes up to sz (simplified)
  info->num_pages_total = PGROUNDUP(p->sz) / PGSIZE;

  int count = 0;
  for (int i = 0; i < p->num_resident && count < MAX_PAGES_INFO; i++) {
    info->pages[count].va = p->resident_set[i].va;
    info->pages[count].state = RESIDENT;
    info->pages[count].is_dirty = p->resident_set[i].is_dirty;  // if you track dirty, fill here
    info->pages[count].seq = p->resident_set[i].seq;
    info->pages[count].swap_slot = -1;
    count++;
  }

  for (int i = 0; i < SWAP_MAX_PAGES && count < MAX_PAGES_INFO; i++) {
    if (p->swap_slots[i].used) {
      info->pages[count].va = p->swap_slots[i].va;
      info->pages[count].state = SWAPPED;
      info->pages[count].is_dirty = 1;
      info->pages[count].seq = -1; // optional
      info->pages[count].swap_slot = i;
      count++;
    }
  }
}
