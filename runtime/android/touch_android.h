// Touch controls (touch_android.cpp): translucent virtual pad over the picture.
#pragma once
#include "common.h"
#include <android/input.h>
#include <string>

bool g3_gamepad_name(std::string& name);  // jni_android.cpp: a connected controller ("" none); false: unknown

namespace touch {
void layout(int bw, int bh, int win_w, int win_h);  // present thread: window buffer and window size
void draw(u32* px, int stride);                     // present thread: overlay into the locked buffer
bool on_motion(const AInputEvent* e);               // input thread: touch events (true = handled)
void controller_used();                             // a game controller was used: hide the overlay
u32 buttons();                                      // pressed buttons, libpad bits
u64 version();                                      // changes whenever the overlay looks different (also fades it)
bool visible();                                     // drawn at all (not faded out or hidden for a controller)
bool pad_active();                                  // a controller is connected or was used last
void set_autohide(bool on);                         // fade out after a few seconds without a touch
void set_camera_speed(int s);                       // camera swipe speed 1..15
void set_dpad(bool on);                             // show the d-pad
bool stick(int* lx, int* ly);                       // left stick while a finger is on it (-32767..32767)
bool camera(int* rx);                               // right stick x from a camera swipe (false: none)
float camera_boost();                               // turn speed factor 1..3 for fast swipes
bool take_menu_tap();                               // the menu button was pressed (once per press)
void release_all();                                 // forget the fingers (the menu took over the screen)
}  // namespace touch
