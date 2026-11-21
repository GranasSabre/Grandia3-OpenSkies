// Battle reward multiplier (settings "EXP" / "Gold" x1..x4).
// battle_003054D0 sets up the result screen and pays the reward out: the battle's gold total
// (0x3ADAB8) is added to the party's gold (with the game's own limit 99,999,999), the EXP total
// (0x3ADBC0) goes to each character with its own factor. Both totals are multiplied just before,
// so the result screen shows the multiplied amounts too. (Found 2026-11-19: gold at 0x5B7488 written
// by 0x3054D0, from a watch on three savestates 0 -> 2 -> 4 gold.)
#include "../common.h"
#include "../memory.h"
#include "../platform/platform.h"
#include "ee_recomp.h"
#include <algorithm>
#include <cstring>

extern "C" void battle_003054D0(EEContext* c);

namespace {
constexpr u32 GOLD_TOTAL = 0x3ADAB8, EXP_TOTAL = 0x3ADBC0;
constexpr s64 LIMIT = 99999999;

void scale(u32 addr, int k, const char* what) {
    s32 v = (s32)mem::rd32(addr);
    if (k <= 1 || v <= 0) {
        log_write(LOG_KERNEL, "battle reward: %s %d", what, v);
        return;
    }
    s64 n = std::min<s64>((s64)v * k, LIMIT);
    mem::wr32(addr, (u32)n);
    log_write(LOG_KERNEL, "battle reward: %s %d x%d = %lld", what, v, k, (long long)n);
}
}  // namespace

// Field camera controller (field 0x29ADB0, a0 = camera object): +208/+212 turn speed, which follows
// the right stick times +144 (0.5) up to +148 (2.4) per frame. While the touch camera swipe asks for
// more than a full stick, both are scaled for this call and put back afterwards (Android).
extern "C" void field_0029ADB0(EEContext* c);
extern "C" void hook_field_camera(EEContext* c) {
    float k = platform::camera_boost();
    u32 o = c->r[4].uw[0];
    if (k <= 1.0f || o < 0x100000 || o > 0x2000000 - 512) {
        field_0029ADB0(c);
        return;
    }
    u32 a = mem::rd32(o + 144), b = mem::rd32(o + 148);
    float fa, fb;
    memcpy(&fa, &a, 4);
    memcpy(&fb, &b, 4);
    float na = fa * k, nb = fb * k;
    u32 ua, ub;
    memcpy(&ua, &na, 4);
    memcpy(&ub, &nb, 4);
    mem::wr32(o + 144, ua);
    mem::wr32(o + 148, ub);
    field_0029ADB0(c);
    mem::wr32(o + 144, a);
    mem::wr32(o + 148, b);
}

extern "C" void hook_battle_reward(EEContext* c) {
    const platform::Config& cfg = platform::config();
    scale(GOLD_TOTAL, cfg.gold_mult, "gold");
    scale(EXP_TOTAL, cfg.exp_mult, "exp");
    battle_003054D0(c);
}
