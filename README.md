# OS-Jackfruit — Supervised Multi-Container Runtime

A lightweight Linux container runtime built from scratch in C, backed by a custom Linux kernel module for real-time memory monitoring.

Built as part of **UE24CS242B — Operating Systems**, PES University, Jan–May 2026.

**Team:**
- Vanishree — PES2UG24CS665
- Shubha — PES2UG24CS660

---

## What This Project Does

This project implements a mini version of Docker using raw Linux system calls. It has two parts:

**1. User-space runtime (`engine.c`)**
A long-running supervisor process that creates and manages isolated containers using `clone()` and Linux namespaces. A UNIX domain socket lets CLI commands (`start`, `stop`, `ps`, `logs`) communicate with the supervisor. Container output is captured asynchronously using a producer-consumer bounded buffer backed by pthreads.

**2. Kernel module (`monitor.c`)**
A Linux kernel module that tracks the RAM usage of registered container processes every second using a timer callback. When usage crosses a soft limit it logs a warning to `dmesg`. When it crosses a hard limit it sends `SIGKILL` to the process and removes it from the monitored list. User space registers and unregisters containers using `ioctl`.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                     User Space                           │
│                                                          │
│   CLI (engine start/stop/ps/logs)                        │
│          │                                               │
│          │  UNIX domain socket (/tmp/mini_runtime.sock)  │
│          ▼                                               │
│   Supervisor (engine supervisor)                         │
│          │                                               │
│          ├── clone() ──► Container Process               │
│          │               (chroot + namespaces)           │
│          │                                               │
│          ├── pipe ──► log reader thread                  │
│          │               │                               │
│          │           bounded buffer                      │
│          │               │                               │
│          │           logging thread ──► logs/c1.log      │
│          │                                               │
│          └── ioctl(MONITOR_REGISTER) ───────────────┐    │
│                                                     │    │
├─────────────────────────────────────────────────────│────┤
│                    Kernel Space                     │    │
│                                                     ▼    │
│   monitor.ko                                             │
│          │                                               │
│          ├── /dev/container_monitor (char device)        │
│          │                                               │
│          ├── linked list of monitored PIDs               │
│          │                                               │
│          └── timer (every 1s)                            │
│               ├── RSS > soft limit → dmesg warning       │
│               └── RSS > hard limit → SIGKILL + cleanup   │
└──────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
boilerplate/
├── engine.c            ← user-space runtime and supervisor
├── monitor.c           ← kernel module (memory monitor)
├── monitor_ioctl.h     ← shared ioctl definitions
├── Makefile            ← builds both user-space and kernel module
├── cpu_hog.c           ← CPU-bound test workload
├── io_pulse.c          ← I/O-bound test workload
├── memory_hog.c        ← memory-consuming test workload
├── environment-check.sh← VM environment preflight check
└── logs/               ← per-container log files (auto-created)
```

---

## Environment Requirements

- Ubuntu 22.04 or 24.04 (bare metal or VM — **not WSL**)
- Secure Boot **OFF** (required for loading unsigned kernel modules)
- Kernel headers installed

Check Secure Boot status:
```bash
mokutil --sb-state
# Must say: SecureBoot disabled
```

---

## Setup and Build

### 1. Install dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 2. Clone and enter the project

```bash
git clone https://github.com/Vaniprsy/OS-Jackfruit-PES.git
cd OS-Jackfruit-PES/boilerplate
```

### 3. Build everything

```bash
make
```

This produces:
- `engine` — the container runtime binary
- `monitor.ko` — the kernel module
- `cpu_hog`, `memory_hog`, `io_pulse` — test workload binaries

### 4. Prepare the Alpine rootfs

```bash
cd ..
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Make one writable copy per container
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Copy workloads into rootfs so containers can run them
cp boilerplate/cpu_hog boilerplate/memory_hog boilerplate/io_pulse rootfs-alpha/
cp boilerplate/cpu_hog boilerplate/memory_hog boilerplate/io_pulse rootfs-beta/
```

### 5. Load the kernel module

```bash
cd boilerplate
sudo insmod monitor.ko
sudo dmesg | grep container_monitor | tail -3
ls /dev/container_monitor
```

Expected output:
```
[container_monitor] Module loaded. Device: /dev/container_monitor
/dev/container_monitor
```

---

## Running the Runtime

### Step 1 — Start the supervisor (Terminal 1)

```bash
sudo ./engine supervisor ../rootfs-base
```

The supervisor binds to `/tmp/mini_runtime.sock` and waits for commands.

### Step 2 — Use the CLI (Terminal 2)

**Start a container in the background:**
```bash
sudo ./engine start c1 ../rootfs-alpha /cpu_hog
```

**Start and wait for it to finish:**
```bash
sudo ./engine run c1 ../rootfs-alpha /cpu_hog
```

**List all containers:**
```bash
sudo ./engine ps
```

Example output:
```
ID               PID      STATE      SOFT(MB)   HARD(MB)
c1               9142     running    40         64
```

**View container logs:**
```bash
sudo ./engine logs c1
```

**Stop a container:**
```bash
sudo ./engine stop c1
```

**With custom memory limits:**
```bash
sudo ./engine start mem-test ../rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40
```

**With custom scheduling priority:**
```bash
sudo ./engine start cpu-lo ../rootfs-alpha /cpu_hog --nice 10
sudo ./engine start cpu-hi ../rootfs-alpha /cpu_hog --nice 0
```

---

## Memory Monitoring Demo

Run this in three terminals to see soft and hard limits fire in real time:

**Terminal 1 — supervisor:**
```bash
sudo ./engine supervisor ../rootfs-base
```

**Terminal 2 — launch memory hog with low limits:**
```bash
sudo ./engine start mem-test ../rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40
```

**Terminal 3 — watch kernel logs:**
```bash
sudo dmesg -w | grep container_monitor
```

Expected kernel output (appears within ~20 seconds):
```
[container_monitor] Registering container=mem-test pid=8940 soft=20971520 hard=41943040
[container_monitor] SOFT LIMIT container=mem-test pid=8940 rss=25763840 limit=20971520
[container_monitor] HARD LIMIT container=mem-test pid=8940 rss=42409984 limit=41943040
[container_monitor] Unregister request container=mem-test pid=8940
```

---

## Unloading the Kernel Module

```bash
sudo rmmod monitor
sudo dmesg | grep container_monitor | tail -3
```

Expected:
```
[container_monitor] Module unloaded.
```

The module frees all remaining list entries on unload — no kernel memory leaks.

---

## Implementation Details

### Container Isolation

Each container is created with `clone()` using three namespace flags:

| Flag | Effect |
|---|---|
| `CLONE_NEWPID` | Container gets its own PID namespace — thinks it is PID 1 |
| `CLONE_NEWUTS` | Container gets its own hostname |
| `CLONE_NEWNS` | Container gets its own mount namespace |

After `clone()`, the child process:
1. Sets hostname to the container ID via `sethostname()`
2. Calls `chroot(rootfs)` to lock into the Alpine filesystem
3. Mounts `/proc` so tools like `ps` work inside
4. Redirects stdout/stderr to a log pipe via `dup2()`
5. Applies the nice value via `nice()`
6. Executes the command via `execv("/bin/sh", "-c", command)`

### Supervisor IPC — UNIX Domain Socket

The supervisor creates a UNIX domain socket at `/tmp/mini_runtime.sock`. Each CLI invocation connects, sends a `control_request_t` struct, and receives a `control_response_t` struct. The supervisor handles one request at a time using a `select()` event loop.

**Why UNIX socket over FIFO:**
Sockets are bidirectional — one connection handles both the request and response. FIFOs are unidirectional and would require two separate files. Sockets also support multiple concurrent clients through `accept()`.

### Asynchronous Logging — Bounded Buffer

```
Container stdout/stderr
        │
        │ pipe
        ▼
  log reader thread  ──push──►  bounded_buffer (16 slots)  ──pop──►  logging thread  ──►  logs/c1.log
   (producer)                   mutex + cond vars              (consumer)
```

- Buffer capacity: 16 chunks of 4096 bytes each
- Producer blocks on `pthread_cond_wait(&not_full)` when buffer is full
- Consumer blocks on `pthread_cond_wait(&not_empty)` when buffer is empty
- Shutdown: `bounded_buffer_begin_shutdown()` broadcasts to both condition variables so all threads wake up and exit cleanly

### Kernel Module — Memory Monitor

The module maintains a kernel linked list (`LIST_HEAD`) of `monitored_entry` nodes. A `timer_list` fires every `CHECK_INTERVAL_SEC` (1 second) and calls `timer_callback()`.

Inside the callback, `list_for_each_entry_safe()` is used for safe deletion during iteration. For each entry:

```
get_rss_bytes(pid)
    │
    ├── returns -1  →  process exited  →  list_del + kfree
    │
    ├── rss > hard_limit  →  send_sig(SIGKILL)  →  list_del + kfree
    │
    └── rss > soft_limit && !soft_warned  →  printk WARNING  →  soft_warned = 1
```

**Why mutex over spinlock:**
The timer callback calls `get_task_mm()` and `mmput()` which can sleep. Spinlocks cannot be held while a function sleeps — doing so causes a kernel deadlock or crash. A mutex is safe to sleep while holding, making it the only correct choice for this code path.

### ioctl Communication

```
engine.c (user space)                    monitor.c (kernel)
                                         
open("/dev/container_monitor")
                                         
ioctl(fd, MONITOR_REGISTER, &req)  ───►  monitor_ioctl()
  req.pid = child_pid                      kmalloc(monitored_entry)
  req.soft_limit_bytes = ...               list_add_tail()
  req.hard_limit_bytes = ...               return 0
  req.container_id = "c1"
                                         
ioctl(fd, MONITOR_UNREGISTER, &req) ──►  monitor_ioctl()
                                           list_for_each_entry_safe()
                                           list_del + kfree
```

### Signal Handling

| Signal | Handler | Action |
|---|---|---|
| `SIGCHLD` | `sigchld_handler` | Calls `waitpid(-1, WNOHANG)` to reap zombie children, updates container state to EXITED or KILLED |
| `SIGTERM` | `sigterm_handler` | Sets `ctx.should_stop = 1` to break the event loop |
| `SIGINT` | `sigterm_handler` | Same as SIGTERM — handles Ctrl+C cleanly |

---

## Scheduling Experiment

Run two containers simultaneously with different nice values and compare CPU progress:

```bash
# Low priority (nice 10)
sudo ./engine run sched-lo ../rootfs-alpha "/cpu_hog 10" --nice 10 &

# Normal priority (nice 0)  
sudo ./engine run sched-hi ../rootfs-alpha "/cpu_hog 10" --nice 0 &
```

The nice 0 container completes faster because the Linux CFS scheduler allocates it more CPU time relative to the nice 10 container when both are competing for the same core.

---

## Design Decisions Summary

| Decision | Choice | Reason |
|---|---|---|
| Kernel lock | `mutex` | Timer callback sleeps — spinlock would crash |
| IPC mechanism | UNIX domain socket | Bidirectional, supports multiple clients, simpler than shared memory |
| Container creation | `clone()` with namespaces | Lightweight isolation without a full VM |
| Filesystem isolation | `chroot()` | Locks container into Alpine rootfs |
| Log transport | pipe + bounded buffer | Decouples fast container output from slow file I/O |
| Memory check interval | 1 second | Responsive enough to catch runaway processes quickly |
| Soft limit behaviour | warn once | Avoid log spam — one warning is enough |
| Hard limit behaviour | SIGKILL + remove | Immediate enforcement, no recovery path needed |

---

## GitHub Repository
