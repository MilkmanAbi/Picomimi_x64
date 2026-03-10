/**
 * Picomimi-x64 String Functions Header
 */
#ifndef _LIB_STRING_H
#define _LIB_STRING_H

#include <kernel/types.h>

#define NULL ((void *)0)

void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

#endif // _LIB_STRING_H
