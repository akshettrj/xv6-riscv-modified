#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

struct Queue proc_queue[NUM_OF_QUEUES];

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{

#if SCHEDULER == S_RR
  printf("Round Robin Scheduler\n");
#elif SCHEDULER == S_FCFS
  printf("First Come First Serve Scheduler\n");
#elif SCHEDULER == S_PBS
  printf("Priority Based Scheduler\n");
#elif SCHEDULER == S_MLFQ
  printf("MLFQ Scheduler\n");
#endif

  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
#if SCHEDULER == S_MLFQ
  for (int i=0; i<NUM_OF_QUEUES; i++) {
    for (int j=0; j<NPROC; j++) {
      proc_queue[i].proc_arr[j] = 0;
    }
    proc_queue[i].count = 0;
  }
#endif
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->rtime = 0;
  p->pbsrtime = 0;
  p->etime = 0;
  p->stime = 0;
  p->ctime = ticks;

  p->tracemask = 0;
  p->static_priority = 60;
  p->niceness = 5;
  p->scount = 0;
  p->qnum = -1;
  for (int i=0; i<NUM_OF_QUEUES; i++) {
    p->qwtimes[i] = 0;
    p->qrtimes[i] = 0;
  }
  p->cqwtime = 0;
  p->cqrtime = 0;
  p->has_over_shoot = 0;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->tracemask = 0;
  p->ctime = 0;
  p->stime = 0;
  p->rtime = 0;
  p->pbsrtime = 0;
  p->etime = 0;
  p->scount = 0;
  p->static_priority = 0;
  p->niceness = 0;
  p->qnum = 0;
  for (int i=0; i<NUM_OF_QUEUES; i++) {
    p->qwtimes[i] = 0;
    p->qrtimes[i] = 0;
  }
  p->cqwtime = 0;
  p->cqrtime = 0;
  p->has_over_shoot = 0;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

#if SCHEDULER == S_MLFQ
  add_to_proc_queue(p, 0);
#endif

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

#if SCHEDULER == S_MLFQ
  add_to_proc_queue(np, 0);
  if (p!=0 && np->qnum > 0) {
    yield();
  }
#endif

  release(&np->lock);

  np->tracemask = p->tracemask;

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->etime = ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
waitx(uint64 addr, uint *rtime, uint *wtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

void
update_time()
{
  struct proc* p;
  for (p = proc; p<&proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->rtime ++;
      p->pbsrtime++;
#if SCHEDULER == S_MLFQ
      p->cqrtime++;
#endif
    }
    else if (p->state == SLEEPING) {
      p->stime ++;
    }
#if SCHEDULER == S_MLFQ
    if (p->state == RUNNABLE) {
      p->qwtimes[p->qnum]++;
      p->cqwtime ++;
    }
    if (p->state != ZOMBIE) {
      p->qrtimes[p->qnum]++;
    }
#endif
    release(&p->lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

#if SCHEDULER == S_RR

    // Default Round Robin Scheduling
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }

#elif SCHEDULER == S_FCFS

    // FCFS Scheduling
    // Process that was created the first
    struct proc *oldest_process = 0;

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        if (oldest_process == 0 || p->ctime < oldest_process->ctime) {
          oldest_process == 0 ? 0 : release(&oldest_process->lock);
          oldest_process = p;
        }
        else
          release(&p->lock);
      }
      else
        release(&p->lock);

    }

    if (oldest_process == 0)
      continue;

    // acquire(&oldest_process->lock);
    // printf("RUNNING PROC with pid: %d, START TIME: %d\n", oldest_process->pid, oldest_process->ctime);
    oldest_process->state = RUNNING;
    c->proc = oldest_process;
    swtch(&c->context, &oldest_process->context);
    c->proc = 0;
    release(&oldest_process->lock);

#elif SCHEDULER == S_PBS
    // Priority Based Scheduling

    struct proc *highest_priority_proc = 0;
    int curr_priority = 0;

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      /* When to update?
       * If the highest_priority_proc is 0.
       * If the current priority is lower than p ki.
       */
      if (p->state == RUNNABLE) {
        int p_priority = 0;
        // p_priority = max(0, min(SP - niceness + 5, 100))
        p_priority = p->static_priority - p->niceness + 5;
        p_priority = (p_priority > 100 ? 100 : p_priority);
        p_priority = (p_priority <  0  ?  0  : p_priority);

        if (highest_priority_proc == 0) {
          highest_priority_proc = p;
          curr_priority = p_priority;
          continue;
        }

        if (p_priority > curr_priority) {
          release(&p->lock);
          continue;
        }

        if (p_priority < curr_priority) {
          release(&highest_priority_proc->lock);
          highest_priority_proc = p;
          curr_priority = p_priority;
          continue;
        }

        if (p->scount < highest_priority_proc->scount) {
          release(&p->lock);
          continue;
        }

        if (p->scount > highest_priority_proc->scount) {
          release(&highest_priority_proc->lock);
          highest_priority_proc = p;
          curr_priority = p_priority;
          continue;
        }

        if (p->ctime < highest_priority_proc->ctime) {
          release(&highest_priority_proc->lock);
          highest_priority_proc = p;
          curr_priority = p_priority;
          continue;
        }

        release(&p->lock);

      }
      else
        release(&p->lock);

    }

    if (highest_priority_proc == 0)
      continue;

    // acquire(&highest_priority_proc->lock);
    if (highest_priority_proc->state == RUNNABLE)
    {
      highest_priority_proc->scount += 1;
      highest_priority_proc->state = RUNNING;
      c->proc = highest_priority_proc;
      swtch(&c->context, &highest_priority_proc->context);
      c->proc = 0;
      if (highest_priority_proc->pbsrtime + highest_priority_proc->stime != 0)
        highest_priority_proc->niceness = (int)((10*(highest_priority_proc->stime))/(highest_priority_proc->pbsrtime + highest_priority_proc->stime));
      // printf("RAN PROC with pid: %d, RUN TIME: %d, SLEEP TIME: %d, NICENESS: %d\n", highest_priority_proc->pid, highest_priority_proc->pbsrtime, highest_priority_proc->stime, highest_priority_proc->niceness);
    }
    release(&highest_priority_proc->lock);


#elif SCHEDULER == S_MLFQ
    // MLFQ Scheduler

    // 1. Find the first non-empty queue with highest priority
    // 2. Start the process present in it.
    // 3. Check for overshooting
    // 4. Check for starvation

    // Check for starvation
    for (int qnum=1; qnum < NUM_OF_QUEUES; qnum++) {
      struct Queue *cqueue = &proc_queue[qnum];
      for (int pnum=0; pnum < cqueue->count; pnum++) {
        p = cqueue->proc_arr[pnum];
        acquire(&p->lock);
        if (p->cqwtime >= STARVATION_TICKS_LIMIT) {
          remove_from_proc_queue(p, qnum);
          add_to_proc_queue(p, qnum-1);
          pnum--;
        }
        release(&p->lock);
      }
    }

    // Find and run the next program
    int proc_found = 0;
    for (int qnum=0; qnum < NUM_OF_QUEUES; qnum++) {
      if (proc_found == 1) break;

      struct Queue *cqueue = &proc_queue[qnum];
      for (int pnum=0; pnum < cqueue->count; pnum++) {
        if (proc_found == 1) break;

        p = cqueue->proc_arr[pnum];
        acquire(&p->lock);
        if (p->state == RUNNABLE) {
          // Round Robin for last queue
          // if (qnum != (NUM_OF_QUEUES-1))
          proc_found = 1;

          //printf("Found process %s (pid=%d) from queue (num=%d) to be runnable\n", p->name, p->pid, p->qnum);

          p->state = RUNNING;
          c->proc = p;
          remove_from_proc_queue(p, qnum);
          swtch(&c->context, &p->context);
          c->proc = 0;

          //printf("Returned from process\n");

          // Check overshoot or completion
          if (p->has_over_shoot == 1) {
            if (qnum != (NUM_OF_QUEUES-1)) {
              add_to_proc_queue(p, qnum+1);
            }
            else
              add_to_proc_queue(p, qnum);
          }
          else if (p->state == RUNNABLE){
            add_to_proc_queue(p, qnum);
          }
        }
        release(&p->lock);
      }
    }

#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
#if SCHEDULER == S_MLFQ
        add_to_proc_queue(p, p->qnum);
#endif
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    = "unused  ",
  [SLEEPING]  = "sleeping",
  [RUNNABLE]  = "runnable",
  [RUNNING]   = "running ",
  [ZOMBIE]    = "zombie  "
  };
  struct proc *p;
  char *state;

  printf("\n");
#if SCHEDULER == S_PBS
  // For PBS
  printf("PID\tPriority\tState\t\trtime\twtime\tnrun\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    int wtime = ticks - p->ctime - p->rtime;
    int priority = 0;
    priority = p->static_priority - p->niceness + 5;
    priority = (priority > 100 ? 100 : priority);
    priority = (priority <  0  ?  0  : priority);
    printf("%d\t%d\t\t%s\t  %d\t%d\t%d", p->pid, priority, state, p->rtime, wtime, p->scount);
    printf("\n");
  }
#elif SCHEDULER == S_MLFQ
  // For MLFQ
  printf("PID\tPriority\tState\t\trtime\twtime\tnrun\tq0\tq1\tq2\tq3\tq4\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    if (p->state == ZOMBIE)
      printf("%d\t%d\t\t%s\t  %d\t%d\t%d", p->pid, -1, state, p->rtime, p->cqwtime, p->scount);
    else
      printf("%d\t%d\t\t%s\t  %d\t%d\t%d", p->pid, p->qnum, state, p->rtime, p->cqwtime, p->scount);
    for (int qnum=0; qnum<NUM_OF_QUEUES; qnum++) {
      printf("\t%d", p->qrtimes[qnum]);
    }
    printf("\n");
  }
#else
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
#endif
}

int
trace(int tracemask)
{
  struct proc *p = myproc();

  p->tracemask = tracemask;

  return 0;
}

int
set_priority(int new_priority, int pid)
{
  if (new_priority < 0 || new_priority > 100)
    return -1;

  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){

      int old_priority = p->static_priority + p->niceness + 5;
      old_priority = (old_priority > 100 ? 100 : old_priority);
      old_priority = (old_priority <  0  ?  0  : old_priority);

      p->static_priority = new_priority;

      // Reset values
      p->niceness = 5;
      p->pbsrtime = 0;
      p->stime = 0;

      release(&p->lock);
      if (new_priority < old_priority) {
        yield();
      }
      return old_priority;
    }
    release(&p->lock);
  }
  return -1;
}

#if SCHEDULER == S_MLFQ
// Add the process with pid p->pid to the
// process queue number qnum
void
add_to_proc_queue(struct proc* p, int qnum)
{
  if (qnum < 0 || qnum >= NUM_OF_QUEUES)
    return;

  // printf("Added proc (pid=%d) to queue (num=%d)\n", p->pid, qnum);

  p->qnum = qnum;
  proc_queue[qnum].proc_arr[proc_queue[qnum].count] = p;
  proc_queue[qnum].count ++;
  p->cqwtime = 0;
  p->cqrtime = 0;
  p->has_over_shoot = 0;
}

void
remove_from_proc_queue(struct proc* p, int qnum) {
  if (qnum < 0 || qnum >= NUM_OF_QUEUES)
    return;

  // printf("Removed proc (pid=%d) from queue (num=%d)\n", p->pid, qnum);

  struct Queue *q = &proc_queue[qnum];
  for (int i=0; i<q->count; i++) {
    if (q->proc_arr[i] == p) {
      /*
       * p->qnum = -1;
       * p->cqwtime = 0;
       * p->cqrtime = 0;
       */

      for (int j=i+1; j<q->count; j++) {
        q->proc_arr[j-1] = q->proc_arr[j];
        q->proc_arr[j] = 0;
      }

      q->count--;
      break;
    }
  }
}
#endif
