#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

// a lot of ts should be in libc ill keep it in mind :p

int rand(void);
void srand(unsigned int seed);
char *getenv(const char *name);
int putenv(char *string);
long strtol(const char *nptr, char **endptr, int base);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

void _Exit(int status);
int fputs(const char *s, FILE *stream);
int putc(int c, FILE *stream);

double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double atan(double x);
double atan2(double y, double x);
double acos(double x);
double pow(double x, double y);
