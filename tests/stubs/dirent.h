// Directory walking, for compilers that have no dirent.h (MSVC). The tests do not walk a
// real directory - they exercise the path checking that decides whether a walk may
// happen at all - so these only need to exist and behave emptily.
#pragma once
struct dirent { char d_name[256]; };
typedef struct DIR DIR;
static inline DIR *opendir(const char *p) { (void)p; return 0; }
static inline struct dirent *readdir(DIR *d) { (void)d; return 0; }
static inline int closedir(DIR *d) { (void)d; return 0; }
