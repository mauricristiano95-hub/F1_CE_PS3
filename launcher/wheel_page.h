// The launcher's Steering Wheel section (wheel_page.cpp): the [wheel] settings of runtime/wheel_config.h as rows,
// with the wheel's live input above them and force-feedback tests; the wheel itself: wheel_device.cpp.
#pragma once
#include <string>

class Ini;

namespace wheel {
void start(const Ini& ini);   // loads [wheel] and starts the wheel's thread (SDL's joystick subsystem must run)
void save(Ini& ini);
void frame(bool visible);     // every frame, before rows(): the wheel's state, live updates while visible
int rows();                   // draws the section's rows (in the rows child); returns how many can take the focus
void release();               // before the game starts: closes the wheel (the game owns its force feedback)
void resume();                // after the game: opens it again
std::string name();           // the open wheel's name, or empty
}  // namespace wheel
