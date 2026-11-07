// Debug: Quick Race settings from the environment, so tests reach a given race without driving the menus (the
// launcher's Advanced tab sets them too).
//   F1CE_QR_TRACK=<track>      folder name (silverstone, hungary, monaco, ...) or calendar index 0-17
//   F1CE_QR_DRIVER=<driver>    driver id (nheidfeld), surname (heidfeld) or code (HEI); the team follows the driver
//   F1CE_QR_DISTANCE=<percent> race distance: 10, 20, 30, 50, 75 or 100
//   F1CE_QR_WEATHER=<w>        variable, sunny, cloudy, lightrain or heavyrain
//   F1CE_QR_AI=<level>         opponent AI: easy, medium or hard
//   F1CE_QR_FUEL=<mode>        fuel use and tyre wear: off, scaled or realistic
//   F1CE_QR_DAMAGE, F1CE_QR_FAILURES, F1CE_QR_PENALTIES = 0/1   crash damage, component failure, penalties
// The game keeps the Quick Race settings in an object whose address is stored at 0x10138748 (found by comparing
// memory snapshots while changing the menu): +0x14 track index (2006 calendar order), +0x6C race distance (float
// fraction), +0x70 the Difficulty screen as bits (below), +0x6A80 / +0x6A98 driver and team id strings ({length,
// data, 0, begin, end, capacity end}, 16-byte buffers). The values are written on every pad read until the first 3D
// scene after them (the race), so the menu may show other choices than the race uses. A setting the player changes in
// the menu is no longer overridden (the menu wins): a change counts as the player's when a d-pad direction or the left
// stick moved in the 2 s before it (the game also writes these values itself, e.g. the profile's last race).
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "kernel.h"
#include "rsx.h"

namespace {
const char* const kTracks[] = {"bahrain", "malaysia", "melbourne", "sanmarino", "nurburgring", "spain",
                               "monaco", "silverstone", "canada", "indy", "france", "hockenheim",
                               "hungary", "istanbul", "monza", "shanghai", "suzuka", "brazil"};
struct Driver { const char* id; const char* surname; const char* code; const char* team; };
const Driver kDrivers[] = {
    {"falonso", "alonso", "ALO", "Renault"},          {"gfisichella", "fisichella", "FIS", "Renault"},
    {"kraikkonen", "raikkonen", "RAI", "McLaren"},    {"jmontoya", "montoya", "MOY", "McLaren"},
    {"mschumacher", "schumacher", "MSC", "Ferrari"},  {"fmassa", "massa", "MAS", "Ferrari"},
    {"rschumacher", "rschumacher", "RSC", "Toyota"},  {"jtrulli", "trulli", "TRU", "Toyota"},
    {"mwebber", "webber", "WEB", "Williams"},         {"nrosberg", "rosberg", "ROS", "Williams"},
    {"rbarrichello", "barrichello", "BAR", "Honda"},  {"jbutton", "button", "BUT", "Honda"},
    {"dcoulthard", "coulthard", "COU", "RedBull"},    {"cklien", "klien", "KLI", "RedBull"},
    {"nheidfeld", "heidfeld", "HEI", "BMW"},          {"jvilleneuve", "villeneuve", "VIL", "BMW"},
    {"tmonteiro", "monteiro", "MON", "Midland"},      {"calbers", "albers", "ALB", "Midland"},
    {"vliuzzi", "liuzzi", "LUZ", "ToroRosso"},        {"sspeed", "speed", "SPE", "ToroRosso"},
    {"tsato", "sato", "SAT", "SuperAguri"},           {"yide", "ide", "IDE", "SuperAguri"},
};
constexpr u32 kSettingsPtr = 0x10138748;

// +0x70 bits (Easy preset 0x11E00001): weather in bits 0-7 (0 variable, 1 sunny, 2 cloudy, 3 light rain, 4 heavy rain,
// the game's forecast/sunny/overcast/lightrain/heavyrain), opponent AI in bits 8-15 (0-2), penalties 0x02000000,
// component failure 0x04000000, crash damage 0x08000000, fuel use and tyre wear in bits 28-31 (0 off, 1 scaled,
// 2 realistic). The Overall Difficulty row is derived from these (Custom when they match no preset).
struct Field { const char* env; u32 mask; int shift; const char* const* names; int count; };
const char* const kWeather[] = {"variable", "sunny", "cloudy", "lightrain", "heavyrain"};
const char* const kLevels[] = {"easy", "medium", "hard"};
const char* const kFuel[] = {"off", "scaled", "realistic"};
const char* const kSwitch[] = {"0", "1"};
const Field kFields[] = {
    {"F1CE_QR_WEATHER", 0x000000FF, 0, kWeather, 5},   {"F1CE_QR_AI", 0x0000FF00, 8, kLevels, 3},
    {"F1CE_QR_PENALTIES", 0x02000000, 25, kSwitch, 2}, {"F1CE_QR_FAILURES", 0x04000000, 26, kSwitch, 2},
    {"F1CE_QR_DAMAGE", 0x08000000, 27, kSwitch, 2},    {"F1CE_QR_FUEL", 0xF0000000, 28, kFuel, 3},
};

bool same(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

struct Choice {
    int track = -1;
    const Driver* driver = nullptr;
    float distance = 0;
    u32 bits_mask = 0, bits = 0;   // Difficulty fields to replace in +0x70
    bool any = false;
};
Choice parse() {
    Choice c;
    if (const char* e = getenv("F1CE_QR_TRACK")) {
        if (isdigit((unsigned char)*e)) c.track = atoi(e) < 18 ? atoi(e) : -1;
        for (int i = 0; i < 18; i++)
            if (same(e, kTracks[i])) c.track = i;
        if (c.track < 0) LOG_W("F1CE_QR_TRACK: unknown track '%s'", e);
    }
    if (const char* e = getenv("F1CE_QR_DRIVER")) {
        for (const Driver& d : kDrivers)
            if (same(e, d.id) || same(e, d.surname) || same(e, d.code)) c.driver = &d;
        if (!c.driver) LOG_W("F1CE_QR_DRIVER: unknown driver '%s'", e);
    }
    if (const char* e = getenv("F1CE_QR_DISTANCE")) {
        int p = atoi(e);
        if (p == 10 || p == 20 || p == 30 || p == 50 || p == 75 || p == 100) c.distance = p / 100.0f;
        else LOG_W("F1CE_QR_DISTANCE: %s is not 10, 20, 30, 50, 75 or 100", e);
    }
    std::string fields;
    for (const Field& f : kFields) {
        const char* e = getenv(f.env);
        if (!e || !*e) continue;
        int v = -1;
        for (int i = 0; i < f.count; i++)
            if (same(e, f.names[i])) v = i;
        if (v < 0) { LOG_W("%s: unknown value '%s'", f.env, e); continue; }
        c.bits_mask |= f.mask;
        c.bits |= (u32)v << f.shift;
        fields += std::string(" ") + (f.env + 8) + "=" + f.names[v];
    }
    c.any = c.track >= 0 || c.driver || c.distance > 0 || c.bits_mask;
    if (c.any)
        LOG_I("quick race override: track %s, driver %s, distance %.0f%%%s", c.track >= 0 ? kTracks[c.track] : "(menu)",
              c.driver ? c.driver->id : "(menu)", c.distance * 100, fields.c_str());
    return c;
}

// writes an id string into one of the settings' string objects, if it fits its buffer
bool set_string(u32 obj, const char* s) {
    u32 begin = ld32(obj + 12), cap = ld32(obj + 20);
    size_t n = strlen(s);
    if (ld32(obj + 4) != begin || cap < begin || n + 1 > cap - begin) return false;
    if (ld32(obj) == n && !memcmp(gmem(begin), s, n + 1)) return true;
    memcpy(gmem(begin), s, n + 1);
    st32(obj, (u32)n);
    st32(obj + 16, begin + (u32)n + 1);
    return true;
}
std::string get_string(u32 obj) {
    u32 n = ld32(obj), begin = ld32(obj + 12);
    if (n > 64 || begin < 0x10000) return {};
    return std::string((const char*)gmem(begin), n);
}

u64 g_last_steer = 0;   // time_usec() of the last d-pad direction / stick movement

// One overridden setting: the override ends when the value changes to something else right after the player moved in
// the menu; other changes are the game's own and are overwritten again.
template <class T> struct Kept {
    bool on = true;
    bool keep(const T& cur, const T& want, const char* what) {
        if (!on) return false;
        if (cur != want && g_last_steer && time_usec() - g_last_steer < 2000000) {
            on = false;
            LOG_I("quick race override: %s changed in the game menu, the menu's choice is used", what);
        }
        return on;
    }
};
}  // namespace

void quick_race_overrides(u32 digital1, u32 lx, u32 ly) {
    // d-pad left/down/right/up (0x80/0x40/0x20/0x10), left stick away from the centre
    if ((digital1 & 0xF0) || lx < 0x40 || lx > 0xC0 || ly < 0x40 || ly > 0xC0) g_last_steer = time_usec();
    static const Choice c = parse();
    static const char* poke = getenv("F1CE_QR_POKE");   // experiment: <offset>:<hex value> written like the others
    static bool applied = false, done = false;
    if ((!c.any && !poke) || done) return;
    if (rsx::g_last_frame_draws >= 800) {   // a 3D scene: the race has loaded with the values (or a garage came first)
        if (applied) { done = true; LOG_I("quick race override: 3D scene reached, no longer applied"); }
        return;
    }
    u32 s = ld32(kSettingsPtr);
    if (s < 0x30000000 || s >= 0x40000000 || ld32(s + 0x14) >= 18) return;   // not created yet
    static Kept<u32> track, distance, fields[std::size(kFields)];
    static Kept<std::string> driver;
    if (c.track >= 0 && track.keep(ld32(s + 0x14), (u32)c.track, "circuit")) st32(s + 0x14, (u32)c.track);
    if (c.distance > 0) {
        u32 f;
        memcpy(&f, &c.distance, 4);
        if (distance.keep(ld32(s + 0x6C), f, "race distance")) st32(s + 0x6C, f);
    }
    if (c.bits_mask) {
        u32 v = ld32(s + 0x70);
        for (size_t i = 0; i < std::size(kFields); i++) {
            const Field& f = kFields[i];
            if (!(c.bits_mask & f.mask) || !fields[i].keep(v & f.mask, c.bits & f.mask, f.env + 8)) continue;
            v = (v & ~f.mask) | (c.bits & f.mask);
        }
        st32(s + 0x70, v);
    }
    if (c.driver && driver.keep(get_string(s + 0x6A80), c.driver->id, "driver") &&
        !(set_string(s + 0x6A80, c.driver->id) && set_string(s + 0x6A98, c.driver->team))) {
        static bool warned = false;
        if (!warned) LOG_W("quick race override: driver strings not where expected");
        warned = true;
    }
    if (poke && strchr(poke, ':')) st32(s + (u32)strtoul(poke, nullptr, 16), (u32)strtoul(strchr(poke, ':') + 1, nullptr, 16));
    applied = true;
}
