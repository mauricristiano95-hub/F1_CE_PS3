// The launcher's settings, without any interface: the options are environment variables of the game (README,
// "Debugging aids"), kept in f1ce_launcher.ini next to the launcher ([options], [developer], [custom preset], [game];
// the same keys as the Win32 launcher), and the game started with them from the launcher's folder.
#pragma once
#include <functional>
#include <string>
#include <vector>

class Ini;

namespace opts {
enum Page { GENERAL, VIDEO, QUICK_RACE, KEYBOARD, GAMEPAD, WHEEL, DEVELOPER, PAGES };
enum Kind { CHOICE, TOGGLE, TEXT, HEADER };   // HEADER: a group title in a page's list

struct Choice { std::string label; const char* value; };   // value nullptr: the variable stays unset
struct Option {
    Page page;
    Kind kind;
    const char* label;
    const char* key;            // the variable ("--name": a command-line switch; "preset": the launcher's own)
    std::vector<Choice> choices;   // CHOICE
    int def = 0;
    const char* help = "";
    const char* on = "1";       // TOGGLE: the variable's value when on
    bool experimental = false;
    // state
    int sel = 0;                // CHOICE: the choice; TOGGLE: 0 / 1
    std::string text;           // TEXT
    std::string value_label() const;
};

std::vector<Option>& all();
Option* find(const char* key);

// at start: the launcher's folder (log, saves and the ini live there), the season packs and GPUs found
void init(const std::string& exe_dir);
const std::string& exe_dir();
std::string ini_path();
void load(const Ini& ini);
// Developer Tools: shown, and their options passed to the game, only with F1CE_DEV_MODE=1 ([game] of the ini, or
// the environment)
bool dev_mode();
void save(Ini& ini);

// presets: picking one sets its options; changing one of them makes it Custom (its values kept in the ini)
void changed(Option& o, Ini& ini);

std::string& game_path();
std::string game_path_problem();   // empty: the folder looks like PS3_GAME with the decrypted EBOOT.elf
std::string gpu_summary();         // "Vulkan · AMD Radeon RX 6600"

// starts the game with the options; returns an error message, or empty when it runs (exited: from another thread)
// (saves the options in the ini first)
std::string start_game(Ini& ini, std::function<void(unsigned code)> exited);
std::string exit_message(unsigned code);   // why the game stopped (empty: normal exit)
}  // namespace opts
