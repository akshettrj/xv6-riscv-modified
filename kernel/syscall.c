#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz)
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  int err = copyinstr(p->pagetable, buf, addr, max);
  if(err < 0)
    return err;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
int
argint(int n, int *ip)
{
  *ip = argraw(n);
  return 0;
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
int
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  return 0;
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  if(argaddr(n, &addr) < 0)
    return -1;
  return fetchstr(addr, buf, max);
}

extern uint64 sys_chdir(void);
extern uint64 sys_close(void);
extern uint64 sys_dup(void);
extern uint64 sys_exec(void);
extern uint64 sys_exit(void);
extern uint64 sys_fork(void);
extern uint64 sys_fstat(void);
extern uint64 sys_getpid(void);
extern uint64 sys_kill(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_mknod(void);
extern uint64 sys_open(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_unlink(void);
extern uint64 sys_wait(void);
extern uint64 sys_waitx(void);
extern uint64 sys_write(void);
extern uint64 sys_uptime(void);
extern uint64 sys_trace(void);
extern uint64 sys_set_priority(void);

static uint64 (*syscalls[])(void) = {
[SYS_fork]           = sys_fork,
[SYS_exit]           = sys_exit,
[SYS_wait]           = sys_wait,
[SYS_waitx]           = sys_waitx,
[SYS_pipe]           = sys_pipe,
[SYS_read]           = sys_read,
[SYS_kill]           = sys_kill,
[SYS_exec]           = sys_exec,
[SYS_fstat]          = sys_fstat,
[SYS_chdir]          = sys_chdir,
[SYS_dup]            = sys_dup,
[SYS_getpid]         = sys_getpid,
[SYS_sbrk]           = sys_sbrk,
[SYS_sleep]          = sys_sleep,
[SYS_uptime]         = sys_uptime,
[SYS_open]           = sys_open,
[SYS_write]          = sys_write,
[SYS_mknod]          = sys_mknod,
[SYS_unlink]         = sys_unlink,
[SYS_link]           = sys_link,
[SYS_mkdir]          = sys_mkdir,
[SYS_close]          = sys_close,
[SYS_trace]          = sys_trace,
[SYS_set_priority]   = sys_set_priority,
};

static char* syscalls_names[] = {
[SYS_fork]           = "fork",
[SYS_exit]           = "exit",
[SYS_wait]           = "wait",
[SYS_waitx]          = "waitx",
[SYS_pipe]           = "pipe",
[SYS_read]           = "read",
[SYS_kill]           = "kill",
[SYS_exec]           = "exec",
[SYS_fstat]          = "fstat",
[SYS_chdir]          = "chdir",
[SYS_dup]            = "dup",
[SYS_getpid]         = "getpid",
[SYS_sbrk]           = "sbrk",
[SYS_sleep]          = "sleep",
[SYS_uptime]         = "uptime",
[SYS_open]           = "open",
[SYS_write]          = "write",
[SYS_mknod]          = "mknod",
[SYS_unlink]         = "unlink",
[SYS_link]           = "link",
[SYS_mkdir]          = "mkdir",
[SYS_close]          = "close",
[SYS_trace]          = "trace",
[SYS_set_priority]   = "set_priority",
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    int tm = p->tracemask;

    int to_trace = 0;
    int arg1, arg2, arg3;

    if (num == SYS_trace) {
      argint(0, &arg1);
      if (((1 << SYS_trace) & arg1) != 0)
        to_trace = 1;
    }
    else if (((1 << num) & tm) != 0)
      to_trace = 1;

    if (to_trace == 1) {
      switch (num) {

        case SYS_getpid:
        case SYS_fork:
        case SYS_uptime:
        break;

        case SYS_wait:
        case SYS_pipe:
        case SYS_chdir:
        case SYS_sleep:
        case SYS_unlink:
        case SYS_dup:
        case SYS_mkdir:
        case SYS_trace:
        case SYS_kill:
        case SYS_close:
        case SYS_sbrk:
        argint(0, &arg1);
        break;

        case SYS_fstat:
        case SYS_exec:
        case SYS_link:
        case SYS_open:
        case SYS_set_priority:
        argint(0, &arg1);
        argint(1, &arg2);
        break;

        case SYS_write:
        case SYS_waitx:
        case SYS_read:
        case SYS_mknod:
        argint(0, &arg1);
        argint(1, &arg2);
        argint(2, &arg3);
        break;

        default:
        break;
      }
    }

    p->trapframe->a0 = syscalls[num]();

    if (to_trace == 1) {
      printf("%d: syscall %s (", p->pid, syscalls_names[num]);

      switch (num) {

        case SYS_getpid:
        case SYS_fork:
        case SYS_uptime:
        break;

        case SYS_wait:
        case SYS_pipe:
        case SYS_chdir:
        case SYS_sleep:
        case SYS_unlink:
        case SYS_dup:
        case SYS_mkdir:
        case SYS_trace:
        case SYS_kill:
        case SYS_close:
        case SYS_sbrk:
        printf("%d", arg1);
        break;

        case SYS_fstat:
        case SYS_exec:
        case SYS_link:
        case SYS_open:
        case SYS_set_priority:
        printf("%d %d", arg1, arg2);
        break;

        case SYS_write:
        case SYS_read:
        case SYS_waitx:
        case SYS_mknod:
        printf("%d %d %d", arg1, arg2, arg3);
        break;

        default:
        break;
      }
      printf(") -> %d\n", p->trapframe->a0);
    }

  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
