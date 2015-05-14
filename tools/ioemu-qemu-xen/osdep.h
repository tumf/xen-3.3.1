#ifndef QEMU_OSDEP_H
#define QEMU_OSDEP_H

#include <stdarg.h>
#include <sys/types.h>

#ifndef glue
#define xglue(x, y) x ## y
#define glue(x, y) xglue(x, y)
#define stringify(s)	tostring(s)
#define tostring(s)	#s
#endif

#ifndef likely
#if __GNUC__ < 3
#define __builtin_expect(x, n) (x)
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x)   __builtin_expect(!!(x), 0)
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef always_inline
#if (__GNUC__ < 3) || defined(__APPLE__)
#define always_inline inline
#else
#define always_inline __attribute__ (( always_inline )) __inline__
#endif
#endif
#define inline always_inline

#ifdef __i386__
#define REGPARM __attribute((regparm(3)))
#else
#define REGPARM
#endif

#define qemu_printf printf

void *qemu_memalign(size_t alignment, size_t size);
void *qemu_vmalloc(size_t size);
void qemu_vfree(void *ptr);

ssize_t qemu_read(int fd, void *buf, size_t count);
ssize_t qemu_write(int fd, const void *buf, size_t count);
 /* Repeatedly call read/write until the request is satisfied or an error
  * occurs, and then returns what read would have done.  If it returns
  * a short read then errno is set, or zero if it was EOF. */

int qemu_read_ok(int fd, void *buf, size_t count);
int qemu_write_ok(int fd, const void *buf, size_t count);
 /* Even more simplified versions which return 1 on success or -1 on
  * failure.  EOF counts as failure but then errno is set to 0. */

void *get_mmap_addr(unsigned long size);

int qemu_create_pidfile(const char *filename);

#ifdef _WIN32
int ffs(int i);

typedef struct {
    long tv_sec;
    long tv_usec;
} qemu_timeval;
int qemu_gettimeofday(qemu_timeval *tp);
#else
typedef struct timeval qemu_timeval;
#define qemu_gettimeofday(tp) gettimeofday(tp, NULL);
#endif /* !_WIN32 */

#endif
