// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13
#define MAPSIZE 31
struct spinlock maplocks[MAPSIZE];//全局分配锁，防止重复缓存同块
//当前桶未找到buf将进入分配时，根据blockno获得该锁后全局搜索
//若无该锁，可能会出现不同buf被打上同一块号导致释放时出现freeing free block
//若只加一把锁，又会像原方法一样出现剧烈锁争用

struct hashbucket{ //每个哈希队列一个linked list及一个lock
  struct spinlock lock;
  struct buf head;
};

struct {
  struct buf buf[NBUF];
  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.
  //struct buf head;
  struct hashbucket hashbucket[NBUCKETS];
}bcache;

int HASH(uint blockno) {return (int)blockno%NBUCKETS;}

void
binit(void)
{
  struct buf *b;
  for(int i=0;i<NBUCKETS;i++){//初始化各哈希桶
    initlock(&bcache.hashbucket[i].lock, "bcache");
    bcache.hashbucket[i].head.prev = &bcache.hashbucket[i].head;
    bcache.hashbucket[i].head.next = &bcache.hashbucket[i].head;
  }

  // 初始化 maplocks
  for (int m = 0; m < MAPSIZE; m++)
    initlock(&maplocks[m], "maplock");

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){//初始化全部buf，先全部加入第0个哈希桶
    b->dev = (uint)-1;
    b->blockno = 0;
    b->refcnt = 0;
    b->valid = 0;

    b->next = bcache.hashbucket[0].head.next;
    b->prev = &bcache.hashbucket[0].head;
    initsleeplock(&b->lock, "buffer");
    bcache.hashbucket[0].head.next->prev = b;
    bcache.hashbucket[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int index = HASH(blockno);
  int m = blockno % MAPSIZE;

  
  acquire(&bcache.hashbucket[index].lock);
  for (b = bcache.hashbucket[index].head.next;b != &bcache.hashbucket[index].head;b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.hashbucket[index].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.hashbucket[index].lock);//本桶未找到，先解锁

  //获得对应maplocks
  acquire(&maplocks[m]);

  //先全局搜索，有就返回，防止两块buf被赋值相同的blockno
  for (int i = 0; i < NBUCKETS; i++) {
    acquire(&bcache.hashbucket[i].lock);
    for (b = bcache.hashbucket[i].head.next;
         b != &bcache.hashbucket[i].head;
         b = b->next) {
      if (b->dev == dev && b->blockno == blockno) {
        b->refcnt++;
        release(&bcache.hashbucket[i].lock);
        release(&maplocks[m]);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.hashbucket[i].lock);
  }

  //全局搜索找不到，进入分配流程
  for (int i = 0; i < NBUCKETS; i++) {
    //先获得CPU号较小的锁，防止循环等待死锁
    int first = (i < index) ? i : index;
    int second = (i < index) ? index : i;

    //以固定顺序获取全部桶的锁
    acquire(&bcache.hashbucket[first].lock);
    if (second != first) acquire(&bcache.hashbucket[second].lock);

    //继承原LRU方法搜索
    struct buf *p;
    for (p = bcache.hashbucket[i].head.prev;p != &bcache.hashbucket[i].head;p = p->prev) {
      if (p->refcnt == 0) {
        
        p->prev->next = p->next;
        p->next->prev = p->prev;

        
        p->next = bcache.hashbucket[index].head.next;
        p->prev = &bcache.hashbucket[index].head;
        bcache.hashbucket[index].head.next->prev = p;
        bcache.hashbucket[index].head.next = p;

        
        p->dev = dev;
        p->blockno = blockno;
        p->valid = 0;
        p->refcnt = 1;

        //释放所有锁
        if (second != first) release(&bcache.hashbucket[second].lock);
        release(&bcache.hashbucket[first].lock);
        release(&maplocks[m]);

        acquiresleep(&p->lock);
        return p;
      }
    }

    //未找到可分配的buf，也释放原桶锁和新桶锁
    if (second != first) release(&bcache.hashbucket[second].lock);
    release(&bcache.hashbucket[first].lock);
  }

  // no buffer available
  release(&maplocks[m]);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int index=HASH(b->blockno);
  acquire(&bcache.hashbucket[index].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.hashbucket[index].head.next;
    b->prev = &bcache.hashbucket[index].head;
    bcache.hashbucket[index].head.next->prev = b;
    bcache.hashbucket[index].head.next = b;
  }
  
  release(&bcache.hashbucket[index].lock);
}

void
bpin(struct buf *b) {
  int index=HASH(b->blockno);
  acquire(&bcache.hashbucket[index].lock);
  b->refcnt++;
  release(&bcache.hashbucket[index].lock);
}

void
bunpin(struct buf *b) {
  int index=HASH(b->blockno);
  acquire(&bcache.hashbucket[index].lock);
  b->refcnt--;
  release(&bcache.hashbucket[index].lock);
}


