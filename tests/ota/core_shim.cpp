#include <stddef.h>
extern "C" void *memcpy(void *out, const void *in, size_t n)
{ unsigned char *a = (unsigned char *)out; const unsigned char *b = (const unsigned char *)in; while (n--) *a++ = *b++; return out; }
extern "C" void *memset(void *out, int c, size_t n)
{ unsigned char *a = (unsigned char *)out; while (n--) *a++ = (unsigned char)c; return out; }
extern "C" int memcmp(const void *aa, const void *bb, size_t n)
{ const unsigned char *a = (const unsigned char *)aa, *b = (const unsigned char *)bb; while (n--) { if (*a != *b) return *a - *b; ++a; ++b; } return 0; }
