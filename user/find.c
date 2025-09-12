#include "kernel/types.h"
#include "kernel/stat.h"
#include "user.h"
#include "kernel/fs.h"
char *fmtname(char *path) {//获取文件名
  static char buf[DIRSIZ + 1];
  char *p;

  // Find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if (strlen(p) >= DIRSIZ) return p;
  memmove(buf, p, strlen(p));
  buf[strlen(p)]=0;//保证以\0结尾
  return buf;
}

void searchfile(char* path,char* filename)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0) {
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }
    //比对当前对象名与目标文件名
      if(strcmp(filename,fmtname(path))==0) printf("%s\n",path);

    //若当前对象为目录，递归搜索
    if(st.type==T_DIR)
    {
        if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
        printf("ls: path too long\n");//目录过长
      }

      //准备将当前目录加入搜索目录
      strcpy(buf, path);
      p = buf + strlen(buf);
      if (*(p-1)!='/') *p++ = '/';

      while(read(fd,&de,sizeof(de))==sizeof(de))
      {
        //跳过无效条目、当前目录与父目录
        if(de.inum==0||strcmp(de.name,".")==0||strcmp(de.name,"..")==0) continue;
        memmove(p,de.name,strlen(de.name));
        p[strlen(de.name)]=0;
        searchfile(buf,filename);
      }
    }
  close(fd);
}

int main(int argc,char* argv[]){
    if(argc != 3)
    {
        printf("more or less argument!\n");
        exit(-1);
    }
    searchfile(argv[1],argv[2]);
    exit(0);
}