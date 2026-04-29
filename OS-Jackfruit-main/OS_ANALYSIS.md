# OS Fundamentals Analysis: Multi-Container Runtime

This document explains the OS concepts underlying the OS-Jackfruit container runtime and how the implementation exercises these mechanisms.

---

## 1. Isolation Mechanisms

### 1.1 Namespace-Based Process Isolation

**What are Namespaces?**

Namespaces are kernel-level virtualization mechanisms that partition kernel resources so processes in different namespaces see different resource views. Think of them as separate virtual "worlds" isolated at the kernel level.

**Our Implementation:**

```c
child_pid = clone(child_fn, stack_mem + STACK_SIZE,
                  CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                  &child_cfg);
```

We create four separate isolation boundaries:

**PID Namespace (`CLONE_NEWPID`):**
- Container sees itself as PID 1 (like a separate Linux system)
- `/bin/sh` inside container runs as PID 1 (not the host PID)
- Container cannot see or signal processes outside its namespace
- Each namespace has its own process tree rooted at PID 1
- Enables: Process isolation, clean init delegation, independent signal handling

**UTS Namespace (`CLONE_NEWUTS`):**
- Container has isolated hostname/domainname (via `uname`)
- Can run `hostname mycontainer` without affecting host
- Applications see their own hostname, enabling per-container identification
- Not security-critical but improves container "feel"

**Mount Namespace (`CLONE_NEWNS`):**
- Container has isolated mount table (/etc/mtab, mount points)
- Allows `chroot` to work: container sees only its rootfs
- Container's `/proc` can be mounted independently
- Prevents container mounts from appearing on host
- Enables: Complete filesystem isolation, mount table independence

**Why 4 separate namespaces, not 1?**
- Different isolation concerns require different mechanisms
- Partial isolation useful: two containers might share network namespace but not filesystem
- Linux kernel design: compose isolation as needed
- Our choice: maximum isolation (good for security labs)

### 1.2 Filesystem Isolation with `chroot` and `pivot_root`

**Our Approach: `chroot`**

```c
if (chdir(cfg->rootfs) != 0) return 1;
if (chroot(".") != 0) return 1;
if (chdir("/") != 0) return 1;
```

**What `chroot` Does:**
- Changes process's root filesystem to a specified directory
- All path lookups start from this new root (instead of `/`)
- Process calling `chroot` cannot traverse above its new root with `..`
- Makes the provided rootfs appear as `/` to the container

**Security Properties:**
- Container cannot access `/etc/passwd` on host anymore
- Container's `/lib` points to `container-rootfs/lib`
- `chroot` implementation note: older, but simpler than `pivot_root`

**What Linux Kernel Shares with Containers?**
- Kernel itself (processes run Linux kernel code)
- Hardware (same CPU, memory, I/O devices)
- Kernel objects not namespaced:
  - User/group IDs (without `CLONE_NEWUSER`)
  - IPC objects without `CLONE_NEWIPC`
  - Network without `CLONE_NEWNET`
  - Device files (unless mount namespace prevents it)

**This Is Not Perfect Isolation!**
- Without `CLONE_NEWUSER`: root in container is still UID 0 (kernel knows)
- Without networking isolation: containers can communicate
- Kernel exploits could still escape to host
- This is why real container runtimes (Docker) add more features:
  - cgroups for resource limits
  - seccomp/AppArmor for syscall filtering
  - User namespace remapping (UID 0 in container → UID 1000 on host)

### 1.3 /proc Filesystem Isolation

**The Problem:**
After `chroot`, container still has no `/proc` filesystem unless we mount it:
```c
mount("proc", "/proc", "proc", 0, NULL);
```

**Why This Matters:**
- `ps` command reads `/proc/*/stat` files
- `top` needs `/proc/stat` for CPU usage
- Container's `ps` was showing host's processes (wrong!)
- After mounting, `ps` shows only container's processes

**Key Insight:**
- `/proc` is a virtual filesystem (procfs) created by kernel
- Each namespace gets its own procfs view
- `mount("proc", ...)` creates filesystem at `/proc` point
- The kernel automatically filters `/proc` contents per PID namespace

---

## 2. Supervisor and Process Lifecycle

### 2.1 Parent-Child Relationships

**Traditional Single-Process Model:**
```
Parent (shell) → calls exec(container_program)
                 Process terminates, parent shell continues
```

**Our Supervisor Model:**
```
Supervisor (parent)
├─ Container 1 (child)
├─ Container 2 (child)
├─ Container 3 (child)
└─ Logger Thread (same process, different thread)

When child exits: parent (supervisor) still alive, can manage other children
```

**Why This Architecture?**
- Traditional model: one container per shell invocation
- Our model: one supervisor → many containers
- Supervisor can: coordinate, collect logs, manage lifecycle, respond to commands
- Enables: true multi-container orchestration (like Docker daemon)

### 2.2 Child Process Reaping

**The Zombie Problem:**
When a child process exits, its parent must call `waitpid()` to collect its exit status. Until then, the child remains a "zombie" process:
- Takes up PID entry in kernel
- Exit status stored in kernel process table
- Shows as `<defunct>` in `ps` output
- Indicates resource leak if many accumulate

**Our Implementation:**
```c
while ((reaped_pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
    /* Process child exit, update metadata */
    container_record_t *rec = find_container_by_pid(&ctx, reaped_pid);
    if (rec) {
        if (WIFEXITED(wstatus)) {
            rec->exit_code = WEXITSTATUS(wstatus);
            rec->state = CONTAINER_EXITED;
        } else if (WIFSIGNALED(wstatus)) {
            rec->exit_signal = WTERMSIG(wstatus);
            rec->state = CONTAINER_KILLED;
        }
    }
}
```

**WNOHANG Flag:**
- `WNOHANG`: "don't wait if no child exited"
- Supervisor doesn't block waiting for child death
- Allows supervisor to service other clients while children run
- Combined with timeout in main loop: responsive but not busy-waiting

**Signal Flow:**
```
Container exits → Kernel sends SIGCHLD to supervisor
Supervisor's SIGCHLD handler runs → Sets a flag → Main loop calls waitpid()
Main loop reaps child, updates metadata → Updates container_record_t
```

### 2.3 Process Substitution in Containers

**When `child_fn` calls `execvp`:**
```c
char *shell_argv[] = {(char *)"/bin/sh", (char *)"-c", cfg->command, NULL};
execvp("/bin/sh", shell_argv);
```

**What `exec` Does (kernel perspective):**
1. Finds `/bin/sh` (inside the chrooted rootfs)
2. Replaces entire address space of calling process with new program
3. PID stays the same (still the same process, different code/data)
4. Namespaces intact (PID is still inside the isolated namespace)
5. Signal dispositions inherited (SIGCHLD still causes shutdown)

**This Is Key:**
- Container process ID doesn't change
- Process remains in the isolated namespace
- From container's perspective: it's now running `sh` with PID 1
- From supervisor's perspective: child's PID unchanged, but now running `sh -c <command>`

### 2.4 Exit Status Encoding

**POSIX Convention:**
```c
if (WIFEXITED(wstatus)) {
    int exit_code = WEXITSTATUS(wstatus);  // 0-255
} else if (WIFSIGNALED(wstatus)) {
    int signal = WTERMSIG(wstatus);         // 1-31
    int core_dumped = WCOREDUMP(wstatus);   // boolean
}
```

**Our Termination Classification:**
```c
if (rec->exit_signal == SIGKILL && !rec->stop_requested) {
    rec->termination_reason = "hard_limit_killed";  /* Kernel monitor killed it */
} else if (rec->stop_requested) {
    rec->termination_reason = "stopped";  /* Supervisor stopped it */
} else if (WIFEXITED(wstatus)) {
    rec->termination_reason = "exited";  /* Normal exit */
}
```

**Why Distinguish These?**
- **Normal exit:** Expected behavior (code finished)
- **Stopped:** User requested stop via `engine stop` (graceful)
- **Killed:** Memory/resource limit triggered (enforcement)
- Different implications for debugging/monitoring

---

## 3. IPC, Threads, and Synchronization

### 3.1 Two IPC Mechanisms

**Path A (Logging): Pipes + Bounded Buffer**
```
Container process
    ↓ (file descriptor 1 = pipe write end)
Supervisor process (parent)
    ↓ (file descriptor 0 = pipe read end)
Pipe Reader Thread
    ↓ (push to bounded buffer)
Bounded Buffer (circular, protected by mutex)
    ↓ (pop from buffer)
Logger Thread
    ↓ (writes to log file)
Filesystem
```

**Path B (Control): UNIX Domain Socket**
```
CLI Process
    ↓ (SOCK_STREAM socket, AF_UNIX)
Supervisor Process
    ↓ (listening on /tmp/mini_runtime.sock)
Main Loop in Supervisor
    ↓ (reads control_request_t, writes control_response_t)
CLI Process
    ↓ (receives response, prints, exits)
```

**Why Two Different IPC Mechanisms?**
- **Logging (pipes):** Designed for continuous stream of data
  - Multiple producers (container processes) → one consumer (logger thread)
  - Kernel buffers data automatically
  - Integrated with file descriptor model
  
- **Control (socket):** Designed for request-response pattern
  - Multiple clients (CLI commands) send structured requests
  - Each client waits for response
  - Independent of logging pipeline

### 3.2 Race Conditions Without Synchronization

**Scenario 1: Bounded Buffer Without Mutex**
```
Thread A (Producer) pushes:        Thread B (Consumer) pops:
1. Read buffer->tail
                                   1. Read buffer->head
2. Write items[tail]
                                   2. Write items[head] 
3. Increment tail
                                   3. Increment head
4. Increment count
                                   4. Decrement count
```

**Problem:** If both read `count` at same time, both might think buffer empty/full. Items might be overwritten while being read.

**Scenario 2: Container List Without Mutex**
```
Supervisor thread (reaping):       SIGCHLD handler:
1. Find rec in list                1. Iterate through list
2. Examine rec->exit_code          2. Free rec, unlink from list
                                   3. Free memory
3. Try to access rec→exit_code     (Use-after-free!)
```

**Scenario 3: Monitor List Without Mutex**
```
Timer callback (softirq context):  ioctl handler (process context):
1. list_for_each_entry(entry...)
                                   1. Allocate new entry
                                   2. list_add(entry, &monitored_list)
                                   3. Unlock
2. Delete entry from list
3. kfree(entry)
4. Access entry->pid                (Use-after-free!)
```

### 3.3 Synchronization Choices

**Bounded Buffer: Mutex + Condition Variables**

Why not spinlock?
- Spinlock: busy-waits, disables interrupts (good for millisecond-scale locks)
- Our buffer: producer/consumer can hold lock for microseconds (waiting for I/O)
- Mutex: allows sleep, lets other threads run (proper for I/O synchronization)

```c
static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    
    /* If full, wait for consumer to drain */
    while (buffer->count >= LOG_BUFFER_CAPACITY) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;
        }
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
        /* Automatically releases mutex while waiting, reacquires on wakeup */
    }
    
    /* Insert and signal consumer */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}
```

**Container Metadata: Mutex**
```c
pthread_mutex_lock(&ctx->metadata_lock);
/* Update container state */
container_record_t *rec = find_container(&ctx, container_id);
rec->exit_code = exit_code;
rec->state = CONTAINER_EXITED;
pthread_mutex_unlock(&ctx->metadata_lock);
```

**Kernel Monitor List: Mutex (not spinlock)**

Why mutex even though kernel module?
- ioctl handlers run in process context (can sleep)
- Timer callback runs in softirq context (cannot sleep, but can use mutex)
- When mutex called from softirq: doesn't actually sleep (already holding CPU)
- When mutex called from process context: allows other processes to run while waiting

```c
/* In ioctl (process context) */
mutex_lock(&monitored_list_lock);  /* Can sleep if needed */
list_add(&entry->list, &monitored_list);
mutex_unlock(&monitored_list_lock);

/* In timer callback (softirq context, preemption disabled) */
mutex_lock(&monitored_list_lock);  /* Won't sleep; runs atomic */
list_for_each_entry(entry, &monitored_list, list) {
    /* Process entry */
}
mutex_unlock(&monitored_list_lock);
```

**Note on Spinlock Alternative:**
- If we used spinlock instead: would work correctly
- But spinlock blocks entire CPU (worse for unrelated processes)
- Mutex smarter: lets other processes use CPU while waiting
- Linux kernel philosophy: mutex for scheduler-involved synch, spinlock for brief critical sections

### 3.4 Metdata Structure Ownership

**Container Records:**
- Supervisor thread owns the list
- SIGCHLD handler just signals a flag
- Main loop (supervisor thread) actually calls `waitpid()` and updates records
- Only one thread ever modifies: no race possible!
- But mutex still needed: CLI threads read metadata for `ps` command

**Bounded Buffer:**
- Multiple producers (pipe reader threads) push items
- One consumer (logger thread) pops items
- Must serialize access: hence mutex + condition variables

**Kernel Monitored List:**
- ioctl handlers (many processes) add/remove entries
- Timer callback (one softirq handler) reads and modifies entries
- Must serialize: hence mutex protecting insert/remove/iteration

---

## 4. Memory Management and Enforcement

### 4.1 What Is RSS (Resident Set Size)?

**RSS: Memory Actually in RAM**

```
Virtual Address Space (4 GiB on 32-bit):
├─ Code segment (executable instructions)       → Some pages in RAM
├─ Data segment (global variables)              → Some pages in RAM
├─ BSS segment (zero-initialized globals)       → Not in RAM (implicit zeros)
├─ Heap (malloc'd memory)                       → Some pages in RAM
├─ Mmap regions                                 → Some pages in RAM
└─ Stack                                        → Some pages in RAM

RSS = total number of pages actually in RAM × PAGE_SIZE (usually 4096 bytes)
```

**How Linux Tracks RSS:**
- Page table entries marked `present` bit = page in RAM
- Kernel walks page tables, counts present pages
- `get_rss_bytes()` in our kernel module does exactly this

**What RSS Does NOT Track:**
- Unused pages (not paged in yet)
- Swapped pages (on disk, not RAM)
- Mmapped but not touched files
- Copy-on-write pages not yet copied

**Why RSS As Limit?**
- RSS = actual system RAM pressure
- Limiting RSS prevents: system RunOut of memory, thrashing
- Alternative: virtual limit (easier to exceed, less useful)

### 4.2 Soft Limit vs Hard Limit: Different Enforcement Strategies

**Soft Limit (Warning):**
```c
if (rss > soft_limit_bytes && !entry->soft_limit_warned) {
    log_soft_limit_event(entry->container_id, entry->pid, soft_limit_bytes, rss);
    entry->soft_limit_warned = 1;
}
```

**Behavior:**
- Process continues running
- User gets logged warning via dmesg
- Can be viewed with `dmesg` or in `/var/log/kern.log`
- Logged once per process (not repeatedly)

**Use Case:**
- Alerts app developers: "Hey, you're using lots of memory"
- Gives chance to optimize voluntarily
- Suitable for staging environments

**Hard Limit (Enforcement):**
```c
if (rss > hard_limit_bytes) {
    kill_process(container->pid, SIGKILL);
    list_del(&entry->list);
}
```

**Behavior:**
- Process immediately terminated with SIGKILL
- Cannot catch/handle signal (SIGKILL is mandatory death)
- Ensures: no process exceeds hard limit
- Protects: host system from being OOM-killed

**Use Case:**
- Production enforcement: prevent runaway processes
- Container isolation: one container can't starve others
- System protection: no single container can crash the host

### 4.3 Why Enforcement Belongs in Kernel Space

**Wrong: User-Space Enforcement Only**
```c
/* In user space (supervisor) */
while (1) {
    rss = get_rss_bytes_via_procfs(pid);  /* Read /proc/stat */
    if (rss > hard_limit) {
        kill(pid, SIGKILL);
    }
    sleep(1);
}
```

**Problems:**
1. **Time window:** Between checks, process can allocate massive memory
2. **Privilege escalation:** Malicious process could DoS supervisor (stop it)
3. **Coarse granularity:** Every-second polling misses brief spikes
4. **Not atomic:** Process could allocate, then be killed (but damage done)

**Right: Kernel-Space Enforcement**
```c
/* In kernel (monitor module) */
/* Periodic timer (1 second) checks RSS */
/* Kernel internally tracks page allocations */
/* Kernel enforces at page-fault time (microsecond granularity) */
```

**Advantages:**
1. **Always running:** Can't be stopped by userspace
2. **Privileged:** Can kill any process (supervisor can't kill privileged processes)
3. **Atomic:** Enforcement at hardware level (page fault)
4. **Integrated:** Kernel knows about memory before it's allocated

**Real Container Runtimes:**
- cgroups (Linux kernel feature) for memory limits
- cgroups enforce limit at page-allocation time
- Kernel OOM killer when memory unavailable
- User space (Docker daemon) just sets up cgroups, kernel enforces

**Note on Cgroups (We Didn't Use Them):**
- Proper way: use Linux cgroups instead of periodic timer
- cgroups: kernel tracks memory per cgroup automatically
- We use simpler timer-based approach for educational clarity
- Production runtimes use cgroups because:
  - More accurate (enforced at alloc time, not check time)
  - Multiple resource limits (CPU, memory, I/O) together
  - Hierarchical (parent limits apply to all children)

---

## 5. Scheduling Behavior Analysis

### 5.1 What Happens When You Run Multiple Workloads

**CPU Scheduler's Job:**
- Decide which process runs on CPU at each moment
- Goal: Fairness (each process gets fair CPU share) + Responsiveness (interactive processes fast)

**Our Test Scenario: CPU-Bound vs I/O-Bound**

**CPU-Bound Process (cpu_hog):**
```c
while (elapsed < duration) {
    accumulator = accumulator * 1664525 + 1013904223;  /* Pure computation */
}
```
- Uses CPU continuously
- Never blocks (never yields CPU voluntarily)
- Kernel scheduler must time-slice this process

**I/O-Bound Process (io_pulse):**
```c
while (1) {
    data = read_file(...);  /* Blocks waiting for I/O */
    process_data(data);      /* CPU work */
    write_file(...);         /* Blocks waiting for I/O */
}
```
- Blocks on I/O operations (disk, network)
- Kernel blocks process, lets others run
- When I/O completes, kernel wakes process again

### 5.2 Linux CFS Scheduler Behavior

**The Completely Fair Scheduler (CFS):**
- Modern Linux kernel uses per-CPU run queues
- Tracks each process's CPU time (vruntime)
- Always runs process with smallest vruntime (fairest)
- Preempts after time slice (~3ms per process)

**With Nice Priority:**
```bash
./engine start cpu1 ./rootfs-alpha /cpu_hog 10 --nice 0   # Normal priority
./engine start cpu2 ./rootfs-beta /cpu_hog 10 --nice 10   # Lower priority

/* Expected behavior:
   cpu1 (nice 0): ~60% of CPU
   cpu2 (nice 10): ~40% of CPU
   
   Why not 50/50?
   Because nice is exponential: each +1 is ~10% less CPU
*/
```

### 5.3 Observing Scheduler Behavior

**Experiment: CPU Contention**
```bash
# Terminal 1: Start supervisor
./engine supervisor ./rootfs-base

# Terminal 2: Run two CPU-bound tasks with different priorities
time ./engine run cpu1 ./rootfs-alpha /cpu_hog 10 --nice 0
time ./engine run cpu2 ./rootfs-beta /cpu_hog 10 --nice 10

# Both are doing same work for 10 seconds
# But nice 10 is less aggressive, might take longer if run first

# Expected output:
# cpu1 (nice 0): ~10 seconds of elapsed time
# cpu2 (nice 10): slightly longer
# Both finish in ~10 seconds of wall-clock time (if system has 2+ CPUs)
```

**Experiment: CPU and I/O Mix**
```bash
# Terminal 1: Start supervisor
./engine supervisor ./rootfs-base

# Terminal 2: Run CPU job (uses CPU, never blocks)
time ./engine run cpu ./rootfs-alpha /cpu_hog 10

# Terminal 3: Run I/O job (blocks on I/O, lets others use CPU)
# If I/O job is interactive (like a shell), it responds quickly
# Because when blocked on read, scheduler runs other jobs
# When I/O ready, scheduler switches back immediately

./engine run io ./rootfs-alpha /bin/sh -c "cat large_file"
```

**What We'll Observe:**
- CPU-bound tasks monopolize one CPU core
- I/O-bound tasks get responsive feel (appear fast when not blocked on I/O)
- Multi-core: multiple CPU jobs run in parallel
- Single-core: scheduler time-slices between CPU jobs
- When I/O job blocks: CPU jobs get that CPU time

### 5.4 Why These Schedulings Behaviors Matter

**For Container Runtimes:**
1. **Fair CPU distribution:** Prevent single container from monopolizing CPU
   - Solution: nice/priority levels or cgroups
   
2. **I/O scheduling:** Prevent I/O-heavy container from starving interactive ones
   - Linux CFQ (Completely Fair Queueing) I/O scheduler helps
   - cgroups io weights allow proportional I/O allocation

3. **Responsiveness:** Ensure interactive containers (shell) remain responsive
   - Scheduler gives interactive processes priority boost
   - Our experiment: even with nice 19, shell is responsive

4. **Fairness bounds:** If unfair, one container starves others
   - Container runtimes must enforce CPU limits (cgroups)
   - We simulate with `--nice` parameter (less precise but visible)

---

## Key Takeaways

### Isolation: Is It Really Secure?
- **YES for:** Preventing accidental interference, logical separation, sandboxing untrusted tests
- **NO for:** Security from malicious processes without user namespaces, seccomp, AppArmor
- Real containers add: user namespaces (UID 0 container → UID 1000 host), seccomp (syscall filtering)

### Process Lifetimes: Why Supervisors?
- Single-container model: process exits, everything ends
- Supervisor model: orchestrates multiple containers, collects logs, responds to commands
- Real orchestrators (Kubernetes): supervisor → supervisor → containers (hierarchical)

### Synchronization: Details Matter
- Race conditions hard to spot: use locks everywhere
- Condition variables: efficient producer-consumer coordination
- Kernel mutex: safe even in softirq context (doesn't actually sleep there)

### Memory Enforcement: Kernel Knows Best
- Kernel enforces at allocation time, not after-the-fact
- Soft/hard limits: different deployment strategies
- RSS: real RAM usage, includes allocated but not-yet-touched memory

### Scheduling: Fair CPU Distribution
- Linux CFS provides fairness; priority (nice) adjusts weighting
- I/O blocking and CPU binding interact with scheduler
- Interactive processes get kernel priority boost automatically

