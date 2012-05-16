/**
 * Hack for yet missing fanotify interface in glibc.
 * Does the syscalls by itself.
 *
 * TODO: Delete this as soon a glibc release supports this.
 */
#ifndef FANOTIFY_MARK_SYSCALL
#define FANOTIFY_MARK_SYSCALL

#define __EXPORTED_HEADERS__
#include <linux/types.h>
#undef __EXPORTED_HEADERS__

#include <unistd.h>

#if defined(__x86_64__)
# define __NR_fanotify_mark	301
#elif defined(__i386__)
# define __NR_fanotify_mark	339
#else
# error "System call numbers not defined for this architecture"
#endif

static int fanotify_mark(int fanotify_fd, unsigned int flags, __u64 mask,
		  int dfd, const char *pathname)
{
	return syscall(__NR_fanotify_mark, fanotify_fd, flags, mask,
		       dfd, pathname);
}

#endif
