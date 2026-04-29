# OS-Jackfruit Implementation Guide

## Project Overview

This is a **multi-container runtime in C** with a user-space supervisor and kernel-space memory monitor. The runtime manages isolated containers with namespace-based isolation and enforces resource limits through both soft and hard memory limits.

---

## Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────────┐
│                    User-Space (engine.c)                     │
├─────────────────────────────────────────────────────────────┤
│  Supervisor Process (long-running daemon)                    │
│  ├─ UNIX Socket Server (Control IPC Path B)                  │
│  ├─ Bounded-Buffer Logging System (Logging IPC Path A)       │
│  ├─ Container Metadata Tracking                              │
│  └─ Signal Handlers (SIGCHLD, SIGINT, SIGTERM)              │
│                                                               │
│  CLI Clients (short-lived processes)                         │
│  ├─ start <id> <rootfs> <command> [--limits]                │
│  ├─ run <id> <rootfs> <command> [--limits]                  │
│  ├─ ps                                                        │
│  ├─ logs <id>                                                │
│  └─ stop <id>                                                │
└─────────────────────────────────────────────────────────────┘
         ↓ (Container Creation via clone)
┌─────────────────────────────────────────────────────────────┐
│              Container Processes (child_fn)                  │
│  ├─ PID Namespace (isolated PID 1)                           │
│  ├─ UTS Namespace (isolated hostname)                        │
│  ├─ Mount Namespace (chroot into rootfs)                     │
│  └─ Working /proc filesystem                                 │
└─────────────────────────────────────────────────────────────┘
         ↓ (IPC: ioctl)
┌─────────────────────────────────────────────────────────────┐
│              Kernel Module (monitor.c)                       │
│  ├─ Device: /dev/container_monitor                           │
│  ├─ Monitored Processes List (linked-list with mutex)        │
│  ├─ Periodic Timer (1-second intervals)                      │
│  ├─ RSS Monitoring & Enforcement                             │
│  │  ├─ Soft Limit: warning message to dmesg                  │
│  │  └─ Hard Limit: SIGKILL the process                       │
│  └─ ioctl() Handlers (register/unregister)                   │
└─────────────────────────────────────────────────────────────┘
```

### Communication Paths

**Path A (Logging IPC):**
```
Container stdout/stderr → Pipe → Supervisor
                                    ↓
                           Bounded-Buffer (producer/consumer)
                                    ↓
                           Logger Thread → Per-container logfile
```

**Path B (Control IPC):**
```
CLI Client → UNIX Domain Socket → Supervisor
                                      ↓
                               Parse & Execute Command
                                      ↓
                               Send Response
```

---

## Implementation Details

### 1. Bounded-Buffer Logging System (Task 3)

**Data Structure:**
```c
typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];  // Fixed-size circular buffer
    size_t head, tail, count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} bounded_buffer_t;
```

**Synchronization:**
- **Mutex:** Protects all buffer state modifications
- **Condition Variable `not_empty`:** Wakes consumer when data available
- **Condition Variable `not_full`:** Wakes producer when space available

**Producer (pipe reader thread):**
- Reads from container's pipe
- Blocks if buffer is full (waits on `not_full`)
- Signals consumer with `not_empty`
- Exits cleanly on pipe close

**Consumer (logging thread):**
- Waits for data (waits on `not_empty`)
- Writes consumed items to per-container logfile
- Returns gracefully on shutdown signal with empty buffer

**Why Mutex + Condition Variables?**
- Supervisor runs in main thread (not interrupt context)
- Logger thread and pipe readers need coordinated access
- Condition variables provide efficient producer-consumer wake-up signaling
- No spinlocks needed (no interrupt/softirq context)

---

### 2. Container Lifecycle (Task 1)

**Child Process Setup (`child_fn`):**
1. Redirect stdout/stderr to pipe for logging
2. Set nice priority (optional)
3. `chdir()` to container rootfs
4. `chroot()` to isolate filesystem
5. `mount("proc", "/proc", "proc", 0, NULL)` for working `ps`
6. `execvp("/bin/sh", ["-c", command, NULL])` to run user command

**Namespace Isolation (via `clone()`):**
- `CLONE_NEWPID`: Container sees itself as PID 1
- `CLONE_NEWUTS`: Independent hostname/domainname
- `CLONE_NEWNS`: Mount namespace (allows chroot isolation)
- `SIGCHLD`: Parent receives SIGCHLD when child exits

**Supervisor Lifecycle Management:**
- Creates per-container `container_record_t` metadata
- Reparenting: Supervisor becomes parent of all containers
- `waitpid(-1, WNOHANG)` reaps exited children
- Metadata tracks: state, exit code, signal, timestamps

---

### 3. CLI Command Interface (Task 2)

**Command Processing:**
```bash
engine supervisor <base-rootfs>          # Start daemon
engine start <id> <rootfs> <cmd> [opts]  # Launch in background
engine run <id> <rootfs> <cmd> [opts]    # Launch and wait
engine ps                                 # List containers
engine logs <id>                          # Show container output
engine stop <id>                          # Send SIGTERM
```

**Optional Flags:**
- `--soft-mib N`: Soft limit (default 40 MiB)
- `--hard-mib N`: Hard limit (default 64 MiB)
- `--nice N`: Nice priority (-20..19)

**Control IPC (UNIX Domain Socket):**
- Client connects to `/tmp/mini_runtime.sock`
- Sends `control_request_t` struct
- Receives `control_response_t` struct
- Connection closes (one-shot per command)

---

### 4. Kernel Memory Monitoring (Task 4)

**Device Registration:**
```c
/dev/container_monitor  (major: dynamic, minor: 0)
```

**Monitored Entry Structure (per container):**
```c
struct monitored_entry {
    struct list_head list;
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit_bytes, hard_limit_bytes;
    int soft_limit_warned;  /* Flag: already warned about soft limit? */
};
```

**Timer Callback (1-second intervals):**
1. Iterate through monitored list under mutex lock
2. Get RSS via `get_rss_bytes(pid)` (uses kernel MM API)
3. If process exited (RSS == -1): remove from list
4. If RSS > hard_limit: `send_sig(SIGKILL)` then remove
5. If RSS > soft_limit and not warned: log printk warning, set flag

**ioctl Operations:**
- `MONITOR_REGISTER`: Add PID to monitored list
- `MONITOR_UNREGISTER`: Remove PID from list

**Why Mutex (not spinlock)?**
- ioctl runs in process context (can sleep)
- Timer runs in softirq context (never sleeps, only disables interrupts)
- Mutex allows safe context switching in ioctl paths
- Timer callback uses mutex_lock (ok in softirq with DEFINE_MUTEX)

---

## Building the Project

### Prerequisites (Ubuntu 22.04/24.04 VM, Secure Boot OFF)

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build Steps

```bash
cd boilerplate

# Check environment
chmod +x environment-check.sh
sudo ./environment-check.sh

# Prepare Alpine rootfs
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Build user-space binaries
make ci

# (Full build, including kernel module - requires sudo later)
make
```

### CI-Safe Build (GitHub Actions compatible)

```bash
make -C boilerplate ci
# Builds: engine, memory_hog, cpu_hog, io_pulse (no kernel module)
```

---

## Running the Runtime

### Terminal 1: Start Supervisor

```bash
cd boilerplate
./engine supervisor ./rootfs-base
# Output: "Supervisor started on socket /tmp/mini_runtime.sock"
```

### Terminal 2: Run Commands

```bash
cd boilerplate

# Start a container in background
./engine start alpha ./rootfs-alpha /bin/sh -c "sleep 5; echo hello"

# List running containers
./engine ps

# View logs
./engine logs alpha

# Stop a container
./engine stop alpha

# Run a container and wait for exit (foreground)
./engine run beta ./rootfs-beta /bin/sh -c "echo 'Hello from beta'"
```

---

## Task Implementation Checklist

### ✅ Task 1: Multi-Container Runtime Supervisor
- [x] Supervisor process manages multiple containers
- [x] PID/UTS/Mount namespace isolation via `clone()`
- [x] Each container uses own rootfs copy
- [x] Proper child reaping (no zombies)
- [x] Container metadata tracking

### ✅ Task 2: Supervisor CLI & Signal Handling
- [x] Command parsing (start, run, ps, logs, stop)
- [x] UNIX domain socket IPC
- [x] SIGCHLD handling for child reaping
- [x] SIGINT/SIGTERM for graceful shutdown
- [x] Container state updates per command

### ✅ Task 3: Bounded-Buffer Logging
- [x] Pipe-based IPC from container to supervisor
- [x] Circular bounded buffer
- [x] Producer (pipe reader) and consumer (logger) threads
- [x] Mutex + condition variables for sync
- [x] Per-container log files
- [x] Clean shutdown without log loss

### ✅ Task 4: Kernel Memory Monitoring
- [x] /dev/container_monitor device
- [x] PID registration via ioctl
- [x] Linked list with mutex protection
- [x] Soft limit warnings (one-shot)
- [x] Hard limit enforcement (SIGKILL)
- [x] Automatic cleanup on process exit

### ✅ Task 5: Scheduler Experiments
- Scheduler experiments can be run with provided workloads:
  - `cpu_hog`: CPU-bound work (10s default)
  - `memory_hog`: Memory-consuming (8 MiB/sec default)
  - `io_pulse`: I/O-intensive operations

### ✅ Task 6: Resource Cleanup
- [x] Container reaping in supervisor
- [x] Logger thread joins cleanly
- [x] File descriptors closed
- [x] Heap resources freed
- [x] Kernel list entries freed on module unload

---

## Testing Scenarios

### Scenario 1: Basic Container Execution
```bash
# Terminal 1
./engine supervisor ./rootfs-base

# Terminal 2
cp ./cpu_hog ./rootfs-alpha/
./engine start test1 ./rootfs-alpha /cpu_hog 3
sleep 1
./engine ps
./engine logs test1
```

### Scenario 2: Memory Limit Enforcement
```bash
# Terminal 1
sudo insmod ./monitor.ko
./engine supervisor ./rootfs-base

# Terminal 2
cp ./memory_hog ./rootfs-alpha/
# Soft limit 32 MiB, hard limit 48 MiB
# memory_hog allocates 8 MiB/sec, will exceed hard limit in ~6 seconds
./engine start memtest ./rootfs-alpha /memory_hog 8 1000 --soft-mib 32 --hard-mib 48
sleep 2
./engine ps
# Should eventually show killed or hard-limit triggered
```

### Scenario 3: Concurrent Containers
```bash
# Terminal 1
./engine supervisor ./rootfs-base

# Terminal 2
./engine start c1 ./rootfs-alpha /bin/sh -c "echo c1; sleep 10"
./engine start c2 ./rootfs-beta /bin/sh -c "echo c2; sleep 5"
./engine start c3 ./rootfs-alpha /bin/sh -c "echo c3; sleep 3"
./engine ps
# Wait for containers to exit
./engine ps
./engine logs c1
./engine logs c2
./engine logs c3
```

### Scenario 4: Signal Handling
```bash
# Terminal 1
./engine supervisor ./rootfs-base

# Terminal 2
./engine run alpha ./rootfs-alpha /bin/sh -c "sleep 100"
# (In another terminal)
# Send Ctrl+C to the run command - should forward SIGTERM to supervisor
```

---

## Debugging Tips

**Check kernel module is loaded:**
```bash
lsmod | grep container_monitor
```

**View kernel logs:**
```bash
sudo dmesg -w
# Or check specific messages:
dmesg | tail -20
```

**Monitor file descriptors:**
```bash
# Check supervisor's open files
ls -la /proc/$(pidof engine)/fd/ | grep socket
```

**Test socket connectivity:**
```bash
# Try connecting manually
nc -U /tmp/mini_runtime.sock
```

**Check container log files:**
```bash
ls -la logs/
cat logs/alpha.log
```

---

## Known Limitations & Future Improvements

1. **Logging Buffer Size:** Fixed at 16 items × 4KB = 64KB max in-flight log data
   - For high-volume output, may need tuning or larger buffer

2. **Memory Limit Granularity:** RSS check every 1 second
   - May miss brief memory spikes between checks
   - Could implement memory cgroup integration for finer control

3. **Soft Limit Handling:** Currently logs warning but does nothing
   - Could implement soft limit via memory cgroup pressure notification
   - Could implement graceful container pause/resume

4. **Container State Transitions:** Simplified state machine
   - Could add more granular states (PAUSED, STOPPED, etc.)

5. **Logging:** File write blocking in consumer thread
   - Could implement async I/O or batching for very high throughput

---

## References

- Linux namespaces: `man namespaces`, `man clone`
- RSS measurement: Linux MM subsystem, `man proc` (/proc/[pid]/stat)
- UNIX domain sockets: `man unix`
- Kernel module development: Linux Kernel Module Programming Guide
- Container concepts: Linux container fundamentals, OCI spec

