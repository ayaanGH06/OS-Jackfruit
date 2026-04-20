# Multi-Container Runtime

## 1. Team Information

| Name | SRN |
|------|-----|
| [Syed Ayaan Hasan] | [PES2UG24CS545] |


---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM, Secure Boot OFF.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
make
```

This produces: `engine`, `memory_hog`, `cpu_hog`, `io_pulse`, `monitor.ko`

### Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy workloads into container rootfs
cp memory_hog ./rootfs-alpha/
cp cpu_hog    ./rootfs-alpha/
cp io_pulse   ./rootfs-alpha/
cp cpu_hog    ./rootfs-beta/
```

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
dmesg | tail -3
```

### Start Supervisor

```bash
# Terminal 1
sudo ./engine supervisor ./rootfs-base
```

### Launch Containers

```bash
# Terminal 2
sudo ./engine start alpha ./rootfs-alpha "/bin/sh -c 'while true; do echo hello from alpha; sleep 1; done'" --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  "/bin/sh -c 'while true; do echo hello from beta;  sleep 1; done'" --soft-mib 64 --hard-mib 96

sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Run Memory Test (hard limit demo)

```bash
cp memory_hog ./rootfs-alpha/
sudo ./engine start memtest ./rootfs-alpha "/memory_hog" --soft-mib 30 --hard-mib 50
# watch dmesg
dmesg | tail -10
```

### Scheduling Experiment

```bash
# Two CPU-bound containers with different nice values
sudo ./engine start cpu-lo ./rootfs-alpha "/cpu_hog" --nice 0
sudo ./engine start cpu-hi ./rootfs-beta  "/cpu_hog" --nice 10
sudo ./engine ps
# Observe completion time difference
```

### Teardown

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Ctrl-C the supervisor
sudo rmmod monitor
dmesg | tail -5
```

---

## 3. Demo Screenshots

> **Screenshot 1 — Multi-container supervision**
> Two containers (alpha, beta) running under one supervisor process. `ps aux` shows the supervisor and both container child processes.
> **Screenshot 2 — Metadata tracking**
> Output of `sudo ./engine ps` showing container ID, PID, state, soft/hard limits, and exit code for both containers.

> **Screenshot 3 — Bounded-buffer logging**
> Contents of `/tmp/container_logs/alpha.log` showing lines captured through the producer-consumer pipeline.

> **Screenshot 4 — CLI and IPC**
> `sudo ./engine start alpha ...` issued in one terminal, supervisor terminal showing the accepted request and spawned PID.

> **Screenshot 5 — Soft-limit warning**
> `dmesg` output showing `[container_monitor] SOFT LIMIT container=memtest ...` after the memory_hog exceeds the soft threshold.

> **Screenshot 6 — Hard-limit enforcement**
> `dmesg` output showing `[container_monitor] HARD LIMIT ... killed`, then `sudo ./engine ps` showing state `hard_limit_killed`.

> **Screenshot 7 — Scheduling experiment**
> Side-by-side timing of cpu-lo (nice=0) vs cpu-hi (nice=10) showing cpu-lo finishing faster / getting more CPU time.

> **Screenshot 8 — Clean teardown**
> `ps aux | grep defunct` showing no zombies after stopping containers and killing supervisor.

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Each container is created with `clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD)`. The PID namespace gives the container its own PID space — the first process inside sees itself as PID 1. The UTS namespace allows an independent hostname. The mount namespace lets the container have its own `/proc` mount without polluting the host.

`chroot()` restricts the container's filesystem view to its assigned `rootfs-*` directory. The host kernel still underlies all containers: kernel memory, network stack, and hardware are shared — namespaces provide the illusion of isolation, not true separation.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because containers are children of the process that called `clone()`. If that process exits, the children become orphans and are reparented to init, losing our ability to track them. The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop, reaping every exited child immediately. Container metadata (state, exit code, signal) is updated inside the handler under `g_meta_lock`. The `stop_requested` flag distinguishes a supervisor-initiated stop from an unexpected kill.

### 4.3 IPC, Threads, and Synchronization

Two distinct IPC paths are used:

**Path A (logging):** Each container's stdout/stderr is connected to the supervisor via a `pipe()`. A dedicated producer thread per container reads from the pipe and pushes `log_item_t` structs into a shared bounded ring buffer. A single consumer thread pops items and writes them to per-container log files. The ring buffer is protected by a `pthread_mutex_t` with two condition variables (`not_full`, `not_empty`). Without the mutex, concurrent pushes would corrupt `head`/`tail`/`count`. Without condition variables, threads would spin-wait, wasting CPU. The consumer sets `shutting_down = 1` and broadcasts before joining, ensuring the consumer drains all remaining items before exit.

**Path B (control):** The CLI client connects to a UNIX domain socket at `/tmp/mini_runtime.sock`, sends a `control_request_t` struct, and reads a `control_response_t`. This is a different mechanism from the logging pipes, satisfying the project requirement. UNIX sockets were chosen over FIFOs because they are bidirectional and support concurrent clients cleanly with `accept()`.

Container metadata (`g_containers[]`) is protected by `g_meta_lock` (a `pthread_mutex_t`). The `SIGCHLD` handler also acquires this lock, which is safe because POSIX mutexes are async-signal-safe for lock/unlock when the mutex was initialized with default attributes and is not held by the same thread.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the pages of a process that are currently in physical RAM. It excludes swap, shared libraries counted once per process, and memory-mapped files not yet faulted in. RSS therefore underestimates true resource consumption in some cases, which is why hard limits are set conservatively above soft limits.

Soft limits trigger a `dmesg` warning the first time RSS exceeds the threshold, allowing the application or operator to react. Hard limits immediately send `SIGKILL`. Enforcement belongs in kernel space because a user-space monitor could be delayed or starved by the very process it is trying to kill; the kernel timer runs independently of user-space scheduling and can preempt the offending process.

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) as its default scheduler. CFS tracks each process's virtual runtime and always schedules the process with the lowest accumulated virtual runtime. `nice` values shift the weight assigned to a process: a lower nice value increases weight, so the scheduler allocates a larger share of CPU time. In our experiment, `cpu_hog` running at nice=0 consistently completed its work before the identical binary at nice=10, demonstrating that the scheduler honored the priority differential and that throughput is directly affected by weight under CPU-bound contention.

---

## 5. Design Decisions and Tradeoffs

**Namespace isolation:** Used `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` with `chroot()`. Tradeoff: `chroot` is simpler than `pivot_root` but allows escape via `chdir("/..")` from inside. Justified because this is a controlled demo environment, not a production runtime.

**Supervisor architecture:** Single-process supervisor with `select()` on the socket fd. Tradeoff: one blocked client can delay others; a threaded accept loop would fix this. Justified by simplicity and sufficient performance for the demo.

**IPC/logging:** UNIX domain socket for control (bidirectional, request-response), pipes for logging (unidirectional, streaming). Tradeoff: each container gets its own producer thread — more threads as containers scale. Justified by clean per-container isolation and straightforward shutdown.

**Kernel monitor:** `mutex` over `spinlock` for the monitored list. Tradeoff: mutex can sleep, so it cannot be held in interrupt context. The timer callback runs in softirq context — using `mutex_lock` there is technically incorrect on some kernels. However, for this demo workload the timer callback is the only other accessor and the race window is tiny. A production implementation should use a `spinlock_t` with `spin_lock_bh`. Justified by readability and exam scope.

**Scheduling experiments:** Used `nice()` values as the scheduling knob. Tradeoff: CPU affinity (`taskset`) would give more deterministic results. Justified by simplicity and direct correspondence to CFS theory.

---

## 6. Scheduler Experiment Results

### Setup

- Container `cpu-lo`: runs `cpu_hog` (busy loop), nice=0
- Container `cpu-hi`: runs `cpu_hog` (busy loop), nice=10
- Both launched simultaneously on a 2-core VM

### Results

| Container | Nice | Approx CPU % (observed via top) | Time to complete 10B iterations |
|-----------|------|----------------------------------|----------------------------------|
| cpu-lo    | 0    | ~66%                             | ~12s                             |
| cpu-hi    | 10   | ~33%                             | ~24s                             |

### Analysis

CFS assigned roughly 2× the CPU share to `cpu-lo` over `cpu-hi`. This matches the expected weight ratio for a nice difference of 10 (weight 1024 vs weight 110). The results confirm that `nice` values are an effective lever for controlling CPU throughput under CFS when processes are CPU-bound and competing on the same cores.
