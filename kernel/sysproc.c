#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  if (argaddr(0, &p) < 0) return -1;

  int flag;
  if(argint(1,&flag)<0) return -1;//得到寄存器中存储的第二个int型参数，即用户级wait()输入的flag
  return wait(p,flag);
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

uint64 sys_yield(void) {
  struct proc *p=myproc();
  struct proc *pp=myproc();
  acquire(&p->lock);
  for(pp=proc;pp<&proc[NPROC];pp++)//从头遍历进程表
  {
    if(pp->state==RUNNABLE&&pp!=p) break;
  }
  release(&p->lock);
  printf("Save the context of the process to the memory region from address %p to %p\n",&(p->context), &(p->context)+1);
  printf("Current running process pid is %d and user pc is %p\n", p->pid, p->trapframe->epc);//trapframe->epc存储对应进程的PC
  printf("Next runnable process pid is %d and user pc is %p\n", pp->pid, pp->trapframe->epc);
  yield();
  return 0;
}