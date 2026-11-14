// Android build: included before every C++ file of the runtime (-include). The shared code builds
// paths with '\\' (Windows); fopen goes through g3_fopen, which converts them to '/'.
#pragma once
#ifdef __cplusplus
#include <cstdio>
FILE* g3_fopen(const char* path, const char* mode);
#define fopen(p, m) g3_fopen(p, m)
#endif
