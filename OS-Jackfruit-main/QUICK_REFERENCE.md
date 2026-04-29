# Quick Reference Guide

## Getting Started (5 Minutes)

### Prerequisites Check
```bash
# Verify environment
uname -a  # Should show Linux
ls -la /boot/vmlinuz*  # Kernel installed
gcc --version  # GCC available
make --version  # GNU Make available
```

### One-Time Setup
```bash
# Install dependencies
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

# Navigate to project
cd boilerplate

# Prepare rootfs
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Build
make ci
```

### Run (Every Test)
```bash
# Terminal 1: Start supervisor
./engine supervisor ./rootfs-base

# Terminal 2: Run commands
./engine start test1 ./rootfs-alpha /bin/sh -c "echo Hello"
./engine ps
./engine logs test1
```

---

## All Commands

```bash
# Supervisor daemon (must run in background)
./engine supervisor ./rootfs-base

# Container operations
./engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]
./engine run <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]
./engine ps
./engine logs <id>
./engine stop <id>
```

---

## Key Files & Functions

| File | Purpose | Key Functions |
|------|---------|---|
| `engine.c` | Supervisor + CLI | `run_supervisor()`, `execute_start_cmd()`, `child_fn()`, `logging_thread()`, `bounded_buffer_push/pop()` |
| `monitor.c` | Kernel module | `timer_callback()`, `monitor_ioctl()`, linked list management |
| `monitor_ioctl.h` | IPC definitions | `struct monitor_request`, ioctl commands |
| `cpu_hog.c` | CPU workload | CPU-bound computation |
| `memory_hog.c` | Memory workload | Allocates and touches memory |
| `io_pulse.c` | I/O workload | I/O-bound operations |

---

## Important Paths

```
/tmp/mini_runtime.sock          # Supervisor IPC socket
./logs/                         # Container output files
./logs/<container_id>.log       # Per-container log
/dev/container_monitor          # Kernel device
/var/log/kern.log              # Kernel messages
~/.ssh/authorized_keys          # (for remote VMs)
```

---

## Viewing Output

```bash
# Watch container logs in real-time
tail -f logs/container_id.log

# View all kernel messages
dmesg | tail -20

# Watch memory limit events
dmesg | grep "LIMIT"

# Check running containers
./engine ps

# See supervisor's listening socket
netstat -U | grep mini_runtime
lsof -i | grep engine
```

---

## Debugging Checklist

- [ ] Supervisor is running (`./engine ps` shows output)
- [ ] Socket exists (`ls -la /tmp/mini_runtime.sock`)
- [ ] Containers created (`./engine ps` shows entries)
- [ ] Logs written (`ls logs/` shows files)
- [ ] No permission denied (running with appropriate privileges)
- [ ] Kernel module loaded (checked with `lsmod`)
- [ ] System calls not timing out (adjust `select()` timeout if needed)

---

## Build Commands

```bash
# CI-safe: just user-space binaries
make ci

# Full: includes kernel module
make

# Clean everything
make clean

# Individual targets
make engine
make monitor.ko
make cpu_hog
make memory_hog
make io_pulse
```

---

## Common Issues & Fixes

### Cannot connect to supervisor
```
Error: Could not connect to supervisor
→ Start supervisor first: ./engine supervisor ./rootfs-base
```

### Kernel module loading fails
```
Error: insmod: ERROR: could not insert module
→ Check kernel version matches headers
→ Try: modprobe monitor (after make module)
```

### Permission denied on /dev/container_monitor
```
Error: open /dev/container_monitor: Permission denied
→ Run with: sudo ./engine supervisor ./rootfs-base
→ Or: sudo insmod monitor.ko first
```

### Zombie processes
```
$ ps aux | grep defunct
→ Supervisor not reaping properly
→ Check SIGCHLD handler or waitpid() logic
```

### No logs created
```
→ Check logs/ directory exists: mkdir -p logs
→ Check container produced output
→ View with: cat logs/container_id.log
```

---

## Performance Tips

### For Better Testing
- Use `-static` flag if cross-compiling to different systems
- Close unused pipes immediately in supervisor
- Use `WNOHANG` in `waitpid()` to avoid blocking
- Condition variables more efficient than polling

### For Container Performance
- Pin CPUs: cgroups cpuset (future enhancement)
- Track memory: use RSS monitoring via cgroups (future)
- Reduce nice values on critical containers (needs evaluation)

---

## Related Commands

```bash
# System info
uname -a
lsb_release -a
cat /proc/cpuinfo
cat /proc/meminfo

# Process inspection
ps aux
top
htop
ps -eLf  # Show threads

# Kernel modules
lsmod                           # List modules
modinfo monitor.ko              # Module info
dmesg -w                        # Watch kernel messages

# IPC inspection
netstat -U                      # Unix sockets
lsof -i                        # Open files
ipcs                           # Shared memory, etc.

# Memory & limits
free -h                        # System memory
top -o %MEM                    # Processes by memory
ulimit -a                      # Process limits
```

---

## Documentation Map

| Document | Contains |
|----------|----------|
| `README.md` | Overview, quick start, commands |
| `IMPLEMENTATION.md` | Architecture details, design decisions |
| `OS_ANALYSIS.md` | Deep OS concepts explanation |
| `SUMMARY.md` | Complete implementation summary |
| `project-guide.md` | Original requirements |
| `QUICK_REFERENCE.md` | This file |

---

## Next Steps After Running

1. **Verify:** Check logs and ps output
2. **Experiment:** Try different memory limits and priorities
3. **Analyze:** Look at dmesg output for kernel events
4. **Extend:** Add more container isolation features
5. **Optimize:** Explore performance tuning opportunities

---

## Essential Environment Variables

```bash
# Usually already set, but verify if build fails:
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"
export LANG=en_US.UTF-8
export LOGNAME=$(whoami)
export USER=$(whoami)
```

---

## Testing Matrix

| Scenario | Command | Expected | Check |
|----------|---------|----------|-------|
| Start container | `engine start test ./rootfs-alpha /bin/sh -c "echo x"` | Returns immediately | `engine ps` shows test |
| Wait for container | `engine run test ./rootfs-alpha /bin/sh -c "sleep 1"` | Waits ~1 second | Time output confirms |
| Stop container | `engine stop test` | Container stops | `engine ps` shows stopped |
| View logs | `engine logs test` | Shows output | Matches console output |
| Memory limit | `memory_hog --hard-mib 50` | Stops at ~50MB | `dmesg` shows SIGKILL |
| No zombies | (after container exits) | `ps aux` clean | No `<defunct>` entries |

---

## Success Checklist

- ✅ Code compiles without errors
- ✅ Supervisor starts and listens
- ✅ Containers launch and run commands
- ✅ Multiple containers coexist peacefully
- ✅ Logs captured correctly
- ✅ Memory limits enforced
- ✅ Clean shutdown without zombies
- ✅ Kernel module loads/unloads safely
- ✅ All tests pass in Documentation

---

## Final Verification

```bash
# Run this sequence to verify complete setup:

cd boilerplate

# Check 1: Build
echo "=== Build Test ==="
make clean && make ci && echo "✓ Build OK" || echo "✗ Build Failed"

# Check 2: Supervisor starts
echo "=== Supervisor Test ==="
timeout 2 ./engine supervisor ./rootfs-base 2>&1 | head -1 || echo "✓ Supervisor started"

# Check 3: Check output format
echo "=== Output Format Test ==="
./engine ps 2>&1 | grep -q "PID.*STATE" && echo "✓ Output format OK" || echo "✗ Format issue"

echo ""
echo "Setup verification complete!"
echo "Next: Start supervisor in terminal 1: ./engine supervisor ./rootfs-base"
echo "Then: Run commands in terminal 2: ./engine start..."
```

---
