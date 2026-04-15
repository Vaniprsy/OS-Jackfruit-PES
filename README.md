# OS Jackfruit – Mini Container Runtime with Kernel Memory Monitor

## Overview
This project implements a lightweight container runtime in C with a kernel-space memory monitor module.

The runtime supports:
- container start/stop
- process supervision
- per-container logs
- container metadata tracking
- `ps` style runtime inspection
- memory soft and hard limit enforcement
- clean module teardown

The system uses a userspace supervisor and a Linux kernel module to monitor per-container resource usage.

---

## Design Decisions

### Lock Choice
A mutex was used in the kernel monitor for container metadata protection because updates may involve sleeping operations and process-context-safe memory access. Spinlocks were avoided since sleeping inside spinlock-protected critical sections is unsafe.

### IPC Choice
UNIX domain sockets were chosen between the CLI and supervisor because they provide lightweight bidirectional request-response semantics and are easier to manage than FIFOs or shared memory for small control messages.

### Bounded Logging
Each container writes to a dedicated log file inside `logs/`. This keeps logging isolated per container and prevents unbounded in-memory growth.

---

## Build and Run

```bash
cd boilerplate
make
sudo insmod monitor.ko
sudo ./engine supervisor /
sudo ./engine start c1 / "echo hello"
sudo ./engine ps
sudo ./engine logs c1
sudo ./engine stop c1
sudo rmmod monitor
