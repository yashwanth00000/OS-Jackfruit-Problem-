# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor. This project implements core OS concepts: process isolation via namespaces, thread synchronization with bounded buffers, kernel module development, and system resource enforcement.

**Student Team:** 2 Students  
**Environment:** Ubuntu 22.04 or 24.04 VM (Secure Boot OFF)

---

## Quick Start

### 1. Prerequisites & Setup

```bash
# Ubuntu 22.04/24.04 VM with Secure Boot disabled
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

cd boilerplate

# Check your environment
chmod +x environment-check.sh
sudo ./environment-check.sh

# Prepare Alpine Linux rootfs
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### 2. Build

```bash
cd boilerplate

# CI-safe build (GitHub Actions compatible)
make ci

# Full build with kernel module
make
```

### 3. Run

#### Terminal 1: Start Supervisor Daemon
```bash
cd boilerplate
sudo ./engine supervisor ./rootfs-base
# Output: "Supervisor started on socket /tmp/mini_runtime.sock"
```

#### Terminal 2: Run Containers
```bash
cd boilerplate

# Start a background container
./engine start alpha ./rootfs-alpha /bin/sh -c "echo Hello from alpha; sleep 5"

# List containers
./engine ps

# View logs
./engine logs alpha

# Run a container in foreground
./engine run beta ./rootfs-beta /bin/sh -c "hostname"

# Stop a running container
./engine stop alpha
```

---

## Architecture Overview

### Design Philosophy

The runtime splits responsibility between three layers:

1. **User-Space Runtime (`engine`):** Multi-container supervisor daemon with CLI
2. **Logging Pipeline:** Bounded-buffer producer-consumer for concurrent log aggregation
3. **Kernel Monitor:** Memory enforcement with soft/hard limits

### Communication Paths

```
┌─────────────────────────────────────────────────────────────┐
│                      Supervisor (engine)                    │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ Main Loop: Accept commands, reap children, manage   │  │
│  ├──────────────────────────────────────────────────────┤  │
│  │ Pipe Readers (1 per container)  ← Container stdout  │  │
│  │    ↓ Push to Bounded Buffer                         │  │
│  │ Logger Thread                    ← Pops from Buffer  │  │
│  │    ↓ Write to Log Files                             │  │
│  └──────────────────────────────────────────────────────┘  │
│  UNIX Domain Socket: Receives CLI commands from clients    │
└─────────────────────────────────────────────────────────────┘
              ↓ (clone with namespace isolation)
┌─────────────────────────────────────────────────────────────┐
│   Container Processes (PID/UTS/Mount namespaces)           │
│  ├─ rootfs via chroot                                      │
│  ├─ isolated /proc filesystem                              │
│  └─ User commands (shell, workloads, etc.)                │
└─────────────────────────────────────────────────────────────┘
              ↓ (ioctl register memory limits)
┌─────────────────────────────────────────────────────────────┐
│           Kernel Module (/dev/container_monitor)           │
│  ├─ Monitored Process List (linked list + mutex)           │
│  ├─ Timer Callback (1-second intervals)                    │
│  │  ├─ Check RSS via kernel MM API                         │
│  │  ├─ Soft limit → log warning (once)                     │
│  │  └─ Hard limit → SIGKILL process                        │
│  └─ ioctl Handlers (register/unregister PID)               │
└─────────────────────────────────────────────────────────────┘
```

---

## CLI Commands

```bash
# Start supervisor (background daemon)
engine supervisor <base-rootfs>

# Start container in background
engine start <id> <container-rootfs> <command> [options]

# Run container in foreground (wait for exit)
engine run <id> <container-rootfs> <command> [options]

# List all containers and their status
engine ps

# Show container's captured log/output
engine logs <id>

# Gracefully stop a container
engine stop <id>
```

### Options

- `--soft-mib N`: Soft memory limit in MiB (default: 40)
- `--hard-mib N`: Hard memory limit in MiB (default: 64)
- `--nice N`: Process priority -20..19 (default: 0)

### Examples

```bash
# Run interactive shell
./engine run web ./rootfs-alpha /bin/sh

# Run with memory limits: warn at 48MB, kill at 80MB
./engine start mem_test ./rootfs-alpha ./memory_hog \
  --soft-mib 48 --hard-mib 80

# Run with lower priority (uses less CPU)
./engine start bg_job ./rootfs-beta ./cpu_hog 10 --nice 10
```

---

## Implementation Details

### Task 1: Multi-Container Supervisor

**Accomplishments:**
- ✅ Supervisor stays alive while managing multiple containers
- ✅ Uses `clone()` with namespaces: PID, UTS, Mount
- ✅ Each container gets isolated chroot rootfs
- ✅ `/proc` mounted inside container for `ps`, `top`
- ✅ Proper child reaping (no zombies)
- ✅ Metadata tracking per container (ID, PID, state, exit code, signal)

**Key Files:**
- `engine.c`: `run_supervisor()`, `execute_start_cmd()`, `child_fn()`

### Task 2: CLI and Signal Handling

**Accomplishments:**
- ✅ Commands: `start`, `run`, `ps`, `logs`, `stop`
- ✅ UNIX domain socket IPC (Path B)
- ✅ SIGCHLD handling with `waitpid(-1, WNOHANG)`
- ✅ SIGINT/SIGTERM graceful shutdown
- ✅ Container state machine: STARTING → RUNNING → EXITED/STOPPED/KILLED

**Key Files:**
- `engine.c`: `send_control_request()`, `cmd_*()` functions, signal handlers

### Task 3: Bounded-Buffer Logging

**Accomplishments:**
- ✅ Circular bounded buffer (16 items × 4KB)
- ✅ Producer threads (one per container) read from pipes
- ✅ Consumer thread (logger) writes to per-container logfiles
- ✅ Mutex + condition variables (`not_empty`, `not_full`)
- ✅ Clean shutdown without log loss
- ✅ Per-container `/var/log` or `logs/` directory

**Key Files:**
- `engine.c`: `bounded_buffer_*()`, `logging_thread()`, `pipe_reader_thread()`

### Task 4: Kernel Memory Monitoring

**Accomplishments:**
- ✅ Device: `/dev/container_monitor` (character device)
- ✅ PID registration via `ioctl(MONITOR_REGISTER)`
- ✅ Linked list with mutex protection
- ✅ Soft limit: one-time warning to `dmesg`
- ✅ Hard limit: `SIGKILL` on RSS overage
- ✅ Automatic cleanup on process exit

**Key Files:**
- `monitor.c`: kernel module with hardening via linked list + mutex

**Synchronization Note:**
- Uses mutex (not spinlock) for safety across ioctl (process context) and timer (softirq context)
- Timer runs in softirq but doesn't sleep; mutex is safe here
- Justification: ioctl may sleep; kernel modules should minimize preemption time

### Task 5: Scheduler Experiments

**Setup:** Use the provided workloads to observe Linux CFS scheduler behavior

```bash
# Experiment 1: CPU contention with different priorities
time ./engine run cpu1 ./rootfs-alpha /cpu_hog 10 --nice 0
time ./engine run cpu2 ./rootfs-beta /cpu_hog 10 --nice 10
# Expected: both finish ~10 seconds (wall-clock time)
# But: nice 0 gets ~60% CPU, nice 10 gets ~40% CPU

# Experiment 2: CPU vs I/O workload mix
./engine start cpu ./rootfs-alpha /cpu_hog 30
./engine start io ./rootfs-beta /bin/sh -c "for i in {1..100}; do echo $i; sleep 0.1; done"
./engine ps
# Expected: CPU job runs continuously; I/O job remains responsive

# Experiment 3: Memory pressure effects
./engine run mem1 ./rootfs-alpha ./memory_hog 8 500 --soft-mib 30 --hard-mib 50
# memory_hog: 8MB chunk, 500ms sleep between allocations
# Will exceed soft limit (~30s), hard limit (~50-60s)
# Check dmesg for soft limit warnings, observe SIGKILL
dmesg | tail -20
```

### Task 6: Resource Cleanup

**Verification:**
```bash
# After supervisor exits:
ps aux | grep engine  # Should see no engine processes

# Check no zombie processes:
ps aux | grep defunct  # Should return nothing

# Check log files cleaned up:
ls -la logs/  # Directory exists but old logs can be deleted

# Verify kernel module unload:
lsmod | grep container_monitor  # Should not appear after `rmmod monitor`
```

---

## Engineering Analysis

See [OS_ANALYSIS.md](OS_ANALYSIS.md) for detailed discussion of OS fundamentals exercised by this project:

1. **Isolation Mechanisms** (Namespaces, chroot, what kernel shares)
2. **Supervisor and Process Lifecycle** (Parent-child relationships, reaping, metadata)
3. **IPC, Threads, and Synchronization** (Race conditions, mutex vs spinlock)
4. **Memory Management and Enforcement** (RSS, soft vs hard limits, kernel vs userspace)
5. **Scheduling Behavior** (CFS scheduler, nice priority, CPU contention)

---

## Building from Scratch

### Development Workflow

```bash
cd boilerplate

# Edit engine.c and/or monitor.c
nano engine.c

# Test compilation (user-space only, no kernel module)
make ci

# If no errors, test full build
make

# Load kernel module and test
sudo insmod monitor.ko
./engine supervisor ./rootfs-base  # In terminal 1
./engine start test ./rootfs-alpha /bin/sh -c "echo test"  # In terminal 2

# Unload module when done
sudo rmmod monitor
```

### Troubleshooting Compilation

**Missing pthread.h (Windows/MinGW):**
- This project requires Linux. Use a Linux VM or WSL2+Ubuntu.

**Missing linux-headers:**
```bash
sudo apt install linux-headers-$(uname -r)
uname -r  # Check kernel version
```

**Module compilation fails:**
```bash
make clean
make module
dmesg | tail -20  # Check kernel messages
```

---

## Testing Checklist

### Functional Tests

- [ ] `make ci` builds user-space binaries without errors
- [ ] Supervisor starts and binds socket with no errors
- [ ] `engine start` creates container and returns immediately
- [ ] `engine run` creates container and waits for exit
- [ ] `engine ps` lists all containers with correct state
- [ ] `engine logs <id>` displays captured output
- [ ] `engine stop <id>` terminates container gracefully
- [ ] Multiple containers run concurrently
- [ ] Container namespaces are isolated (`ps` shows only container processes)
- [ ] Container output is captured and logged correctly

### Kernel Module Tests

- [ ] `make module` compiles without errors
- [ ] `sudo insmod monitor.ko` loads successfully
- [ ] `/dev/container_monitor` device appears
- [ ] PID registration via ioctl succeeds
- [ ] Soft limit warnings appear in `dmesg`
- [ ] Hard limit enforcement (SIGKILL) works
- [ ] `sudo rmmod monitor` unloads without memory leaks
- [ ] No zombie processes after supervisor exit

### Signal Handling Tests

- [ ] SIGINT to supervisor triggers graceful shutdown
- [ ] SIGINT in `run` command forwards SIGTERM to container
- [ ] Children reap cleanly (no zombies)
- [ ] Multiple rapid stop commands don't crash supervisor

---

## Project Structure

```
.
├── README.md                          # This file
├── OS_ANALYSIS.md                     # Detailed OS fundamentals analysis
├── IMPLEMENTATION.md                  # Architecture and design details
├── project-guide.md                   # Official project specifications
├── boilerplate/
│   ├── Makefile                       # Build configuration
│   ├── engine.c                       # User-space runtime (supervisor + CLI)
│   ├── monitor.c                      # Kernel module
│   ├── monitor_ioctl.h                # Shared ioctl definitions
│   ├── cpu_hog.c                      # CPU-bound workload
│   ├── memory_hog.c                   # Memory-consuming workload
│   ├── io_pulse.c                     # I/O-bound workload
│   ├── environment-check.sh           # Preflight environment check
│   └── logs/                          # Per-container log files (created at runtime)
└── rootfs-*                           # Container root filesystems (not in repo)
```

---

## Known Limitations & Future Work

### Current Limitations

1. **Bounded Buffer Size:** Fixed at 64KB (may overflow with high-volume logging)
2. **Memory Check Granularity:** 1-second intervals (misses brief spikes)
3. **Logging File Writes:** Synchronous (potential bottleneck at very high throughput)
4. **No Network Isolation:** Containers share host network namespace
5. **Single-Thread Command Processing:** Sequential request handling

### Future Improvements

1. **Integrate cgroups:** Proper resource limits (CPU, memory, I/O, PIDs)
2. **Async Logging:** Non-blocking I/O for log writes
3. **Network Namespace:** Full container network isolation
4. **REST API:** HTTP interface instead of CLI
5. **Hierarchical Containers:** Nested supervision (container in container)
6. **Live Migration:** Checkpoint/restore container state
7. **Resource Awareness:** Dynamic limit adjustment based on system load

---

## References & Further Reading

### Linux Kernel Concepts
- Namespaces: `man namespaces`, `man clone`
- Processes: `man execve`, `man wait`
- Process Priority: `man nice`, `man setpriority`

### Kernel Module Development
- Linux Kernel Module Programming Guide
- `man ioctl`, `man timer_setup`
- Device drivers in Linux (`Documentation/driver-api/` in kernel source)

### Container Technology
- Docker documentation
- Open Container Initiative (OCI) specification
- Linux container fundamentals

### Performance & Profiling
- CPU scheduling: `man sched`, CFS scheduler internals
- Memory management: `man proc` (procfs), Linux MM subsystem
- `perf` tool for tracing and analysis

---

## Contributors

- Implemented by: [Team Members]
- Project by: Shivang Jhalani

---

## License

This educational project is provided as-is for OS systems learning.


---

## What to Do Next

Read [`project-guide.md`](project-guide.md) end to end. It contains:

- The six implementation tasks (multi-container runtime, CLI, logging, kernel monitor, scheduling experiments, cleanup)
- The engineering analysis you must write
- The exact submission requirements, including what your `README.md` must contain (screenshots, analysis, design decisions)

Your fork's `README.md` should be replaced with your own project documentation as described in the submission package section of the project guide. (As in get rid of all the above content and replace with your README.md)
