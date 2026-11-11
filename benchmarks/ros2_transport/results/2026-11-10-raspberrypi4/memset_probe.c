#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MIN_SIZE ((size_t)1000000)
#define SIZE_6220816 ((size_t)6220816)
#define SIZE_6220800 ((size_t)6220800)

typedef void *(*memset_fn)(void *, int, size_t);

struct tally {
    uint64_t calls;
    uint64_t bytes;
    uint64_t cpu_ns;
    uint64_t skipped;
};

static memset_fn next_memset;
static __thread int resolving;
static int skip_exact_zero;
static struct tally zero_total;
static struct tally nonzero_total;
static struct tally zero_6220816;
static struct tally nonzero_6220816;
static struct tally zero_6220800;
static struct tally nonzero_6220800;
static struct tally zero_other;
static struct tally nonzero_other;

static memset_fn resolve_memset(void) {
    if (next_memset != 0 || resolving) {
        return next_memset;
    }
    resolving = 1;
    next_memset = (memset_fn)dlsym(RTLD_NEXT, "memset");
    resolving = 0;
    return next_memset;
}

static void fallback_memset(void *destination, unsigned char value, size_t size) {
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = value;
    }
}

static uint64_t elapsed_ns(const struct timespec *start, const struct timespec *end) {
    int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nanos = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    int64_t total = seconds * 1000000000LL + nanos;
    return total > 0 ? (uint64_t)total : 0;
}

static void add_tally(struct tally *tally, size_t size, uint64_t duration, int skipped) {
    __atomic_fetch_add(&tally->calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&tally->bytes, (uint64_t)size, __ATOMIC_RELAXED);
    __atomic_fetch_add(&tally->cpu_ns, duration, __ATOMIC_RELAXED);
    __atomic_fetch_add(&tally->skipped, skipped ? 1 : 0, __ATOMIC_RELAXED);
}

static int same_text(const char *left, const char *right) {
    size_t index = 0;
    while (left[index] != '\0' && right[index] != '\0' && left[index] == right[index]) {
        ++index;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static int executable_is_rbt_benchmark(void) {
    char path[512];
    long length = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0 || (size_t)length >= sizeof(path) - 1) {
        return 0;
    }
    path[length] = '\0';
    const char *base = path;
    for (size_t index = 0; path[index] != '\0'; ++index) {
        if (path[index] == '/') {
            base = path + index + 1;
        }
    }
    return same_text(base, "rbt_transport_bench");
}

static int is_publisher_process(void) {
    char arguments[2048];
    long length = syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline", 0, 0);
    if (length < 0) {
        return 0;
    }
    int descriptor = (int)length;
    length = syscall(SYS_read, descriptor, arguments, sizeof(arguments));
    syscall(SYS_close, descriptor);
    if (length <= 0) {
        return 0;
    }
    for (long index = 0; index + 6 < length; ++index) {
        if (arguments[index] == '-' && arguments[index + 1] == '-' &&
            arguments[index + 2] == 'r' && arguments[index + 3] == 'o' &&
            arguments[index + 4] == 'l' && arguments[index + 5] == 'e' &&
            arguments[index + 6] == '\0' && index + 12 < length) {
            if (arguments[index + 7] == 'p' && arguments[index + 8] == 'u' &&
                arguments[index + 9] == 'b' && arguments[index + 10] == '\0') {
                return 1;
            }
        }
    }
    return 0;
}

__attribute__((constructor)) static void initialize_probe(void) {
    (void)resolve_memset();
    const char *skip = getenv("RBT_DIAG_SKIP_VIDEO_ZERO");
    skip_exact_zero = skip != 0 && skip[0] == '1' && skip[1] == '\0' &&
        executable_is_rbt_benchmark() && is_publisher_process();
}

void *memset(void *destination, int value, size_t size) {
    memset_fn function = next_memset;
    if (function != 0 && size < MIN_SIZE) {
        return function(destination, value, size);
    }
    int saved_errno = errno;
    if (function == 0) {
        function = resolve_memset();
    }
    if (function == 0) {
        fallback_memset(destination, (unsigned char)value, size);
        errno = saved_errno;
        return destination;
    }
    if (size < MIN_SIZE) {
        errno = saved_errno;
        return function(destination, value, size);
    }

    unsigned char byte_value = (unsigned char)value;
    int is_zero = byte_value == 0;
    int skipped = skip_exact_zero && is_zero && size == SIZE_6220816;
    struct timespec start = {0, 0};
    struct timespec end = {0, 0};
    int timed = !skipped && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start) == 0;
    void *result = destination;
    if (!skipped) {
        result = function(destination, value, size);
        if (timed && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end) != 0) {
            timed = 0;
        }
    }
    uint64_t duration = timed ? elapsed_ns(&start, &end) : 0;
    struct tally *total = is_zero ? &zero_total : &nonzero_total;
    struct tally *bucket = is_zero ? &zero_other : &nonzero_other;
    add_tally(total, size, duration, skipped);
    if (size == SIZE_6220816) {
        bucket = is_zero ? &zero_6220816 : &nonzero_6220816;
    } else if (size == SIZE_6220800) {
        bucket = is_zero ? &zero_6220800 : &nonzero_6220800;
    }
    add_tally(bucket, size, duration, skipped);
    errno = saved_errno;
    return result;
}

static size_t append_text(char *buffer, size_t position, const char *text) {
    while (*text != '\0') {
        buffer[position++] = *text++;
    }
    return position;
}

static size_t append_number(char *buffer, size_t position, uint64_t value) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        buffer[position++] = digits[--count];
    }
    return position;
}

static size_t append_tally(char *buffer, size_t position, const char *name,
                           const struct tally *tally) {
    position = append_text(buffer, position, name);
    position = append_text(buffer, position, "_calls=");
    position = append_number(buffer, position, __atomic_load_n(&tally->calls, __ATOMIC_RELAXED));
    position = append_text(buffer, position, "_bytes=");
    position = append_number(buffer, position, __atomic_load_n(&tally->bytes, __ATOMIC_RELAXED));
    position = append_text(buffer, position, "_cpu_ns=");
    position = append_number(buffer, position, __atomic_load_n(&tally->cpu_ns, __ATOMIC_RELAXED));
    position = append_text(buffer, position, "_skipped=");
    return append_number(buffer, position, __atomic_load_n(&tally->skipped, __ATOMIC_RELAXED));
}

__attribute__((destructor)) static void report_probe(void) {
    char line[2048];
    size_t position = 0;
    position = append_text(line, position, "MEMSET_PROBE pid=");
    position = append_number(line, position, (uint64_t)getpid());
    position = append_text(line, position, " threshold=1000000");
    position = append_tally(line, position, "zero", &zero_total);
    position = append_tally(line, position, " nonzero", &nonzero_total);
    position = append_tally(line, position, " zero_6220816", &zero_6220816);
    position = append_tally(line, position, " nonzero_6220816", &nonzero_6220816);
    position = append_tally(line, position, " zero_6220800", &zero_6220800);
    position = append_tally(line, position, " nonzero_6220800", &nonzero_6220800);
    position = append_tally(line, position, " zero_other", &zero_other);
    position = append_tally(line, position, " nonzero_other", &nonzero_other);
    line[position++] = '\n';
    (void)syscall(SYS_write, STDERR_FILENO, line, position);
}
