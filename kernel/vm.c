#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S




/*
 * create a direct-map page table for the kernel.
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64 kvmpa(uint64 va) {
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0) return -1;
    if (*pte & PTE_V) panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V) panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  char *mem;
  uint64 a;

  if (newsz < oldsz) return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  /*uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len) n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;*/
  return copyin_new(pagetable,dst,srcva,len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  /*uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max) n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
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
  if (got_null) {
    return 0;
  } else {
    return -1;
  }*/
  return copyinstr_new(pagetable,dst,srcva,max);
}

// check if use global kpgtbl or not
int test_pagetable() {
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(kernel_pagetable);
  printf("test_pagetable: %d\n", satp != gsatp);
  return satp != gsatp;
}



//递归调用的打印函数
void vmp(pagetable_t pgtbl,int level,uint64 va_base) {
  char flags[4];
  for(int i=0;i<512;i++){// 遍历当前页表页中的所有PTE表项
    pte_t pte = pgtbl[i]; //获取第i条PTE 
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){ //页表项有效&不具有R/W/X权限，说明不是叶子映射
      flags[0]=(pte&PTE_R)?'r':'-';
      flags[1]=(pte&PTE_W)?'w':'-';
      flags[2]=(pte&PTE_X)?'x':'-';
      flags[3]=(pte&PTE_U)?'u':'-';
      uint64 pa=PTE2PA(pte);
      uint64 next_va_base=va_base+((uint64)i << PXSHIFT(2 - level));
      printf("||");
      for(int j=0;j<level;j++) printf("   ||");
      printf("idx: %d: pa: %p, flags: %s\n",i,pa,flags);
      vmp((pagetable_t)pa,level+1,next_va_base);
    }
    else if(pte & PTE_V){// 如果PTE有效且具有R/W/X权限（叶子节点
      flags[0]=(pte&PTE_R)?'r':'-';
      flags[1]=(pte&PTE_W)?'w':'-';
      flags[2]=(pte&PTE_X)?'x':'-';
      flags[3]=(pte&PTE_U)?'u':'-';
      uint64 pa=PTE2PA(pte);
      uint64 va=va_base+((uint64)i << PXSHIFT(2 - level));
      printf("||");
      for(int j=0;j<level;j++) printf("   ||");
      printf("idx: %d: va: %p -> pa: %p, flags: %s\n",i,va,pa,flags);
    }
  }
}
void vmprint(pagetable_t pgtbl) {
  printf("page table %p\n",pgtbl);
  vmp(pgtbl,0,0);
}


//创建独立内核页表,不映射CLINT,分配失败返回0
pagetable_t proc_kpagetable(void) {
  pagetable_t kpagetable=(pagetable_t)kalloc();
  memset(kpagetable,0,PGSIZE);//所有页表项初始为0

  // uart registers
  if(mappages(kpagetable, UART0, PGSIZE, UART0, PTE_R | PTE_W) != 0){
    kfree((void*)kpagetable);
    return 0;
  }

  // virtio mmio disk interface
  if(mappages(kpagetable, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W) != 0){
    kfree((void*)kpagetable);
    return 0;
  }

  // PLIC
  if(mappages(kpagetable, PLIC, 0x400000, PLIC, PTE_R | PTE_W) != 0){
    kfree((void*)kpagetable);
    return 0;
  }

  // map kernel text executable and read-only.
  if(mappages(kpagetable, KERNBASE, (uint64)etext - KERNBASE, KERNBASE, PTE_R | PTE_X) != 0){
    kfree((void*)kpagetable);
    return 0;
  }

  // map kernel data and the physical RAM we'll make use of.
  if(mappages(kpagetable, (uint64)etext, PHYSTOP - (uint64)etext, (uint64)etext, PTE_R | PTE_W) != 0){
    kfree((void*)kpagetable);
    return 0;
  }

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  if(mappages(kpagetable,TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) != 0){
    kfree((void*)kpagetable);
    return 0;
  }

  return kpagetable;
}

/*//将用户页表的整个映射复制到内核页表
void sync_pagetable(pagetable_t kpagetable,pagetable_t upagetable,uint64 size){
  pte_t* pte;
  for(uint64 i=0;i<size;i+=PGSIZE){
    pte=walk(upagetable,i,0);//遍历用户页表
    if(pte==0||!(*pte&PTE_V)) continue;//页表不存在或无效
    if(i>=TRAPFRAME||(walk(kpagetable,i,0)!=0&&(*pte&PTE_V))) continue;//内核页表该区域已被分配
    uint64 pa=PTE2PA(*pte);
    if(mappages(kpagetable,i,PGSIZE,pa,PTE_FLAGS(*pte)&~PTE_U)!=0){//建立内核页表的映射，将用户标志位置0
      return;
    }
  }
}*/

int sync_pagetable(pagetable_t kpagetable, pagetable_t upagetable, uint64 sz, uint64 sz_n) {
  pte_t* pte;
  uint64 pa, i;
  uint flags;
  uint64 start_sz = PGROUNDUP(sz);
  for (i = start_sz; i < sz_n; i += PGSIZE) {
    // 跳过 TRAMPOLINE，因为它已经在内核页表中了（proc_kpagetable 中已映射）
    if (i == TRAMPOLINE) continue;
    
    if ((pte = walk(upagetable, i, 0)) == 0) {
      continue; // 跳过未映射的页面
    }
    if ((*pte & PTE_V) == 0) {
      continue; // 跳过无效的页面
    }
    
    // 检查内核页表中是否已经存在映射（除了 TRAMPOLINE）
    pte_t *kpte = walk(kpagetable, i, 0);
    if (kpte != 0 && (*kpte & PTE_V)) {
      continue; // 已经映射，跳过以避免重复映射
    }
    
    pa = PTE2PA(*pte);
    // 允许内存访问
    flags = PTE_FLAGS(*pte) & (~PTE_U); // 移除用户标志位
    if (mappages(kpagetable, i, PGSIZE, (uint64)pa, flags) != 0) { // 创建页表项
      // 移除映射：只移除从 start_sz 到 i 之间已经成功映射的页面
      if (i > start_sz) {
        for (uint64 j = start_sz; j < i; j += PGSIZE) {
          if (j == TRAMPOLINE) continue;
          kpte = walk(kpagetable, j, 0);
          if (kpte != 0 && (*kpte & PTE_V)) {
            *kpte = 0;
          }
        }
      }
      return -1;
    }
  }
  // 单独处理 TRAPFRAME，确保它被同步到内核页表中（内核需要访问它）
  if ((pte = walk(upagetable, TRAPFRAME, 0)) != 0 && (*pte & PTE_V)) {
    pte_t *kpte = walk(kpagetable, TRAPFRAME, 0);
    if (kpte == 0 || (*kpte & PTE_V) == 0) {
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte) & (~PTE_U);
      if (mappages(kpagetable, TRAPFRAME, PGSIZE, pa, flags) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

uint64 uvmdealloc_u_in_k(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return newsz;
  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    uint64 start_va = PGROUNDUP(newsz);
    uint64 end_va = PGROUNDUP(oldsz);
    // 安全地取消映射，跳过未映射的页面
    for (uint64 a = start_va; a < end_va; a += PGSIZE) {
      pte_t *pte = walk(pagetable, a, 0);
      if (pte != 0 && (*pte & PTE_V)) {
        *pte = 0; // 只清除PTE，不释放物理内存
      }
    }
  }
  return newsz;
}
