/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#define _BSD_SOURCE
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/capability.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <sys/capability.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libminijail.h"
#include "libminijail-private.h"

#include "signal.h"
#include "syscall_filter.h"
#include "util.h"

#ifdef HAVE_SECUREBITS_H
#include <linux/securebits.h>
#else
#define SECURE_ALL_BITS         0x15
#define SECURE_ALL_LOCKS        (SECURE_ALL_BITS << 1)
#endif

/* Until these are reliably available in linux/prctl.h */
#ifndef PR_SET_SECCOMP
# define PR_SET_SECCOMP 22
#endif

/* For seccomp_filter using BPF. */
#ifndef PR_SET_NO_NEW_PRIVS
# define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef SECCOMP_MODE_FILTER
# define SECCOMP_MODE_FILTER 2 /* uses user-supplied filter. */
#endif

#ifdef USE_SECCOMP_SOFTFAIL
# define SECCOMP_SOFTFAIL 1
#else
# define SECCOMP_SOFTFAIL 0
#endif

struct binding {
	char *src;
	char *dest;
	int writeable;
	struct binding *next;
};

struct minijail {
	/*
	 * WARNING: if you add a flag here you need to make sure it's
	 * accounted for in minijail_pre{enter|exec}() below.
	 */
	struct {
		int uid:1;
		int gid:1;
		int caps:1;
		int vfs:1;
		int enter_vfs:1;
		int pids:1;
		int net:1;
		int seccomp:1;
		int readonly:1;
		int usergroups:1;
		int ptrace:1;
		int no_new_privs:1;
		int seccomp_filter:1;
		int log_seccomp_filter:1;
		int chroot:1;
		int mount_tmp:1;
	} flags;
	uid_t uid;
	gid_t gid;
	gid_t usergid;
	char *user;
	uint64_t caps;
	pid_t initpid;
	int mountns_fd;
	int filter_len;
	int binding_count;
	char *chrootdir;
	struct sock_fprog *filter_prog;
	struct binding *bindings_head;
	struct binding *bindings_tail;
};

/*
 * Strip out flags meant for the parent.
 * We keep things that are not inherited across execve(2) (e.g. capabilities),
 * or are easier to set after execve(2) (e.g. seccomp filters).
 */
void minijail_preenter(struct minijail *j)
{
	j->flags.vfs = 0;
	j->flags.enter_vfs = 0;
	j->flags.readonly = 0;
	j->flags.pids = 0;
}

/*
 * Strip out flags meant for the child.
 * We keep things that are inherited across execve(2).
 */
void minijail_preexec(struct minijail *j)
{
	int vfs = j->flags.vfs;
	int enter_vfs = j->flags.enter_vfs;
	int readonly = j->flags.readonly;
	if (j->user)
		free(j->user);
	j->user = NULL;
	memset(&j->flags, 0, sizeof(j->flags));
	/* Now restore anything we meant to keep. */
	j->flags.vfs = vfs;
	j->flags.enter_vfs = enter_vfs;
	j->flags.readonly = readonly;
	/* Note, |pids| will already have been used before this call. */
}

/* Minijail API. */

struct minijail API *minijail_new(void)
{
	return calloc(1, sizeof(struct minijail));
}

void API minijail_change_uid(struct minijail *j, uid_t uid)
{
	if (uid == 0)
		die("useless change to uid 0");
	j->uid = uid;
	j->flags.uid = 1;
}

void API minijail_change_gid(struct minijail *j, gid_t gid)
{
	if (gid == 0)
		die("useless change to gid 0");
	j->gid = gid;
	j->flags.gid = 1;
}

int API minijail_change_user(struct minijail *j, const char *user)
{
	char *buf = NULL;
	struct passwd pw;
	struct passwd *ppw = NULL;
	ssize_t sz = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (sz == -1)
		sz = 65536;	/* your guess is as good as mine... */

	/*
	 * sysconf(_SC_GETPW_R_SIZE_MAX), under glibc, is documented to return
	 * the maximum needed size of the buffer, so we don't have to search.
	 */
	buf = malloc(sz);
	if (!buf)
		return -ENOMEM;
	getpwnam_r(user, &pw, buf, sz, &ppw);
	/*
	 * We're safe to free the buffer here. The strings inside pw point
	 * inside buf, but we don't use any of them; this leaves the pointers
	 * dangling but it's safe. ppw points at pw if getpwnam_r succeeded.
	 */
	free(buf);
	/* getpwnam_r(3) does *not* set errno when |ppw| is NULL. */
	if (!ppw)
		return -1;
	minijail_change_uid(j, ppw->pw_uid);
	j->user = strdup(user);
	if (!j->user)
		return -ENOMEM;
	j->usergid = ppw->pw_gid;
	return 0;
}

int API minijail_change_group(struct minijail *j, const char *group)
{
	char *buf = NULL;
	struct group gr;
	struct group *pgr = NULL;
	ssize_t sz = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (sz == -1)
		sz = 65536;	/* and mine is as good as yours, really */

	/*
	 * sysconf(_SC_GETGR_R_SIZE_MAX), under glibc, is documented to return
	 * the maximum needed size of the buffer, so we don't have to search.
	 */
	buf = malloc(sz);
	if (!buf)
		return -ENOMEM;
	getgrnam_r(group, &gr, buf, sz, &pgr);
	/*
	 * We're safe to free the buffer here. The strings inside gr point
	 * inside buf, but we don't use any of them; this leaves the pointers
	 * dangling but it's safe. pgr points at gr if getgrnam_r succeeded.
	 */
	free(buf);
	/* getgrnam_r(3) does *not* set errno when |pgr| is NULL. */
	if (!pgr)
		return -1;
	minijail_change_gid(j, pgr->gr_gid);
	return 0;
}

void API minijail_use_seccomp(struct minijail *j)
{
	j->flags.seccomp = 1;
}

void API minijail_no_new_privs(struct minijail *j)
{
	j->flags.no_new_privs = 1;
}

void API minijail_use_seccomp_filter(struct minijail *j)
{
	j->flags.seccomp_filter = 1;
}

void API minijail_log_seccomp_filter_failures(struct minijail *j)
{
	j->flags.log_seccomp_filter = 1;
}

void API minijail_use_caps(struct minijail *j, uint64_t capmask)
{
	j->caps = capmask;
	j->flags.caps = 1;
}

void API minijail_namespace_vfs(struct minijail *j)
{
	j->flags.vfs = 1;
}

void API minijail_namespace_enter_vfs(struct minijail *j, const char *ns_path)
{
	int ns_fd = open(ns_path, O_RDONLY);
	if (ns_fd < 0) {
		pdie("failed to open namespace '%s'", ns_path);
	}
	j->mountns_fd = ns_fd;
	j->flags.enter_vfs = 1;
}

void API minijail_namespace_pids(struct minijail *j)
{
	j->flags.vfs = 1;
	j->flags.readonly = 1;
	j->flags.pids = 1;
}

void API minijail_namespace_net(struct minijail *j)
{
	j->flags.net = 1;
}

void API minijail_remount_readonly(struct minijail *j)
{
	j->flags.vfs = 1;
	j->flags.readonly = 1;
}

void API minijail_inherit_usergroups(struct minijail *j)
{
	j->flags.usergroups = 1;
}

void API minijail_disable_ptrace(struct minijail *j)
{
	j->flags.ptrace = 1;
}

int API minijail_enter_chroot(struct minijail *j, const char *dir)
{
	if (j->chrootdir)
		return -EINVAL;
	j->chrootdir = strdup(dir);
	if (!j->chrootdir)
		return -ENOMEM;
	j->flags.chroot = 1;
	return 0;
}

void API minijail_mount_tmp(struct minijail *j)
{
	j->flags.mount_tmp = 1;
}

int API minijail_bind(struct minijail *j, const char *src, const char *dest,
		      int writeable)
{
	struct binding *b;

	if (*dest != '/')
		return -EINVAL;
	b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	b->dest = strdup(dest);
	if (!b->dest)
		goto error;
	b->src = strdup(src);
	if (!b->src)
		goto error;
	b->writeable = writeable;

	info("bind %s -> %s", src, dest);

	/*
	 * Force vfs namespacing so the bind mounts don't leak out into the
	 * containing vfs namespace.
	 */
	minijail_namespace_vfs(j);

	if (j->bindings_tail)
		j->bindings_tail->next = b;
	else
		j->bindings_head = b;
	j->bindings_tail = b;
	j->binding_count++;

	return 0;

error:
	free(b->src);
	free(b->dest);
	free(b);
	return -ENOMEM;
}

void API minijail_parse_seccomp_filters(struct minijail *j, const char *path)
{
	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, NULL)) {
		if ((errno == ENOSYS) && SECCOMP_SOFTFAIL) {
			warn("not loading seccomp filter, seccomp not supported");
			return;
		}
	}
	FILE *file = fopen(path, "r");
	if (!file) {
		pdie("failed to open seccomp filter file '%s'", path);
	}

	struct sock_fprog *fprog = malloc(sizeof(struct sock_fprog));
	if (compile_filter(file, fprog, j->flags.log_seccomp_filter)) {
		die("failed to compile seccomp filter BPF program in '%s'",
		    path);
	}

	j->filter_len = fprog->len;
	j->filter_prog = fprog;

	fclose(file);
}

struct marshal_state {
	size_t available;
	size_t total;
	char *buf;
};

void marshal_state_init(struct marshal_state *state,
			char *buf, size_t available)
{
	state->available = available;
	state->buf = buf;
	state->total = 0;
}

void marshal_append(struct marshal_state *state,
		    char *src, size_t length)
{
	size_t copy_len = MIN(state->available, length);

	/* Up to |available| will be written. */
	if (copy_len) {
		memcpy(state->buf, src, copy_len);
		state->buf += copy_len;
		state->available -= copy_len;
	}
	/* |total| will contain the expected length. */
	state->total += length;
}

void minijail_marshal_helper(struct marshal_state *state,
			     const struct minijail *j)
{
	struct binding *b = NULL;
	marshal_append(state, (char *)j, sizeof(*j));
	if (j->user)
		marshal_append(state, j->user, strlen(j->user) + 1);
	if (j->chrootdir)
		marshal_append(state, j->chrootdir, strlen(j->chrootdir) + 1);
	if (j->flags.seccomp_filter && j->filter_prog) {
		struct sock_fprog *fp = j->filter_prog;
		marshal_append(state, (char *)fp->filter,
				fp->len * sizeof(struct sock_filter));
	}
	for (b = j->bindings_head; b; b = b->next) {
		marshal_append(state, b->src, strlen(b->src) + 1);
		marshal_append(state, b->dest, strlen(b->dest) + 1);
		marshal_append(state, (char *)&b->writeable,
				sizeof(b->writeable));
	}
}

size_t API minijail_size(const struct minijail *j)
{
	struct marshal_state state;
	marshal_state_init(&state, NULL, 0);
	minijail_marshal_helper(&state, j);
	return state.total;
}

int minijail_marshal(const struct minijail *j, char *buf, size_t available)
{
	struct marshal_state state;
	marshal_state_init(&state, buf, available);
	minijail_marshal_helper(&state, j);
	return (state.total > available);
}

/* consumebytes: consumes @length bytes from a buffer @buf of length @buflength
 * @length    Number of bytes to consume
 * @buf       Buffer to consume from
 * @buflength Size of @buf
 *
 * Returns a pointer to the base of the bytes, or NULL for errors.
 */
void *consumebytes(size_t length, char **buf, size_t *buflength)
{
	char *p = *buf;
	if (length > *buflength)
		return NULL;
	*buf += length;
	*buflength -= length;
	return p;
}

/* consumestr: consumes a C string from a buffer @buf of length @length
 * @buf    Buffer to consume
 * @length Length of buffer
 *
 * Returns a pointer to the base of the string, or NULL for errors.
 */
char *consumestr(char **buf, size_t *buflength)
{
	size_t len = strnlen(*buf, *buflength);
	if (len == *buflength)
		/* There's no null-terminator */
		return NULL;
	return consumebytes(len + 1, buf, buflength);
}

int minijail_unmarshal(struct minijail *j, char *serialized, size_t length)
{
	int i;
	int count;
	int ret = -EINVAL;

	if (length < sizeof(*j))
		goto out;
	memcpy((void *)j, serialized, sizeof(*j));
	serialized += sizeof(*j);
	length -= sizeof(*j);

	/* Potentially stale pointers not used as signals. */
	j->bindings_head = NULL;
	j->bindings_tail = NULL;
	j->filter_prog = NULL;

	if (j->user) {		/* stale pointer */
		char *user = consumestr(&serialized, &length);
		if (!user)
			goto clear_pointers;
		j->user = strdup(user);
		if (!j->user)
			goto clear_pointers;
	}

	if (j->chrootdir) {	/* stale pointer */
		char *chrootdir = consumestr(&serialized, &length);
		if (!chrootdir)
			goto bad_chrootdir;
		j->chrootdir = strdup(chrootdir);
		if (!j->chrootdir)
			goto bad_chrootdir;
	}

	if (j->flags.seccomp_filter && j->filter_len > 0) {
		size_t ninstrs = j->filter_len;
		if (ninstrs > (SIZE_MAX / sizeof(struct sock_filter)) ||
		    ninstrs > USHRT_MAX)
			goto bad_filters;

		size_t program_len = ninstrs * sizeof(struct sock_filter);
		void *program = consumebytes(program_len, &serialized, &length);
		if (!program)
			goto bad_filters;

		j->filter_prog = malloc(sizeof(struct sock_fprog));
		j->filter_prog->len = ninstrs;
		j->filter_prog->filter = malloc(program_len);
		memcpy(j->filter_prog->filter, program, program_len);
	}

	count = j->binding_count;
	j->binding_count = 0;
	for (i = 0; i < count; ++i) {
		int *writeable;
		const char *dest;
		const char *src = consumestr(&serialized, &length);
		if (!src)
			goto bad_bindings;
		dest = consumestr(&serialized, &length);
		if (!dest)
			goto bad_bindings;
		writeable = consumebytes(sizeof(*writeable), &serialized, &length);
		if (!writeable)
			goto bad_bindings;
		if (minijail_bind(j, src, dest, *writeable))
			goto bad_bindings;
	}

	return 0;

bad_bindings:
	if (j->flags.seccomp_filter && j->filter_len > 0) {
		free(j->filter_prog->filter);
		free(j->filter_prog);
	}
bad_filters:
	if (j->chrootdir)
		free(j->chrootdir);
bad_chrootdir:
	if (j->user)
		free(j->user);
clear_pointers:
	j->user = NULL;
	j->chrootdir = NULL;
out:
	return ret;
}

/* bind_one: Applies bindings from @b for @j, recursing as needed.
 * @j Minijail these bindings are for
 * @b Head of list of bindings
 *
 * Returns 0 for success.
 */
int bind_one(const struct minijail *j, struct binding *b)
{
	int ret = 0;
	char *dest = NULL;
	if (ret)
		return ret;
	/* dest has a leading "/" */
	if (asprintf(&dest, "%s%s", j->chrootdir, b->dest) < 0)
		return -ENOMEM;
	ret = mount(b->src, dest, NULL, MS_BIND, NULL);
	if (ret)
		pdie("bind: %s -> %s", b->src, dest);
	if (!b->writeable) {
		ret = mount(b->src, dest, NULL,
			    MS_BIND | MS_REMOUNT | MS_RDONLY, NULL);
		if (ret)
			pdie("bind ro: %s -> %s", b->src, dest);
	}
	free(dest);
	if (b->next)
		return bind_one(j, b->next);
	return ret;
}

int enter_chroot(const struct minijail *j)
{
	int ret;
	if (j->bindings_head && (ret = bind_one(j, j->bindings_head)))
		return ret;

	if (chroot(j->chrootdir))
		return -errno;

	if (chdir("/"))
		return -errno;

	return 0;
}

int mount_tmp(void)
{
	return mount("none", "/tmp", "tmpfs", 0, "size=64M,mode=777");
}

int remount_readonly(void)
{
	const char *kProcPath = "/proc";
	const unsigned int kSafeFlags = MS_NODEV | MS_NOEXEC | MS_NOSUID;
	/*
	 * Right now, we're holding a reference to our parent's old mount of
	 * /proc in our namespace, which means using MS_REMOUNT here would
	 * mutate our parent's mount as well, even though we're in a VFS
	 * namespace (!). Instead, remove their mount from our namespace
	 * and make our own.
	 */
	if (umount(kProcPath))
		return -errno;
	if (mount("", kProcPath, "proc", kSafeFlags | MS_RDONLY, ""))
		return -errno;
	return 0;
}

void drop_ugid(const struct minijail *j)
{
	if (j->flags.usergroups) {
		if (initgroups(j->user, j->usergid))
			pdie("initgroups");
	} else {
		/* Only attempt to clear supplemental groups if we are changing
		 * users. */
		if ((j->uid || j->gid) && setgroups(0, NULL))
			pdie("setgroups");
	}

	if (j->flags.gid && setresgid(j->gid, j->gid, j->gid))
		pdie("setresgid");

	if (j->flags.uid && setresuid(j->uid, j->uid, j->uid))
		pdie("setresuid");
}

/*
 * We specifically do not use cap_valid() as that only tells us the last
 * valid cap we were *compiled* against (i.e. what the version of kernel
 * headers says).  If we run on a different kernel version, then it's not
 * uncommon for that to be less (if an older kernel) or more (if a newer
 * kernel).  So suck up the answer via /proc.
 */
static int run_cap_valid(unsigned int cap)
{
	static unsigned int last_cap;

	if (!last_cap) {
		const char cap_file[] = "/proc/sys/kernel/cap_last_cap";
		FILE *fp = fopen(cap_file, "re");
		if (fscanf(fp, "%u", &last_cap) != 1)
			pdie("fscanf(%s)", cap_file);
		fclose(fp);
	}

	return cap <= last_cap;
}

void drop_caps(const struct minijail *j)
{
	cap_t caps = cap_get_proc();
	cap_value_t flag[1];
	const uint64_t one = 1;
	unsigned int i;
	if (!caps)
		die("can't get process caps");
	if (cap_clear_flag(caps, CAP_INHERITABLE))
		die("can't clear inheritable caps");
	if (cap_clear_flag(caps, CAP_EFFECTIVE))
		die("can't clear effective caps");
	if (cap_clear_flag(caps, CAP_PERMITTED))
		die("can't clear permitted caps");
	for (i = 0; i < sizeof(j->caps) * 8 && run_cap_valid(i); ++i) {
		/* Keep CAP_SETPCAP for dropping bounding set bits. */
		if (i != CAP_SETPCAP && !(j->caps & (one << i)))
			continue;
		flag[0] = i;
		if (cap_set_flag(caps, CAP_EFFECTIVE, 1, flag, CAP_SET))
			die("can't add effective cap");
		if (cap_set_flag(caps, CAP_PERMITTED, 1, flag, CAP_SET))
			die("can't add permitted cap");
		if (cap_set_flag(caps, CAP_INHERITABLE, 1, flag, CAP_SET))
			die("can't add inheritable cap");
	}
	if (cap_set_proc(caps))
		die("can't apply initial cleaned capset");

	/*
	 * Instead of dropping bounding set first, do it here in case
	 * the caller had a more permissive bounding set which could
	 * have been used above to raise a capability that wasn't already
	 * present. This requires CAP_SETPCAP, so we raised/kept it above.
	 */
	for (i = 0; i < sizeof(j->caps) * 8 && run_cap_valid(i); ++i) {
		if (j->caps & (one << i))
			continue;
		if (prctl(PR_CAPBSET_DROP, i))
			pdie("prctl(PR_CAPBSET_DROP)");
	}

	/* If CAP_SETPCAP wasn't specifically requested, now we remove it. */
	if ((j->caps & (one << CAP_SETPCAP)) == 0) {
		flag[0] = CAP_SETPCAP;
		if (cap_set_flag(caps, CAP_EFFECTIVE, 1, flag, CAP_CLEAR))
			die("can't clear effective cap");
		if (cap_set_flag(caps, CAP_PERMITTED, 1, flag, CAP_CLEAR))
			die("can't clear permitted cap");
		if (cap_set_flag(caps, CAP_INHERITABLE, 1, flag, CAP_CLEAR))
			die("can't clear inheritable cap");
	}

	if (cap_set_proc(caps))
		die("can't apply final cleaned capset");

	cap_free(caps);
}

void set_seccomp_filter(const struct minijail *j)
{
	/*
	 * Set no_new_privs. See </kernel/seccomp.c> and </kernel/sys.c>
	 * in the kernel source tree for an explanation of the parameters.
	 */
	if (j->flags.no_new_privs) {
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
			pdie("prctl(PR_SET_NO_NEW_PRIVS)");
	}

	/*
	 * If we're logging seccomp filter failures,
	 * install the SIGSYS handler first.
	 */
	if (j->flags.seccomp_filter && j->flags.log_seccomp_filter) {
		if (install_sigsys_handler())
			pdie("install SIGSYS handler");
		warn("logging seccomp filter failures");
	}

	/*
	 * Install the syscall filter.
	 */
	if (j->flags.seccomp_filter) {
		if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, j->filter_prog)) {
			if ((errno == ENOSYS) && SECCOMP_SOFTFAIL) {
				warn("seccomp not supported");
				return;
			}
			pdie("prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER)");
		}
	}
}

void API minijail_enter(const struct minijail *j)
{
	if (j->flags.pids)
		die("tried to enter a pid-namespaced jail;"
		    " try minijail_run()?");

	if (j->flags.usergroups && !j->user)
		die("usergroup inheritance without username");

	/*
	 * We can't recover from failures if we've dropped privileges partially,
	 * so we don't even try. If any of our operations fail, we abort() the
	 * entire process.
	 */
	if (j->flags.enter_vfs && setns(j->mountns_fd, CLONE_NEWNS))
		pdie("setns(CLONE_NEWNS)");

	if (j->flags.vfs && unshare(CLONE_NEWNS))
		pdie("unshare(vfs)");

	if (j->flags.net && unshare(CLONE_NEWNET))
		pdie("unshare(net)");

	if (j->flags.chroot && enter_chroot(j))
		pdie("chroot");

	if (j->flags.mount_tmp && mount_tmp())
		pdie("mount_tmp");

	if (j->flags.readonly && remount_readonly())
		pdie("remount");

	if (j->flags.caps) {
		/*
		 * POSIX capabilities are a bit tricky. If we drop our
		 * capability to change uids, our attempt to use setuid()
		 * below will fail. Hang on to root caps across setuid(), then
		 * lock securebits.
		 */
		if (prctl(PR_SET_KEEPCAPS, 1))
			pdie("prctl(PR_SET_KEEPCAPS)");
		if (prctl
		    (PR_SET_SECUREBITS, SECURE_ALL_BITS | SECURE_ALL_LOCKS))
			pdie("prctl(PR_SET_SECUREBITS)");
	}

	/*
	 * If we're setting no_new_privs, we can drop privileges
	 * before setting seccomp filter. This way filter policies
	 * don't need to allow privilege-dropping syscalls.
	 */
	if (j->flags.no_new_privs) {
		drop_ugid(j);
		if (j->flags.caps)
			drop_caps(j);

		set_seccomp_filter(j);
	} else {
		/*
		 * If we're not setting no_new_privs,
		 * we need to set seccomp filter *before* dropping privileges.
		 * WARNING: this means that filter policies *must* allow
		 * setgroups()/setresgid()/setresuid() for dropping root and
		 * capget()/capset()/prctl() for dropping caps.
		 */
		set_seccomp_filter(j);

		drop_ugid(j);
		if (j->flags.caps)
			drop_caps(j);
	}

	/*
	 * seccomp has to come last since it cuts off all the other
	 * privilege-dropping syscalls :)
	 */
	if (j->flags.seccomp && prctl(PR_SET_SECCOMP, 1)) {
		if ((errno == ENOSYS) && SECCOMP_SOFTFAIL) {
			warn("seccomp not supported");
			return;
		}
		pdie("prctl(PR_SET_SECCOMP)");
	}
}

/* TODO(wad) will visibility affect this variable? */
static int init_exitstatus = 0;

void init_term(int __attribute__ ((unused)) sig)
{
	_exit(init_exitstatus);
}

int init(pid_t rootpid)
{
	pid_t pid;
	int status;
	/* so that we exit with the right status */
	signal(SIGTERM, init_term);
	/* TODO(wad) self jail with seccomp_filters here. */
	while ((pid = wait(&status)) > 0) {
		/*
		 * This loop will only end when either there are no processes
		 * left inside our pid namespace or we get a signal.
		 */
		if (pid == rootpid)
			init_exitstatus = status;
	}
	if (!WIFEXITED(init_exitstatus))
		_exit(MINIJAIL_ERR_INIT);
	_exit(WEXITSTATUS(init_exitstatus));
}

int API minijail_from_fd(int fd, struct minijail *j)
{
	size_t sz = 0;
	size_t bytes = read(fd, &sz, sizeof(sz));
	char *buf;
	int r;
	if (sizeof(sz) != bytes)
		return -EINVAL;
	if (sz > USHRT_MAX)	/* Arbitrary sanity check */
		return -E2BIG;
	buf = malloc(sz);
	if (!buf)
		return -ENOMEM;
	bytes = read(fd, buf, sz);
	if (bytes != sz) {
		free(buf);
		return -EINVAL;
	}
	r = minijail_unmarshal(j, buf, sz);
	free(buf);
	return r;
}

int API minijail_to_fd(struct minijail *j, int fd)
{
	char *buf;
	size_t sz = minijail_size(j);
	ssize_t written;
	int r;

	if (!sz)
		return -EINVAL;
	buf = malloc(sz);
	r = minijail_marshal(j, buf, sz);
	if (r) {
		free(buf);
		return r;
	}
	/* Sends [size][minijail]. */
	written = write(fd, &sz, sizeof(sz));
	if (written != sizeof(sz)) {
		free(buf);
		return -EFAULT;
	}
	written = write(fd, buf, sz);
	if (written < 0 || (size_t) written != sz) {
		free(buf);
		return -EFAULT;
	}
	free(buf);
	return 0;
}

int setup_preload(void)
{
	char *oldenv = getenv(kLdPreloadEnvVar) ? : "";
	char *newenv = malloc(strlen(oldenv) + 2 + strlen(PRELOADPATH));
	if (!newenv)
		return -ENOMEM;

	/* Only insert a separating space if we have something to separate... */
	sprintf(newenv, "%s%s%s", oldenv, strlen(oldenv) ? " " : "",
		PRELOADPATH);

	/* setenv() makes a copy of the string we give it */
	setenv(kLdPreloadEnvVar, newenv, 1);
	free(newenv);
	return 0;
}

int setup_pipe(int fds[2])
{
	int r = pipe(fds);
	char fd_buf[11];
	if (r)
		return r;
	r = snprintf(fd_buf, sizeof(fd_buf), "%d", fds[0]);
	if (r <= 0)
		return -EINVAL;
	setenv(kFdEnvVar, fd_buf, 1);
	return 0;
}

int setup_pipe_end(int fds[2], size_t index)
{
	if (index > 1)
		return -1;

	close(fds[1 - index]);
	return fds[index];
}

int setup_and_dupe_pipe_end(int fds[2], size_t index, int fd)
{
	if (index > 1)
		return -1;

	close(fds[1 - index]);
	/* dup2(2) the corresponding end of the pipe into |fd|. */
	return dup2(fds[index], fd);
}

int API minijail_run(struct minijail *j, const char *filename,
		     char *const argv[])
{
	return minijail_run_pid_pipes(j, filename, argv,
				      NULL, NULL, NULL, NULL);
}

int API minijail_run_pid(struct minijail *j, const char *filename,
			 char *const argv[], pid_t *pchild_pid)
{
	return minijail_run_pid_pipes(j, filename, argv, pchild_pid,
				      NULL, NULL, NULL);
}

int API minijail_run_pipe(struct minijail *j, const char *filename,
			  char *const argv[], int *pstdin_fd)
{
	return minijail_run_pid_pipes(j, filename, argv, NULL, pstdin_fd,
				      NULL, NULL);
}

int API minijail_run_pid_pipe(struct minijail *j, const char *filename,
			      char *const argv[], pid_t *pchild_pid,
			      int *pstdin_fd)
{
	return minijail_run_pid_pipes(j, filename, argv, pchild_pid, pstdin_fd,
				      NULL, NULL);
}

int API minijail_run_pid_pipes(struct minijail *j, const char *filename,
			       char *const argv[], pid_t *pchild_pid,
			       int *pstdin_fd, int *pstdout_fd, int *pstderr_fd)
{
	char *oldenv, *oldenv_copy = NULL;
	pid_t child_pid;
	int pipe_fds[2];
	int stdin_fds[2];
	int stdout_fds[2];
	int stderr_fds[2];
	int ret;
	/* We need to remember this across the minijail_preexec() call. */
	int pid_namespace = j->flags.pids;

	oldenv = getenv(kLdPreloadEnvVar);
	if (oldenv) {
		oldenv_copy = strdup(oldenv);
		if (!oldenv_copy)
			return -ENOMEM;
	}

	if (setup_preload())
		return -EFAULT;

	/*
	 * Before we fork(2) and execve(2) the child process, we need to open
	 * a pipe(2) to send the minijail configuration over.
	 */
	if (setup_pipe(pipe_fds))
		return -EFAULT;

	/*
	 * If we want to write to the child process' standard input,
	 * create the pipe(2) now.
	 */
	if (pstdin_fd) {
		if (pipe(stdin_fds))
			return -EFAULT;
	}

	/*
	 * If we want to read from the child process' standard output,
	 * create the pipe(2) now.
	 */
	if (pstdout_fd) {
		if (pipe(stdout_fds))
			return -EFAULT;
	}

	/*
	 * If we want to read from the child process' standard error,
	 * create the pipe(2) now.
	 */
	if (pstderr_fd) {
		if (pipe(stderr_fds))
			return -EFAULT;
	}

	/* Use sys_clone() if and only if we're creating a pid namespace.
	 *
	 * tl;dr: WARNING: do not mix pid namespaces and multithreading.
	 *
	 * In multithreaded programs, there are a bunch of locks inside libc,
	 * some of which may be held by other threads at the time that we call
	 * minijail_run_pid(). If we call fork(), glibc does its level best to
	 * ensure that we hold all of these locks before it calls clone()
	 * internally and drop them after clone() returns, but when we call
	 * sys_clone(2) directly, all that gets bypassed and we end up with a
	 * child address space where some of libc's important locks are held by
	 * other threads (which did not get cloned, and hence will never release
	 * those locks). This is okay so long as we call exec() immediately
	 * after, but a bunch of seemingly-innocent libc functions like setenv()
	 * take locks.
	 *
	 * Hence, only call sys_clone() if we need to, in order to get at pid
	 * namespacing. If we follow this path, the child's address space might
	 * have broken locks; you may only call functions that do not acquire
	 * any locks.
	 *
	 * Unfortunately, fork() acquires every lock it can get its hands on, as
	 * previously detailed, so this function is highly likely to deadlock
	 * later on (see "deadlock here") if we're multithreaded.
	 *
	 * We might hack around this by having the clone()d child (init of the
	 * pid namespace) return directly, rather than leaving the clone()d
	 * process hanging around to be init for the new namespace (and having
	 * its fork()ed child return in turn), but that process would be crippled
	 * with its libc locks potentially broken. We might try fork()ing in the
	 * parent before we clone() to ensure that we own all the locks, but
	 * then we have to have the forked child hanging around consuming
	 * resources (and possibly having file descriptors / shared memory
	 * regions / etc attached). We'd need to keep the child around to avoid
	 * having its children get reparented to init.
	 *
	 * TODO(ellyjones): figure out if the "forked child hanging around"
	 * problem is fixable or not. It would be nice if we worked in this
	 * case.
	 */
	if (pid_namespace)
		child_pid = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, NULL);
	else
		child_pid = fork();

	if (child_pid < 0) {
		free(oldenv_copy);
		die("failed to fork child");
	}

	if (child_pid) {
		/* Restore parent's LD_PRELOAD. */
		if (oldenv_copy) {
			setenv(kLdPreloadEnvVar, oldenv_copy, 1);
			free(oldenv_copy);
		} else {
			unsetenv(kLdPreloadEnvVar);
		}
		unsetenv(kFdEnvVar);

		j->initpid = child_pid;

		/* Send marshalled minijail. */
		close(pipe_fds[0]);	/* read endpoint */
		ret = minijail_to_fd(j, pipe_fds[1]);
		close(pipe_fds[1]);	/* write endpoint */
		if (ret) {
			kill(j->initpid, SIGKILL);
			die("failed to send marshalled minijail");
		}

		if (pchild_pid)
			*pchild_pid = child_pid;

		/*
		 * If we want to write to the child process' standard input,
		 * set up the write end of the pipe.
		 */
		if (pstdin_fd)
			*pstdin_fd = setup_pipe_end(stdin_fds,
						    1	/* write end */);

		/*
		 * If we want to read from the child process' standard output,
		 * set up the read end of the pipe.
		 */
		if (pstdout_fd)
			*pstdout_fd = setup_pipe_end(stdout_fds,
						     0	/* read end */);

		/*
		 * If we want to read from the child process' standard error,
		 * set up the read end of the pipe.
		 */
		if (pstderr_fd)
			*pstderr_fd = setup_pipe_end(stderr_fds,
						     0	/* read end */);

		return 0;
	}
	free(oldenv_copy);

	/*
	 * If we want to write to the jailed process' standard input,
	 * set up the read end of the pipe.
	 */
	if (pstdin_fd) {
		if (setup_and_dupe_pipe_end(stdin_fds, 0 /* read end */,
					    STDIN_FILENO) < 0)
			die("failed to set up stdin pipe");
	}

	/*
	 * If we want to read from the jailed process' standard output,
	 * set up the write end of the pipe.
	 */
	if (pstdout_fd) {
		if (setup_and_dupe_pipe_end(stdout_fds, 1 /* write end */,
					    STDOUT_FILENO) < 0)
			die("failed to set up stdout pipe");
	}

	/*
	 * If we want to read from the jailed process' standard error,
	 * set up the write end of the pipe.
	 */
	if (pstderr_fd) {
		if (setup_and_dupe_pipe_end(stderr_fds, 1 /* write end */,
					    STDERR_FILENO) < 0)
			die("failed to set up stderr pipe");
	}

	/* Strip out flags that cannot be inherited across execve. */
	minijail_preexec(j);
	/* Jail this process and its descendants... */
	minijail_enter(j);

	if (pid_namespace) {
		/*
		 * pid namespace: this process will become init inside the new
		 * namespace, so fork off a child to actually run the program
		 * (we don't want all programs we might exec to have to know
		 * how to be init).
		 *
		 * If we're multithreaded, we'll probably deadlock here. See
		 * WARNING above.
		 */
		child_pid = fork();
		if (child_pid < 0)
			_exit(child_pid);
		else if (child_pid > 0)
			init(child_pid);	/* never returns */
	}

	/*
	 * If we aren't pid-namespaced:
	 *   calling process
	 *   -> execve()-ing process
	 * If we are:
	 *   calling process
	 *   -> init()-ing process
	 *      -> execve()-ing process
	 */
	_exit(execve(filename, argv, environ));
}

int API minijail_run_static(struct minijail *j, const char *filename,
			    char *const argv[])
{
	pid_t child_pid;
	int pid_namespace = j->flags.pids;

	if (j->flags.caps)
		die("caps not supported with static targets");

	if (pid_namespace)
		child_pid = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, NULL);
	else
		child_pid = fork();

	if (child_pid < 0) {
		die("failed to fork child");
	}
	if (child_pid > 0 ) {
		j->initpid = child_pid;
		return 0;
	}

	/*
	 * We can now drop this child into the sandbox
	 * then execve the target.
	 */

	j->flags.pids = 0;
	minijail_enter(j);

	if (pid_namespace) {
		/*
		 * pid namespace: this process will become init inside the new
		 * namespace, so fork off a child to actually run the program
		 * (we don't want all programs we might exec to have to know
		 * how to be init).
		 *
		 * If we're multithreaded, we'll probably deadlock here. See
		 * WARNING above.
		 */
		child_pid = fork();
		if (child_pid < 0)
			_exit(child_pid);
		else if (child_pid > 0)
			init(child_pid);	/* never returns */
	}

	_exit(execve(filename, argv, environ));
}

int API minijail_kill(struct minijail *j)
{
	int st;
	if (kill(j->initpid, SIGTERM))
		return -errno;
	if (waitpid(j->initpid, &st, 0) < 0)
		return -errno;
	return st;
}

int API minijail_wait(struct minijail *j)
{
	int st;
	if (waitpid(j->initpid, &st, 0) < 0)
		return -errno;

	if (!WIFEXITED(st)) {
		int error_status = st;
		if (WIFSIGNALED(st)) {
			int signum = WTERMSIG(st);
			warn("child process %d received signal %d",
			     j->initpid, signum);
			/*
			 * We return MINIJAIL_ERR_JAIL if the process received
			 * SIGSYS, which happens when a syscall is blocked by
			 * seccomp filters.
			 * If not, we do what bash(1) does:
			 * $? = 128 + signum
			 */
			if (signum == SIGSYS) {
				error_status = MINIJAIL_ERR_JAIL;
			} else {
				error_status = 128 + signum;
			}
		}
		return error_status;
	}

	int exit_status = WEXITSTATUS(st);
	if (exit_status != 0)
		info("child process %d exited with status %d",
		     j->initpid, exit_status);

	return exit_status;
}

void API minijail_destroy(struct minijail *j)
{
	if (j->flags.seccomp_filter && j->filter_prog) {
		free(j->filter_prog->filter);
		free(j->filter_prog);
	}
	while (j->bindings_head) {
		struct binding *b = j->bindings_head;
		j->bindings_head = j->bindings_head->next;
		free(b->dest);
		free(b->src);
		free(b);
	}
	j->bindings_tail = NULL;
	if (j->user)
		free(j->user);
	if (j->chrootdir)
		free(j->chrootdir);
	free(j);
}
