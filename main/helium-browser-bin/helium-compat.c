/*
 * Compatibility fixes for Helium, an official glibc build running on musl
 * through gcompat. The APKBUILD makes this library the first DT_NEEDED
 * entry of Helium's binaries, so every Helium process loads it, before
 * musl in symbol lookup order, whatever environment Chromium gives it.
 *
 * clone(): musl rejects CLONE_SETTLS. Chromium's sandbox passes it, with a
 * scratch TLS block, for a CLONE_VM | CLONE_VFORK child that only chroots
 * and exits (ChrootToSafeEmptyDir() in sandbox/linux/services/
 * credentials.cc). The parent stays suspended until that child exits, so
 * the child can keep the parent's thread pointer: drop the flag and call
 * musl's clone().
 *
 * mprotect(): zygote children get a fresh stack canary
 * (base::ResetStackCanaryIfPossible(), --change-stack-guard-on-fork). On
 * arm64 glibc builds, Chromium makes the page holding __stack_chk_guard
 * writable, stores the canary and then makes the page read-only again,
 * because glibc keeps the guard in RELRO memory. musl keeps it in its
 * writable .bss, next to its locks and atfork list, so the child faults
 * on its next write there. Leave that one page writable.
 *
 * freelocale(): gcompat's newlocale() and duplocale() return a glibc-sized
 * locale object from the public calloc()/malloc(), which Chromium
 * replaces, but gcompat leaves freelocale() to musl, which hands the
 * object to musl's internal allocator. Free gcompat's objects with the
 * public free() instead.
 *
 * seccomp: Chromium's syscall policies are written for glibc. musl's
 * pthread_create() also passes CLONE_DETACHED, which the kernel has
 * ignored since Linux 2.6 and the policies reject, and registers for
 * private expedited membarrier, which they do not allow. When a filter is
 * installed through prctl() or syscall(), prepend a prologue that allows
 * clone() with musl's thread flags (glibc's plus CLONE_DETACHED) and the
 * two private expedited membarrier() commands, and fails other
 * membarrier() commands with ENOSYS. Refusing the private ones would send
 * musl to its fallback, which signals every thread with tkill(). It also
 * fails pwritev2() with ENOSYS: musl's pwrite() tries it first (with
 * RWF_NOAPPEND) and falls back to pwrite64() on ENOSYS, while the policies
 * treat pwritev2() as a crash. Everything else falls through to
 * Chromium's program unchanged.
 *
 * raise(), pthread_kill(), abort(): musl signals threads with tkill(), which
 * the policies turn into a crash, and it does so with SIGSYS blocked, so
 * the kernel kills the process without a report. glibc uses tgkill() on
 * its own process, which the policies allow; do the same.
 *
 * Scheduling: musl's sched_getparam(), sched_setparam(),
 * sched_getscheduler() and sched_setscheduler() fail with ENOSYS, while
 * glibc makes the (per-thread) Linux system calls, which Chromium uses for
 * thread priorities. Make the system calls. musl's pthread_getschedparam(),
 * pthread_setschedparam() and pthread_setschedprio() make them with SIGSYS
 * blocked, so the policies' handler for these calls cannot run and the
 * kernel kills the process. Make them without blocking signals.
 *
 * musl keeps a thread's tid at offset 0x20 of its struct pthread. That is
 * checked against gettid() before use, and musl's own pthread functions
 * are used when it does not match.
 *
 * fcntl(): musl adds O_LARGEFILE to every F_SETFL argument. The kernel
 * ignores that flag there, but the policies reject F_SETFL with any flag
 * outside their list, and glibc's arm64 O_LARGEFILE is 0. Pass F_SETFL
 * through unchanged.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/membarrier.h>
#include <linux/seccomp.h>
#include <locale.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

typedef int (*clone_fn)(int (*)(void *), void *, int, void *, ...);

EXPORT int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...)
{
	static clone_fn real;
	va_list ap;
	pid_t *ptid;
	void *tls;
	pid_t *ctid;

	if (!real)
		real = (clone_fn)dlsym(RTLD_NEXT, "clone");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	va_start(ap, arg);
	ptid = va_arg(ap, pid_t *);
	tls = va_arg(ap, void *);
	ctid = va_arg(ap, pid_t *);
	va_end(ap);
	if (flags & CLONE_VM)
		flags &= ~CLONE_SETTLS;
	return real(fn, stack, flags, arg, ptid, tls, ctid);
}

EXPORT int mprotect(void *addr, size_t len, int prot)
{
	static uintptr_t guard_page;
	long page;

	if (prot == PROT_READ) {
		page = sysconf(_SC_PAGESIZE);
		if (!guard_page) {
			void *guard = dlsym(RTLD_DEFAULT, "__stack_chk_guard");

			guard_page = guard ? (uintptr_t)guard & -page : 1;
		}
		if ((uintptr_t)addr == guard_page && len == (size_t)page)
			prot |= PROT_WRITE;
	}
	return syscall(SYS_mprotect, addr, len, prot);
}

typedef void (*freelocale_fn)(locale_t);

EXPORT void freelocale(locale_t loc)
{
	static freelocale_fn real;
	static int gcompat;

	if (!real) {
		/* gcompat, when loaded, comes before this library. */
		gcompat = dlsym(RTLD_DEFAULT, "newlocale") !=
			  dlsym(RTLD_NEXT, "newlocale");
		real = (freelocale_fn)dlsym(RTLD_NEXT, "freelocale");
	}
	if (!gcompat) {
		if (real)
			real(loc);
		return;
	}
	if (loc && loc != LC_GLOBAL_LOCALE)
		free(loc);
}

EXPORT void __freelocale(locale_t loc) __attribute__((alias("freelocale")));

#define MUSL_THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
			   CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | \
			   CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | \
			   CLONE_DETACHED)
#define SECCOMP_ARG_LO(n) (offsetof(struct seccomp_data, args) + 8 * (n))
#define SECCOMP_ARG_HI(n) (SECCOMP_ARG_LO(n) + 4)

static const struct sock_filter seccomp_prologue[] = {
	/* 0 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			offsetof(struct seccomp_data, arch)),
	/* 1 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 0, 15),
	/* 2 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			offsetof(struct seccomp_data, nr)),
	/* 3 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pwritev2, 6, 0),
	/* 4 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_membarrier, 0, 6),
	/* 5 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SECCOMP_ARG_LO(1)),
	/* 6 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3),
	/* 7 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SECCOMP_ARG_LO(0)),
	/* 8 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
			MEMBARRIER_CMD_PRIVATE_EXPEDITED, 7, 0),
	/* 9 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
			MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 6, 0),
	/* 10 */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
	/* 11 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 5),
	/* 12 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SECCOMP_ARG_LO(0)),
	/* 13 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, MUSL_THREAD_FLAGS, 0, 3),
	/* 14 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SECCOMP_ARG_HI(0)),
	/* 15 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
	/* 16 */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	/* 17: Chromium's program */
};

#define PROLOGUE_LEN (sizeof(seccomp_prologue) / sizeof(seccomp_prologue[0]))

/*
 * Returns a copy of @prog with the prologue, or @prog itself when it
 * cannot be extended: the program must start by loading a value, since
 * the prologue leaves its own value in the accumulator.
 */
static const struct sock_fprog *seccomp_extend(const struct sock_fprog *prog,
					       struct sock_fprog *copy)
{
	struct sock_filter *f;

	if (!prog || !prog->filter || !prog->len ||
	    prog->len > BPF_MAXINSNS - PROLOGUE_LEN ||
	    BPF_CLASS(prog->filter[0].code) != BPF_LD)
		return prog;
	f = malloc((prog->len + PROLOGUE_LEN) * sizeof(*f));
	if (!f)
		return prog;
	memcpy(f, seccomp_prologue, sizeof(seccomp_prologue));
	memcpy(f + PROLOGUE_LEN, prog->filter, prog->len * sizeof(*f));
	copy->len = prog->len + PROLOGUE_LEN;
	copy->filter = f;
	return copy;
}

typedef long (*syscall_fn)(long, ...);

EXPORT long syscall(long nr, ...)
{
	static syscall_fn real;
	struct sock_fprog copy = { 0 };
	va_list ap;
	long a[6];
	long ret;
	int i;

	if (!real)
		real = (syscall_fn)dlsym(RTLD_NEXT, "syscall");
	va_start(ap, nr);
	for (i = 0; i < 6; i++)
		a[i] = va_arg(ap, long);
	va_end(ap);
	if (nr == SYS_seccomp && a[0] == SECCOMP_SET_MODE_FILTER)
		a[2] = (long)seccomp_extend((const struct sock_fprog *)a[2],
					    &copy);
	ret = real(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
	free(copy.filter);
	return ret;
}

typedef int (*prctl_fn)(int, ...);

EXPORT int prctl(int option, ...)
{
	static prctl_fn real;
	struct sock_fprog copy = { 0 };
	va_list ap;
	unsigned long a[4];
	int ret;
	int i;

	if (!real)
		real = (prctl_fn)dlsym(RTLD_NEXT, "prctl");
	va_start(ap, option);
	for (i = 0; i < 4; i++)
		a[i] = va_arg(ap, unsigned long);
	va_end(ap);
	if (option == PR_SET_SECCOMP && a[0] == SECCOMP_MODE_FILTER)
		a[1] = (unsigned long)seccomp_extend(
			(const struct sock_fprog *)a[1], &copy);
	ret = real(option, a[0], a[1], a[2], a[3]);
	free(copy.filter);
	return ret;
}

typedef int (*fcntl_fn)(int, int, ...);

EXPORT int fcntl(int fd, int cmd, ...)
{
	static fcntl_fn real;
	unsigned long arg;
	va_list ap;

	va_start(ap, cmd);
	arg = va_arg(ap, unsigned long);
	va_end(ap);
	if (cmd == F_SETFL)
		return syscall(SYS_fcntl, fd, cmd, arg);
	if (!real)
		real = (fcntl_fn)dlsym(RTLD_NEXT, "fcntl");
	return real(fd, cmd, arg);
}

#define MUSL_PTHREAD_TID 0x20

static int tid_of(pthread_t t)
{
	static int usable = -1;

	if (usable < 0)
		usable = *(int *)((char *)pthread_self() + MUSL_PTHREAD_TID) ==
			 gettid();
	return usable ? *(volatile int *)((char *)t + MUSL_PTHREAD_TID) : -1;
}

typedef int (*pthread_kill_fn)(pthread_t, int);

EXPORT int pthread_kill(pthread_t t, int sig)
{
	static pthread_kill_fn real;
	int tid = tid_of(t);

	if (tid < 0) {
		if (!real)
			real = (pthread_kill_fn)dlsym(RTLD_NEXT, "pthread_kill");
		return real(t, sig);
	}
	if ((unsigned)sig >= _NSIG)
		return EINVAL;
	/* An exited thread has tid 0; musl then reports success too. */
	if (!tid)
		return 0;
	return syscall(SYS_tgkill, getpid(), tid, sig) ? errno : 0;
}

EXPORT int raise(int sig)
{
	return syscall(SYS_tgkill, getpid(), gettid(), sig);
}

typedef void (*abort_fn)(void);

EXPORT _Noreturn void abort(void)
{
	abort_fn real = (abort_fn)dlsym(RTLD_NEXT, "abort");

	raise(SIGABRT);
	if (real)
		real();
	__builtin_trap();
}

EXPORT int sched_getparam(pid_t pid, struct sched_param *param)
{
	return syscall(SYS_sched_getparam, pid, param);
}

EXPORT int sched_setparam(pid_t pid, const struct sched_param *param)
{
	return syscall(SYS_sched_setparam, pid, param);
}

EXPORT int sched_getscheduler(pid_t pid)
{
	return syscall(SYS_sched_getscheduler, pid);
}

EXPORT int sched_setscheduler(pid_t pid, int policy,
			      const struct sched_param *param)
{
	return syscall(SYS_sched_setscheduler, pid, policy, param);
}

typedef int (*getschedparam_fn)(pthread_t, int *, struct sched_param *);
typedef int (*setschedparam_fn)(pthread_t, int, const struct sched_param *);
typedef int (*setschedprio_fn)(pthread_t, int);

EXPORT int pthread_getschedparam(pthread_t t, int *policy,
				 struct sched_param *param)
{
	int tid = tid_of(t);
	int ret;

	if (tid < 0)
		return ((getschedparam_fn)dlsym(RTLD_NEXT,
			"pthread_getschedparam"))(t, policy, param);
	if (!tid)
		return ESRCH;
	if (syscall(SYS_sched_getparam, tid, param))
		return errno;
	ret = syscall(SYS_sched_getscheduler, tid);
	if (ret < 0)
		return errno;
	*policy = ret;
	return 0;
}

EXPORT int pthread_setschedparam(pthread_t t, int policy,
				 const struct sched_param *param)
{
	int tid = tid_of(t);

	if (tid < 0)
		return ((setschedparam_fn)dlsym(RTLD_NEXT,
			"pthread_setschedparam"))(t, policy, param);
	if (!tid)
		return ESRCH;
	return syscall(SYS_sched_setscheduler, tid, policy, param) ? errno : 0;
}

EXPORT int pthread_setschedprio(pthread_t t, int prio)
{
	struct sched_param param = { .sched_priority = prio };
	int tid = tid_of(t);

	if (tid < 0)
		return ((setschedprio_fn)dlsym(RTLD_NEXT,
			"pthread_setschedprio"))(t, prio);
	if (!tid)
		return ESRCH;
	return syscall(SYS_sched_setparam, tid, &param) ? errno : 0;
}
