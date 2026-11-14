// Touch controls (touch_android.cpp): translucent virtual pad over the picture.
#pragma once
#include "common.h"
#include <android/input.h>

namespace touch {
void layout(int bw, int bh, int win_w, int win_h);  // present thread: window buffer and window size
void draw(u32* px, int stride);                     // present thread: overlay into the locked buffer
bool on_motion(const AInputEvent* e);               // input thread: touch events (true = handled)
void controller_used();                             // a game controller was used: hide the overlay
u32 buttons();
u64 version();                                      // changes whenever the overlay looks different
bool visible();                                      // pressed buttons, libpad bits
bool stick(int* lx, int* ly);                       // left stick while a finger is on it (-32767..32767)
bool take_menu_tap();                               // the menu button was pressed (once per press)
void release_all();                                 // forget the fingers (the menu took over the screen)
}  // namespace touch
