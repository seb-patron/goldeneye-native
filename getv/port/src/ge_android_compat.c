/* The four libc functions bionic does not give us, and nothing else.
 *
 * Whole file is inside __ANDROID__ so it contributes nothing anywhere else: on Linux, macOS
 * and Windows these already come from the platform and a second definition would collide.
 *
 * bcopy and bzero are the BSD spellings. The decompilation uses them throughout and PR/os.h
 * redeclares them with the N64's int-length signatures, which is why they cannot simply be
 * swapped for memmove/memset at every call site. bionic dropped them, so they are supplied
 * here with those same signatures, forwarding to the functions that did survive. bcopy takes
 * source first and destination second -- the reverse of memcpy -- and it is defined to handle
 * overlap, so memmove is the correct target and memcpy is not.
 *
 * backtrace and backtrace_symbols_fd are glibc's <execinfo.h>. Android has no equivalent, and
 * the port only calls them from its crash reporter. Stubbed rather than emulated: a crash
 * handler that reports no frames is a poor diagnostic but a working build, and unwinding on
 * arm64 bionic needs a real unwinder library to do properly. Returning 0 frames is the honest
 * answer to "what is on the stack" when we cannot look.
 */
#ifdef __ANDROID__

#include <string.h>
#include <unistd.h>

void bcopy(const void *src, void *dst, size_t n);
void bzero(void *dst, size_t n);
int  backtrace(void **buffer, int size);
void backtrace_symbols_fd(void *const *buffer, int size, int fd);

void bcopy(const void *src, void *dst, size_t n)
{
    /* Source first, destination second, and overlap-safe. */
    memmove(dst, src, n);
}

void bzero(void *dst, size_t n)
{
    memset(dst, 0, n);
}

int backtrace(void **buffer, int size)
{
    (void) buffer;
    (void) size;
    return 0;   /* no frames, rather than a wrong frame count */
}

void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    static const char msg[] = "[getv] backtrace unavailable on Android\n";
    (void) buffer;
    (void) size;
    (void) write(fd, msg, sizeof msg - 1);
}

#endif /* __ANDROID__ */
