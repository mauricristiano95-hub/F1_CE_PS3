// The wheel as the launcher sees it: SDL joystick and haptic calls on a thread of their own, since DirectInput takes
// its time (2026-11-12, TX: haptic init 0.6 s, opening the wheel 0.5 s). The interface says what it wants (Want),
// reads a copy of the wheel's state (State) each frame and queues its force-feedback tests. The window's event loop
// keeps the joysticks updated (SDL_PumpEvents), and SDL must be initialised (joystick subsystem) before start().
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../runtime/wheel_config.h"

namespace wheeldev {
constexpr int AXES = 12;
struct Device { std::string guid, label, name; bool wheel; };   // (label: name, or name and "not a wheel")
struct State {
    bool ready = false, sdl = false;      // the thread runs / SDL's joystick subsystem works
    std::vector<Device> devices;
    unsigned devices_gen = 0;
    bool open = false, haptic = false;    // the wheel / its force feedback
    std::string guid, name;
    unsigned vid = 0, pid = 0, open_gen = 0;
    int axes = 0, buttons = 0, hats = 0;
    int axis[AXES] = {};
    unsigned pressed[2] = {};
    unsigned char hat = 0;
    std::string status;                   // the wheel found, or what went wrong
    unsigned status_gen = 0;
    // the separate pedal set (Want::pedals)
    bool ped_open = false;
    std::string ped_guid, ped_name;
    unsigned ped_open_gen = 0;
    int ped_axes = 0;
    int ped_axis[AXES] = {};
};
// guid empty: the first wheel; pedals_guid empty: the first pedal set (wheelcfg::pick, as the game picks them)
struct Want { bool open = true, fast = false, haptic = true, pedals = false; std::string guid, pedals_guid; };

void start(const std::string& guid);
void want(const std::function<void(Want&)>& change);
State state();
// closes the wheel and waits (up to 3 s) until it is: the game owns its force feedback while it runs
void close_and_wait();
void test(const wheelcfg::Config& cfg, int t);   // t: 0 push left, 1 push right, 2 spring, 3 bumpy road, 4 stop
void set_gain(int gain);
}  // namespace wheeldev
