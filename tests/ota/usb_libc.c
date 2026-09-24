#include <stddef.h>
int _fltused;
void *memcpy(void *out,const void *in,size_t n)
{ unsigned char *a=out;const unsigned char *b=in;while(n--)*a++=*b++;return out; }
void *memmove(void *out,const void *in,size_t n)
{ unsigned char *a=out;const unsigned char *b=in;if(a<b){while(n--)*a++=*b++;}else{while(n){--n;a[n]=b[n];}}return out; }
void *memset(void *out,int c,size_t n)
{ unsigned char *a=out;while(n--)*a++=(unsigned char)c;return out; }
int memcmp(const void *aa,const void *bb,size_t n)
{ const unsigned char *a=aa,*b=bb;while(n--){if(*a!=*b)return *a-*b;++a;++b;}return 0; }
void *memchr(const void *p,int c,size_t n)
{ const unsigned char *s=p;while(n--){if(*s==(unsigned char)c)return (void *)s;++s;}return NULL; }
size_t strlen(const char *s) { size_t n=0;while(s[n])++n;return n; }
int strcmp(const char *a,const char *b)
{ while(*a && *a==*b){++a;++b;}return (unsigned char)*a-(unsigned char)*b; }
