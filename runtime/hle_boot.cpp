// Quick boot: F1CE_SKIP_INTRO=1 starts the game at the "Press Start" screen, without the language menu and the intro
// movie; F1CE_LANGUAGE=<name> picks the language (english, french, german, italian, spanish, portuguese, finnish;
// default: the system language, English). The front end is Lua (offline.wad, Lua 5.0 instructions in a 5.1-style
// container); the boot decisions are made by its scripts through the game object:
//  - Init/Language.lua (the boot Init graph, after NoPads) shows the menu unless game.initBoot().firstBoot == 0; then
//    it applies initBoot().bootLanguage itself (game.language, speed units mph for English else km/h) and takes
//    outcome "skipped". firstBoot is the byte at 0x10040010, cleared by the boot code only when the game is started
//    with a {language} argument (the return from ONLINE.BIN), and read only by game.initBoot (0x1FE0EC). That path
//    must not stay on: GameOrProfileMainSelector.lua then skips the Press Start screen for the profile autoload.
//    So the first initBoot call (Language.lua's onActivate) sees firstBoot 0 and later ones the real value.
//  - bootLanguage (0x100C7620) is the game code of the system language (cellSysutilGetSystemParamInt 0x111), mapped
//    by 0x48F88. Codes from the localisation switch at 0x247878: 0 english, 1 german, 2 french, 3 italian, 4 spanish,
//    5 finnish, 6 japanese, 7 american, 10 portuguese.
//  - GameOrProfileMainSelector.lua plays the intro (outcome IntroMovie) unless game.attract() is true, which
//    Attract.lua sets once the intro has played; then it goes to DoStartScreen (Splash.lua, "PRESS START BUTTON").
//    The flag is the byte at game + 0x70F4 (accessor 0x1F9EBC).
// Hooks in tools/recomp.py HOOKS.
#include <cctype>
#include <cstdlib>

#include "kernel.h"

namespace {
struct Language { const char* name; u32 code; };
const Language kLanguages[] = {{"english", 0}, {"german", 1}, {"french", 2},     {"italian", 3},
                               {"spanish", 4}, {"finnish", 5}, {"portuguese", 10}};

const bool g_skip = [] {
    const char* e = getenv("F1CE_SKIP_INTRO");
    return e && *e && *e != '0';
}();
int language() {
    const char* e = getenv("F1CE_LANGUAGE");
    if (!e || !*e) return -1;
    for (const Language& l : kLanguages) {
        const char *a = e, *b = l.name;
        while (*a && *b && tolower((unsigned char)*a) == *b) a++, b++;
        if (!*a && !*b) return (int)l.code;
    }
    LOG_W("F1CE_LANGUAGE: unknown language '%s' (english, french, german, italian, spanish, portuguese, finnish)", e);
    return -1;
}
}  // namespace

// 0x48F88: system language -> game language code (called once by the boot code)
bool boot_language_hook(PPUContext& c) {
    static const int code = language();
    if (code < 0) return false;
    LOG_I("boot: language code %d (F1CE_LANGUAGE)", code);
    c.r[3] = (u64)code;
    return true;
}
// 0x1FE0EC: game.initBoot() -> {firstBoot, bootLanguage, bootDisplay}
bool boot_init_hook(PPUContext&) {
    constexpr u32 kFirstBoot = 0x10040010;
    static int calls = 0;
    static u8 first_boot = 0;
    if (!g_skip || calls > 1) return false;
    if (calls++ == 0) {
        first_boot = ld8(kFirstBoot);
        st8(kFirstBoot, 0);
        LOG_I("boot: language menu skipped (F1CE_SKIP_INTRO)");
    } else {
        st8(kFirstBoot, first_boot);
    }
    return false;
}
// 0x1F9EBC: game.attract([value]); r3 = game object
bool boot_attract_hook(PPUContext& c) {
    static bool done = false;
    if (g_skip && !done) {
        done = true;
        st8((u32)c.r[3] + 0x70F4, 1);
        LOG_I("boot: intro movie skipped (F1CE_SKIP_INTRO)");
    }
    return false;
}
