// The launcher's KEYBOARD and GAMEPAD sections (input_page.cpp): the [keyboard] and [gamepad] bindings of
// runtime/input_config.h as rows, a binding set by pressing the key or pad input.
#pragma once

class Ini;

namespace input {
void load(const Ini& ini);
void save(Ini& ini);
// draw the section's rows from row index first; return the index after the last one
int keyboard_rows(int first);
int gamepad_rows(int first);
}  // namespace input
