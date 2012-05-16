/**
 * Hack for yet missing fanotify interface in glibc.
 * Does the syscalls by itself.
 *
 * TODO: Delete this as soon a glibc release supports this.
 */
#ifndef FANOTIFY_INIT_SYSCALL
#define FANOTIFY_INIT_SYSCALL

#define __EXPORTED_HEADERS__
#include <linux/types.h>
#undef __EXPORTED_HEADERS__

#include <unistd.h>

#if defined(__x86_64__)
# define __NR_fanotify_init	300
#elif defined(__i386__)
# define __NR_fanotify_init	338
#else
# error "System call numbers not defined for this architecture"
#endif

static int fanotify_init(unsigned int flags, unsigned int event_f_flags)
{
	return syscall(__NR_fanotify_init, flags, event_f_flags);
}

#endif
