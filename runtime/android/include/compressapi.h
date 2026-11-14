// Android build: declarations only. LoadLibraryW finds no cabinet.dll, so savestate.cpp writes
// uncompressed states and never calls these.
#pragma once
#include <windows.h>

typedef void* COMPRESSOR_HANDLE;
typedef void* DECOMPRESSOR_HANDLE;
typedef void* PCOMPRESS_ALLOCATION_ROUTINES;
#define COMPRESS_ALGORITHM_XPRESS_HUFF 4

BOOL CreateCompressor(DWORD alg, PCOMPRESS_ALLOCATION_ROUTINES alloc, COMPRESSOR_HANDLE* h);
BOOL Compress(COMPRESSOR_HANDLE h, const void* in, SIZE_T in_size, void* out, SIZE_T out_size, SIZE_T* got);
BOOL CloseCompressor(COMPRESSOR_HANDLE h);
BOOL CreateDecompressor(DWORD alg, PCOMPRESS_ALLOCATION_ROUTINES alloc, DECOMPRESSOR_HANDLE* h);
BOOL Decompress(DECOMPRESSOR_HANDLE h, const void* in, SIZE_T in_size, void* out, SIZE_T out_size, SIZE_T* got);
BOOL CloseDecompressor(DECOMPRESSOR_HANDLE h);
