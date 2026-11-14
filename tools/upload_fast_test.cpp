// Bit-exactness of the block-run upload loops (gs.cpp exec_upload_rows_m) against per-pixel addressing.
// Build: clang++ -O2 -std=c++20 -Iruntime/src tools/upload_fast_test.cpp runtime/src/gs/gs_mem.cpp
#include "gs/gs_mem.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace gs;
void log_write(int, const char*, ...) {}  // (gs_mem.cpp mem_init, unused here)


template <int BPP>
static void ref(u32* mem, u32 psm, int x0, int y, u32 bp, u32 bw, const u8* p, u32 cnt) {
    for (u32 i = 0; i < cnt; i++) {
        int x = (x0 + i) & 2047;
        if (BPP == 32) memcpy(&mem[addr32(x, y, bp, bw)], p + i * 4, 4);
        if (BPP == 16) memcpy(&((u16*)mem)[addr16(x, y, bp, bw)], p + i * 2, 2);
        if (BPP == 8) ((u8*)mem)[addr8(x, y, bp, bw)] = p[i];
    }
}
template <int BPP>
static void fast(u32* mem, u32 psm, int x0, int y, u32 bp, u32 bw, const u8* p, u32 cnt) {
    if (BPP == 32) {
        const u8* ct = columnTable32[y & 7];
        for (u32 i = 0; i < cnt;) {
            int x = (x0 + i) & 2047;
            u32 run = std::min<u32>(cnt - i, 8 - (x & 7));
            u32* blk = mem + (addr32(x & ~7, y, bp, bw) - ct[0]);
            for (u32 j = 0; j < run; j++) memcpy(&blk[ct[(x & 7) + j]], p + (i + j) * 4, 4);
            i += run;
        }
    } else if (BPP == 16) {
        const u8* ct = columnTable16[y & 7];
        for (u32 i = 0; i < cnt;) {
            int x = (x0 + i) & 2047;
            u32 run = std::min<u32>(cnt - i, 16 - (x & 15));
            u16* blk = (u16*)mem + (addr16(x & ~15, y, bp, bw) - ct[0]);
            for (u32 j = 0; j < run; j++) memcpy(&blk[ct[(x & 15) + j]], p + (i + j) * 2, 2);
            i += run;
        }
    } else {
        const u8* ct = columnTable8[y & 15];
        for (u32 i = 0; i < cnt;) {
            int x = (x0 + i) & 2047;
            u32 run = std::min<u32>(cnt - i, 16 - (x & 15));
            u8* blk = (u8*)mem + (addr8(x & ~15, y, bp, bw) - ct[0]);
            for (u32 j = 0; j < run; j++) blk[ct[(x & 15) + j]] = p[i + j];
            i += run;
        }
    }
}
int main() {
    std::vector<u32> a(1 << 20), b(1 << 20);
    std::vector<u8> src(2048 * 4);
    std::mt19937 rng(1);
    long bad = 0, n = 0;
    for (int it = 0; it < 200000; it++) {
        for (auto& c : src) c = (u8)rng();
        int bpp = (int[]){32, 16, 8}[it % 3];
        u32 bp = rng() & 0x3FFF, bw = 1 + rng() % 32;
        int x0 = rng() & 2047, y = rng() & 2047;
        u32 cnt = 1 + rng() % 600;
        if (bpp == 32) { ref<32>(a.data(), 0, x0, y, bp, bw, src.data(), cnt); fast<32>(b.data(), 0, x0, y, bp, bw, src.data(), cnt); }
        if (bpp == 16) { ref<16>(a.data(), 0, x0, y, bp, bw, src.data(), cnt); fast<16>(b.data(), 0, x0, y, bp, bw, src.data(), cnt); }
        if (bpp == 8) { ref<8>(a.data(), 0, x0, y, bp, bw, src.data(), cnt); fast<8>(b.data(), 0, x0, y, bp, bw, src.data(), cnt); }
        n++;
        if (it % 1000 == 999 && memcmp(a.data(), b.data(), a.size() * 4)) { bad++; b = a; }
    }
    bool same = memcmp(a.data(), b.data(), a.size() * 4) == 0;
    printf("%ld uploads, %ld mismatching windows, final %s\n", n, bad, same ? "identical" : "DIFFERENT");
    return bad || !same;
}
