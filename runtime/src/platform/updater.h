#pragma once
#include <string>

namespace updater {

// At the start (before the disc image is opened): asks GitHub for the newest release and, when it is
// newer than this build, shows the update window. Returns true when the game should quit right away
// (an update was installed and the new version has been started).
bool run_at_start(int argc, char** argv);

// --update-shot=DIR: draws every state of the update window into DIR\updater_*.png (no window, no
// network) and quits.
void render_shots(const std::string& dir);

}  // namespace updater
