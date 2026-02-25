#include "bleed_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <syscalls/femtoseconds.h>
#include "time.h"

struct rtc_time_compat {
    unsigned char sec;
    unsigned char min;
    unsigned char hour;
    unsigned char day;
    unsigned char mon;
    unsigned short year;
};

extern int _time(struct rtc_time_compat *buf);

static unsigned int g_rand_state = 1U;

void srand(unsigned int seed) {
    g_rand_state = seed ? seed : 1U;
}

int rand(void) {
    g_rand_state = (1103515245U * g_rand_state) + 12345U;
    return (int)((g_rand_state >> 16) & 0x7fffU);
}

// stub
char *getenv(const char *name) {
    (void)name;
    return 0;
}

//stub
int putenv(char *string) {
    (void)string;
    return -1;
}

// ill add a nicer one to libc later this is quite wide
long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;
    int neg = 0;
    int any = 0;
    unsigned long cutoff;
    unsigned long max_abs;
    int cutlim;

    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        ++s;

    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        ++s;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    if (base < 2 || base > 36) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0;
    }

    max_abs = neg ? ((unsigned long)LONG_MAX + 1UL) : (unsigned long)LONG_MAX;
    cutoff = max_abs;
    cutlim = (int)(cutoff % (unsigned long)base);
    cutoff /= (unsigned long)base;

    for (;; ++s) {
        int c = (unsigned char)*s;
        int digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'z')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            digit = c - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;

        if (any < 0)
            continue;

        if (acc > cutoff || (acc == cutoff && digit > cutlim)) {
            any = -1;
            acc = max_abs;
        } else {
            any = 1;
            acc = acc * (unsigned long)base + (unsigned long)digit;
        }
    }

    if (endptr)
        *endptr = (char *)(any ? s : nptr);

    if (any < 0)
        return neg ? LONG_MIN : LONG_MAX;

    return neg ? -(long)acc : (long)acc;
}

static void byte_swap(unsigned char *a, unsigned char *b, size_t size) {
    while (size--)
    {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    size_t i;
    size_t j;
    unsigned char *arr = (unsigned char *)base;

    if (!base || !compar || size == 0 || nmemb < 2)
        return;

    for (i = 1; i < nmemb; ++i)
    {
        for (j = i; j > 0; --j)
        {
            unsigned char *a = arr + (j - 1) * size;
            unsigned char *b = arr + j * size;
            if (compar(a, b) <= 0)
                break;
            byte_swap(a, b, size);
        }
    }
}

void _Exit(int status) {
    exit(status);
}

// move to libc soon
int fputs(const char *s, FILE *stream) {
    size_t len;

    if (!s || !stream)
        return -1;

    len = strlen(s);
    return (fwrite(s, 1, len, stream) == len) ? 0 : -1;
}

// move to libc soon
int putc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    if (!stream)
        return -1;
    return (fwrite(&ch, 1, 1, stream) == 1) ? c : -1;
}

static double dabs(double x) {
    return (x < 0.0) ? -x : x;
}

double floor(double x) {
    long i = (long)x;
    if ((double)i > x)
        --i;
    return (double)i;
}

double ceil(double x) {
    long i = (long)x;
    if ((double)i < x)
        ++i;
    return (double)i;
}

double fmod(double x, double y) {
    double q;

    if (y == 0.0)
        return 0.0;

    q = floor(x / y);
    return x - q * y;
}

static double atan_approx(double x) {
    double ax = dabs(x);
    double r;

    if (ax > 1.0)
    {
        r = 1.5707963267948966 - ((0.2447 + 0.0663 * (1.0 / ax)) * (1.0 / ax));
    }
    else
    {
        r = ax * (0.7853981633974483 + 0.273 * (1.0 - ax));
    }

    return x < 0.0 ? -r : r;
}

double atan(double x) {
    return atan_approx(x);
}

double atan2(double y, double x) {
    if (x > 0.0)
        return atan_approx(y / x);
    if (x < 0.0 && y >= 0.0)
        return atan_approx(y / x) + 3.1415926535897932;
    if (x < 0.0 && y < 0.0)
        return atan_approx(y / x) - 3.1415926535897932;
    if (x == 0.0 && y > 0.0)
        return 1.5707963267948966;
    if (x == 0.0 && y < 0.0)
        return -1.5707963267948966;
    return 0.0;
}

double acos(double x) {
    double negate;
    double ret;

    if (x > 1.0)
        x = 1.0;
    if (x < -1.0)
        x = -1.0;

    negate = (double)(x < 0.0);
    x = dabs(x);

    ret = -0.0187293;
    ret = ret * x;
    ret = ret + 0.0742610;
    ret = ret * x;
    ret = ret - 0.2121144;
    ret = ret * x;
    ret = ret + 1.5707288;
    ret = ret * sqrt(1.0 - x);
    ret = ret - 2.0 * negate * ret;

    return negate * 3.1415926535897932 + ret;
}

double pow(double x, double y) {
    int i;
    int n;
    double result;
    double frac;
    double root;

    if (x <= 0.0)
        return 0.0;
    if (y == 0.0)
        return 1.0;

    n = (int)floor(y);
    frac = y - (double)n;

    result = 1.0;

    if (n >= 0)
    {
        for (i = 0; i < n; ++i)
            result *= x;
    }
    else
    {
        for (i = 0; i < -n; ++i)
            result /= x;
    }

    if (frac > 0.0)
    {
        unsigned int bits = (unsigned int)(frac * 65536.0);
        unsigned int mask = 1U << 15;
        root = x;

        for (i = 0; i < 16; ++i)
        {
            root = sqrt(root);
            if (bits & mask)
                result *= root;
            mask >>= 1;
        }
    }

    return result;
}

time_t time(time_t *tloc) {
    uint64_t fs = _femtoseconds();
    time_t sec = (time_t)(fs / femtosecondsPerSecond);
    if (tloc)
        *tloc = sec;
    return sec;
}

struct tm *localtime(const time_t *timer)
{
    static struct tm out;
    struct rtc_time_compat rtc;

    (void)timer;

    if (_time(&rtc) < 0)
    {
        memset(&out, 0, sizeof(out));
        return &out;
    }

    out.tm_sec = rtc.sec;
    out.tm_min = rtc.min;
    out.tm_hour = rtc.hour;
    out.tm_mday = rtc.day;
    out.tm_mon = (int)rtc.mon - 1;
    out.tm_year = (int)rtc.year - 1900;
    out.tm_wday = 0;
    out.tm_yday = 0;
    out.tm_isdst = 0;

    return &out;
}
