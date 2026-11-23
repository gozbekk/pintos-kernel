# Pintos: Thread Scheduling and User Programs

Low-level systems programming in C on **Pintos**, a small x86 operating system kernel developed at Stanford. Pintos starts as a minimal skeleton: the scheduler, synchronisation and the boundary between user programs and the kernel are left to build.

In this project I extended the kernel in two areas: **thread scheduling** (alarm clock, priority scheduling with donation, and a multi-level feedback queue scheduler) and **user programs** (argument passing, system calls, process synchronisation). It runs on emulated x86 hardware with QEMU.

![C](https://img.shields.io/badge/language-C-blue) ![x86](https://img.shields.io/badge/arch-x86-lightgrey) ![QEMU](https://img.shields.io/badge/runs%20on-QEMU-orange)

---

## Why this project matters

An operating system has to decide **which program runs on the CPU, when, and for how long**. It also has to keep user programs isolated from the kernel. This project implements both of these jobs directly inside a real (small) kernel:

- no busy-waiting: threads sleep and the kernel wakes them at the right time;
- a fair scheduler that avoids *priority inversion*;
- a safe interface (system calls) between user programs and the kernel.

---

## Part 1: Threads

| Feature | What I implemented |
|---|---|
| **Alarm clock** | Replaced the busy-waiting `timer_sleep()` with a blocking version. Each thread stores a `wake_tick`, and the timer interrupt wakes it at the right tick. The CPU is no longer wasted while threads wait. |
| **Priority scheduling** | The ready queue, semaphores and condition variables always choose the highest-priority thread. A thread gives up the CPU right away when a higher-priority thread becomes ready. |
| **Priority donation** | When a high-priority thread waits for a lock held by a low-priority thread, it *donates* its priority to the lock holder. This fixes priority inversion, and the original priority comes back when the lock is released. |
| **Advanced scheduler (MLFQS)** | A 4.4BSD-style multi-level feedback queue scheduler. Priorities are recomputed from `nice`, `recent_cpu` and the system `load_avg`, using **17.14 fixed-point arithmetic** (`fixed-point.h`), because the kernel has no floating point. |

## Part 2: User programs

| Feature | What I implemented |
|---|---|
| **Argument passing** | The command line is split with `strtok_r()`, and the arguments are pushed onto the user stack following the **80x86 calling convention** (right to left, word-aligned, with `argv`/`argc` and a fake return address). |
| **System calls** | `halt`, `exit`, `exec`, `wait`, `create`, `remove`, `open`, `filesize`, `read`, `write`, `seek`, `tell`, `close` |
| **Memory safety** | Every pointer, string and buffer coming from user space is checked (`is_user_vaddr`, page table lookup) before the kernel uses it. A bad pointer kills the process, not the kernel. |
| **Process synchronisation** | A `child_info` structure shared by parent and child, with a semaphore: `exec` only returns once the child's load result is known, and `wait` returns the child's exit status even if the child finished first. A running executable cannot be modified (write-denied). |

---

## Where to look in the code

| File | Content |
|---|---|
| [`src/devices/timer.c`](src/devices/timer.c) | Alarm clock (`timer_sleep`, timer interrupt) |
| [`src/threads/thread.c`](src/threads/thread.c) | Priority scheduling, MLFQS (`load_avg`, `recent_cpu`) |
| [`src/threads/synch.c`](src/threads/synch.c) | Priority donation, priority-aware semaphores and condition variables |
| [`src/threads/fixed-point.h`](src/threads/fixed-point.h) | 17.14 fixed-point arithmetic |
| [`src/userprog/process.c`](src/userprog/process.c) | Program loading, argument passing, `process_wait` |
| [`src/userprog/syscall.c`](src/userprog/syscall.c) | System call handler and user-pointer checks |

## Design documents

The [design document](docs/design.pdf) explains the data structures, algorithms, synchronisation choices and trade-offs of each part, and the known limitations.

---

## Build and run

You need an **i386 cross-compiler toolchain** (`i386-elf-gcc`) and **QEMU**.

```bash
# Part 1: threads
cd src/threads
make
make check          # runs the official Pintos test suite

# Part 2: user programs
cd src/userprog
make
make check
```

## What I learned

- How a kernel schedules threads, and why interrupts must be disabled around shared state
- Real synchronisation problems: priority inversion and race conditions between parent and child processes
- How user and kernel space are separated, and why every user pointer must be checked
- Doing fixed-point math in an environment without a floating-point unit

---

## Credits

Pintos was written by Ben Pfaff and others at Stanford University. Its original licence is in [`src/LICENSE`](src/LICENSE).
The scheduling, synchronisation, argument passing and system call code in this repository is my own work.

**Görkem Özbek**, Galatasaray University
