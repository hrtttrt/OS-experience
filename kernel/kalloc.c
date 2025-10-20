// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define EACHKMEM (PHYSTOP-(uint64)end)/NCPU

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem{
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];//每个CPU拥有独立的freelist


void
kinit()
{
  for(int i=0;i<NCPU;++i) initlock(&kmems[i].lock, "kmem");//给每个kmem单独初始化锁
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  push_off();
  int index = cpuid();
  pop_off();

  acquire(&kmems[index].lock);
  r->next = kmems[index].freelist;
  kmems[index].freelist = r;
  release(&kmems[index].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int index = cpuid();
  pop_off();

  acquire(&kmems[index].lock);
  r = kmems[index].freelist;

  if(r){
    kmems[index].freelist = r->next;
    release(&kmems[index].lock);
  }
  else {
    release(&kmems[index].lock);//先解锁当前kmem
    for(int i=0;i<NCPU;++i){
      if(i==index) continue;//跳过当前空间已满CPU
      acquire(&kmems[i].lock);
      r=kmems[i].freelist;
      if(r){//尝试从别的CPUfreelist偷取内存
        kmems[i].freelist=r->next;
        release(&kmems[i].lock);
        break;
      }
      release(&kmems[i].lock);//没偷到也解锁，防止死锁
    }
  }
  

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
