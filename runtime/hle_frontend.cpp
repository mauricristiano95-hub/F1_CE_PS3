// Front-end additions written in Lua. The menus are Lua scripts (offline.wad script packs: u32 name hash, u32 size,
// chunk), and the game links the whole Lua 5.0/5.1-work library, parser included, so new behaviour can be added as Lua
// source run in the game's own state instead of patched bytecode:
//  - 0x7521C loads a script by name from the current pack (lua_load 0x58935C with its own reader) and, when asked,
//    runs it (lua_pcall 0x58C1DC at 0x75424). The probe after that call runs kPatch (luaL_loadbuffer 0x5A3CF8 +
//    lua_pcall) with F1CE_CHUNK set to the script's name, so the patch can extend the classes a script defines.
//    Screen scripts are loaded without running (the engine runs them in their own environments); include classes
//    run here. kPatch adds "QUIT GAME" to the navigation bar (see its comments).
//  - os.exit (0x5A9120, never called by the game's scripts) requests the same shutdown as closing the window.
//  - print (luaB_print 0x5A566C) calls a handler the retail game never sets; F1CE_LUA_PRINT=1 logs its arguments.
//  - Lua errors unwind with setjmp/longjmp, which translated code cannot do (see lua_rawrunprotected_hook).
// Things that cost time: the parser allows at most 16 operators in one expression chain ("chunk has too many syntax
// levels"); running Lua can reallocate the thread's stack, so host code keeps stack positions as indices
// (lua_gettop/lua_settop), never as L->top pointers (a stale one corrupted the engine's next push much later).
// F1CE_LUA_PATCH=<file> replaces kPatch (development); F1CE_LUA_LOG=1 logs every script load.
// Hooks and probes in tools/recomp.py.
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "host.h"
#include "kernel.h"
#include "rsx.h"

namespace {
constexpr u32 kToc = 0x70F138;
constexpr u32 kLuaLoadBuffer = 0x5A3CF8, kLuaPcall = 0x58C1DC, kLuaToString = 0x58AB34, kLuaGetTop = 0x588D08,
              kLuaSetTop = 0x588D20;

const char kPatch[] = R"LUA(
-- "QUIT GAME" in the bottom bar of the 2D front-end menus, between BACK and SELECT; reached with the pad by moving
-- past the last (or before the first) item of a menu, chosen with SELECT: os.exit, which the runtime turns into the
-- same shutdown as closing the window. Not over a 3D scene (garage, race pause menus): os.getenv("F1CE_SCENE3D").
-- This runs after every script the loader runs, each time with that script's globals table: xmove makes a real
-- global, as NavigationBars.lua does. The game's own tables get no new fields (only wrapped methods); the button's
-- state is F1CEQuit and the objects it keeps are in S.
-- The engine calls the pad handlers unprotected (a Lua error there ends the game), so the button's own logic runs
-- in pcall and falls back to the game's behaviour.
if F1CEQuit == nil then
  local state = {wrapped = setmetatable({}, {__mode = "k"})}
  xmove("F1CEQuit", {focused = false, shown = -1, menuPatched = "", configPatched = "", navPatched = "",
                     state = function() return state end})
end
local Q = F1CEQuit
local S = Q.state()

local function now()
  return os.time()
end

local function visible()
  return Q.shown >= 0 and now() - Q.shown < 0.3
end

local function label()
  local lang = game.language()
  local t = {[1] = "SPIEL BEENDEN", [2] = "QUITTER LE JEU", [3] = "ESCI DAL GIOCO", [4] = "SALIR DEL JUEGO",
             [5] = "LOPETA PELI", [10] = "SAIR DO JOGO"}
  return t[lang] or "QUIT GAME"
end

local function selectable(menu, i)
  local item = menu.mMenuObjects[i]
  return item ~= nil and item.mState ~= nil and item.mState < 2
end

-- true when no selectable item lies beyond the current one in direction dir
local function atEnd(menu, dir)
  local i = menu.mCurrentMenuItem + dir
  while i >= 1 and i <= menu.mMenuItemCount do
    if selectable(menu, i) then
      return false
    end
    i = i + dir
  end
  return true
end

local function setArrows(menu, on)
  local arrows = menu.mSelectionArrow
  if arrows == nil then
    return
  end
  if on then
    if S.arrowsWere ~= nil then
      arrows.mActive = S.arrowsWere
    end
    S.arrowsWere = nil
  else
    S.arrowsWere = arrows.mActive
    arrows.mActive = false
  end
end

local function unfocus()
  if Q.focused and S.menu ~= nil then
    setArrows(S.menu, true)
  end
  Q.focused = false
  S.menu = nil
  S.env = nil
end

local function quit()
  sfx.play(sfx.select)
  print("F1CE: Quit Game selected")
  os.exit()
end

local function report(err)
  print("F1CE: Quit Game error: " .. tostring(err))
end

-- the screen's own onSelect may act on the current item before it calls Menu:userSelect: take SELECT first
-- (found on the call stack: the screen's onMenuUp/onMenuDown called Menu:moveSelection; returns the screen's env)
local function hookScreen(level)
  local ok, env = pcall(getfenv, level)
  if not ok or type(env) ~= "table" or env == _G then
    return nil
  end
  local onSelect = rawget(env, "onSelect")
  if type(onSelect) ~= "function" then
    return nil
  end
  if S.wrapped[env] ~= onSelect then
    local wrapped = function(state)
      if state == 1 and Q.focused and S.env == env and visible() then
        quit()
        return
      end
      return onSelect(state)
    end
    rawset(env, "onSelect", wrapped)
    S.wrapped[env] = wrapped
  end
  return env
end

local function patchReactivate(cls)
  local reactivate = rawget(cls, "ReActivateMenu")
  if reactivate == nil then
    return
  end
  rawset(cls, "ReActivateMenu", function(self)
    unfocus()
    return reactivate(self)
  end)
end

-- Up/Down in a menu: true when the move was the button's (focus taken), false to let the menu move
local function quitMove(self, dir)
  if Q.focused and (S.menu ~= self or not visible()) then
    unfocus()
  end
  if dir == 0 or self.mMenuObjects == nil or self.mOutcomeSent == true then
    return false
  end
  if Q.focused then
    -- leave the button: down wraps to the first item, up to the last
    unfocus()
    local cur = self.mMenuObjects[self.mCurrentMenuItem]
    if cur ~= nil and cur.mState == 1 then
      cur:setState(0)
    end
    if dir > 0 then
      self.mCurrentMenuItem = self.mMenuItemCount
    else
      self.mCurrentMenuItem = 1
    end
    return false
  end
  if not visible() or not atEnd(self, dir) then
    return false
  end
  local cur = self.mMenuObjects[self.mCurrentMenuItem]
  if cur ~= nil and cur.mState == 1 then
    cur:setState(0)
  end
  -- as a real move does (screens animate from the previous item to the current one: MainMenu's picture)
  self.mPreviousMenuItem = self.mCurrentMenuItem
  Q.focused = true
  S.menu = self
  setArrows(self, false)
  sfx.play(sfx.move)
  -- stack from getfenv: pcall, hookScreen, quitMove, pcall, the wrapper, the class's moveSelection, the screen
  local level = 3
  while S.env == nil and level <= 9 do
    S.env = hookScreen(level)
    level = level + 1
  end
  return true
end

local function patchMenu(cls)
  local moveSelection = cls.moveSelection
  cls.moveSelection = function(self, dir)
    local ok, handled = pcall(quitMove, self, dir)
    if not ok then
      report(handled)
    elseif handled then
      return
    end
    return moveSelection(self, dir)
  end
  local userSelect = cls.userSelect
  if userSelect ~= nil then
    cls.userSelect = function(self)
      if Q.focused and S.menu == self and visible() then
        quit()
        return
      end
      return userSelect(self)
    end
  end
  -- config menus: left/right would change the row that was highlighted before the button took the focus
  local changeValue = rawget(cls, "changeValueOfSelection")
  if changeValue ~= nil then
    rawset(cls, "changeValueOfSelection", function(self, a, b)
      if Q.focused and S.menu == self then
        return
      end
      return changeValue(self, a, b)
    end)
  end
  -- a menu rebuilt for another screen starts without the button focused (ClassTextMenu is shared by the screens)
  patchReactivate(cls)
end

local function drawArrow(sprite, x, y, w, h, mirrored)
  local d = gDimensionDefs
  sprite:rectangle(x, y, w, h)
  if mirrored then
    sprite:sts(d.SelectionArrowWST, d.MinST, -d.SelectionArrowWST, d.SelectionArrowHST)
  else
    sprite:sts(d.MinST, d.MinST, d.SelectionArrowWST, d.SelectionArrowHST)
  end
  sprite:draw()
end

-- made on the first draw, in the navigation bar's own callback (the game makes its pens and sprites in its scripts)
local function makeResources()
  S.pen = pen(gFonts.Small)
  S.pen:scalex(1)
  S.pen:alignv(pen.alignbottom)
  S.pen:alignh(pen.aligncentre)
  S.pen:subpixels(true)
  S.pos = point(0, 0)
  S.text = label()
  S.arrowTexture = texture("Frontend/Textures/Selection_Arrows_half.pct")
  S.arrowTexture:wrapu(texture.clamp)
  S.arrowTexture:wrapv(texture.clamp)
  S.arrows = {sprite(), sprite(), sprite(), sprite()}
  local red = colour(255, 0, 0, 255)
  S.arrows[1]:texture(S.arrowTexture)
  S.arrows[2]:texture(S.arrowTexture)
  S.arrows[3]:texture(S.arrowTexture)
  S.arrows[4]:texture(S.arrowTexture)
  S.arrows[1]:colour(red)
  S.arrows[2]:colour(red)
  S.arrows[3]:colour(red)
  S.arrows[4]:colour(red)
end

local function drawQuit(nb)
  Q.shown = now()
  if S.pen == nil then
    makeResources()
  end
  local d = gDimensionDefs
  local cx = d.screenWidth / 2
  local y = gLayoutDefs.okButY + d.navigationButtonOffsetY
  S.pos.x = cx
  S.pos.y = y
  S.penShadow.DrawShadow(S.text, nb.mTextColour, nb.mShadowColour, S.pen, S.pos)
  S.pen:position(S.pos)
  S.pen:colour(nb.mTextColour)
  S.pen:draw(S.text)
  if Q.focused then
    local w = d.selectionArrowWidth
    local h = d.selectionArrowHeight
    -- (y grows upwards: a rectangle's y is its bottom edge; centred on the capitals, as the menu's arrows)
    local half = S.pen:width(S.text) / 2
    local bottom = y + S.pen:height(gFonts.LetterAUniCode) / 2 - h / 2
    local step = w * 0.55
    local gap = 4
    drawArrow(S.arrows[1], cx - half - gap - w - step, bottom, w, h, false)
    drawArrow(S.arrows[2], cx - half - gap - w, bottom, w, h, false)
    drawArrow(S.arrows[3], cx + half + gap, bottom, w, h, true)
    drawArrow(S.arrows[4], cx + half + gap + step, bottom, w, h, true)
  end
end

local function patchNavigation(env)
  local nb = rawget(env, "NavigationButtons")
  if nb == nil or Q.navPatched == tostring(nb) then
    return
  end
  Q.navPatched = tostring(nb)
  S.penShadow = rawget(env, "PenShadow")
  local onRender = nb.onRender
  nb.onRender = function(self)
    onRender(self)
    if gNavBarOverride.mShowNavButtons and self.mButtons ~= nil and not S.broken and not os.getenv("F1CE_SCENE3D") then
      local ok, err = pcall(drawQuit, self)
      if not ok then
        S.broken = true
        report(err)
      end
    end
  end
  print("F1CE: Quit Game added to the navigation bar")
end

if gClassTextMenu ~= nil and Q.menuPatched ~= tostring(gClassTextMenu) then
  Q.menuPatched = tostring(gClassTextMenu)
  patchMenu(gClassTextMenu)
  print("F1CE: Quit Game added to the text menus")
end
-- the config menus (options: toggles and sliders) inherit from ClassMenu, not from gClassTextMenu
if gClassConfigMenu ~= nil and Q.configPatched ~= tostring(gClassConfigMenu) then
  Q.configPatched = tostring(gClassConfigMenu)
  patchMenu(gClassConfigMenu)
  print("F1CE: Quit Game added to the config menus")
end
if gClassTextMenu ~= nil then
  patchNavigation(getfenv(gClassTextMenu.updateItemHighlight))
end
)LUA";

const bool g_log = getenv("F1CE_LUA_LOG") != nullptr;
const bool g_print = [] {
    const char* e = getenv("F1CE_LUA_PRINT");
    return e && *e && *e != '0';
}();

std::string patch_source() {
    if (const char* path = getenv("F1CE_LUA_PATCH")) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            LOG_I("frontend: Lua patch from %s", path);
            return ss.str();
        }
        LOG_W("F1CE_LUA_PATCH: cannot read %s", path);
    }
    return kPatch;
}

// Calls guest code at addr (not an OPD) and leaves the caller's registers as they were.
u64 call_code(PPUContext& c, u32 addr, std::initializer_list<u64> args) {
    const PPUContext saved = c;
    int i = 0;
    for (u64 a : args) c.r[3 + i++] = a;
    c.r[2] = kToc;
    call_indirect(c, addr);
    const u64 r = c.r[3];
    c = saved;
    return r;
}

u32 guest_string(const std::string& s) {
    const u32 ea = mem::alloc((u32)s.size() + 1);
    memcpy(gmem(ea), s.data(), s.size());
    st8(ea + (u32)s.size(), 0);
    return ea;
}

std::string chunk_name(PPUContext& c) {
    // 0x7521C keeps its name argument (u32 length, u32 string, also the chunk name given to lua_load) at r1 + 280
    const u32 len = ld32((u32)c.r[1] + 280), name = ld32((u32)c.r[1] + 284);
    return name ? gstr(name, len < 256 ? len : 256) : std::string();
}

// Runs src in state L: compile errors and runtime errors are logged and leave the stack as it was.
void run_lua(PPUContext& c, u32 L, const std::string& src, const char* what) {
    static u32 name_ea = guest_string("=f1ce_frontend");
    static u32 buf = 0, cap = 0;   // the HLE heap has no free: one buffer, grown when needed
    if (src.size() + 1 > cap) {
        cap = (u32)src.size() + 1024;
        buf = mem::alloc(cap);
    }
    memcpy(gmem(buf), src.data(), src.size());
    st8(buf + (u32)src.size(), 0);
    // (by index: running the patch can reallocate the thread's stack, so a saved L->top would point into the old one)
    const u64 top = call_code(c, kLuaGetTop, {L});
    u64 status = call_code(c, kLuaLoadBuffer, {L, buf, src.size(), name_ea});
    if (status == 0) status = call_code(c, kLuaPcall, {L, 0, 0, 0});
    if (status != 0) {
        const u32 msg = (u32)call_code(c, kLuaToString, {L, (u64)(s64)-1});
        LOG_W("frontend: Lua patch failed after %s (%llu): %s", what, (unsigned long long)status,
              msg ? gstr(msg, 1024).c_str() : "?");
    }
    call_code(c, kLuaSetTop, {L, top});
}
}  // namespace

// probe at 0x7534C: lua_load of a script returned (r3 status, r26 run it)
void lua_script_loaded(PPUContext& c) {
    if (g_log) LOG_I("frontend: script %s loaded (status %d, run %d)", chunk_name(c).c_str(), (int)c.r[3], (int)c.r[26]);
}

// probe at 0x75428: a script loaded by 0x7521C has run (r27 = lua_State)
void lua_script_ran(PPUContext& c) {
    static const std::string patch = patch_source();
    if (patch.find_first_not_of(" \r\n") == std::string::npos) return;
    const std::string name = chunk_name(c);
    std::string esc;
    for (char ch : name) {
        if (ch == '\\' || ch == '"') esc += '\\';
        esc += ch;
    }
    run_lua(c, (u32)c.r[27], "F1CE_CHUNK = \"" + esc + "\"\n" + patch, name.c_str());
}

// Lua errors unwind with the guest's setjmp/longjmp (0x668C54/0x668D78), which translated code cannot do: longjmp
// would return to its caller. The game's scripts never raise an error, but a protected call has to work for the
// patch (and makes script errors survivable), so the two ends of Lua's error handling run on host exceptions.
namespace {
struct LuaError { u32 jmp; };
}
// 0x58DE18 luaD_rawrunprotected(L, f, ud): runs f(L, ud) with a struct lua_longjmp (previous +0, jmp_buf +4,
// status +548) linked at L + 80; returns its status
bool lua_rawrunprotected_hook(PPUContext& c) {
    const PPUContext entry = c;
    const u32 L = (u32)c.r[3], f = (u32)c.r[4], sp = (u32)c.r[1] - 0x2A0, lj = sp + 0x70;
    st64(sp, c.r[1]);   // the frame the original allocates (back chain)
    st32(lj, ld32(L + 80));
    st32(lj + 548, 0);
    st32(L + 80, lj);
    c.r[1] = sp;
    c.r[3] = L;
    c.r[4] = entry.r[5];
    c.r[2] = ld32(f + 4);
    try {
        call_indirect(c, ld32(f));
    } catch (const LuaError& e) {
        if (e.jmp != lj) throw;
    }
    const u32 status = ld32(lj + 548);
    c = entry;
    st32(L + 80, ld32(lj));
    c.r[3] = (u64)(s64)(s32)status;
    return true;
}
// 0x58DDA4 luaD_throw(L, errcode)
bool lua_throw_hook(PPUContext& c) {
    const u32 jmp = ld32((u32)c.r[3] + 80);
    if (!jmp) {   // unprotected: the original calls the panic function and exits
        const u32 msg = (u32)call_code(c, kLuaToString, {c.r[3], (u64)(s64)-1});
        LOG_E("frontend: unprotected Lua error, the game exits: %s", msg ? gstr(msg, 1024).c_str() : "?");
        return false;
    }
    st32(jmp + 548, (u32)c.r[4]);
    throw LuaError{jmp};
}

// 0x5A9120 os.exit
bool lua_os_exit_hook(PPUContext& c) {
    LOG_I("frontend: Quit Game");
    host::request_quit();
    c.r[3] = 0;
    return true;
}

// 0x5A8EF0 os.getenv (never called by the game's scripts): os.getenv("F1CE_SCENE3D") is true while the frame shown
// is a 3D scene (garage, race, pause menus: >= 800 draws), where the patch keeps the button away
bool lua_os_getenv_hook(PPUContext& c) {
    const u32 L = (u32)c.r[3];
    const u32 s = (u32)call_code(c, kLuaToString, {L, 1});
    if (!s || gstr(s, 64) != "F1CE_SCENE3D") return false;
    const u32 top = ld32(L + 8);   // a C function always has room for its results
    st32(top, 1);                  // LUA_TBOOLEAN
    st32(top + 4, rsx::g_last_frame_draws >= 800 ? 1 : 0);
    st32(L + 8, top + 8);
    c.r[3] = 1;
    return true;
}

// 0x5A566C print
bool lua_print_hook(PPUContext& c) {
    if (!g_print) return false;
    const u32 L = (u32)c.r[3];
    const int n = (int)(s32)call_code(c, kLuaGetTop, {L});
    std::string line;
    for (int i = 1; i <= n; i++) {
        const u32 s = (u32)call_code(c, kLuaToString, {L, (u64)i});
        if (i > 1) line += '\t';
        line += s ? gstr(s, 2048) : "?";
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    LOG_I("lua: %s", line.c_str());
    c.r[3] = 0;
    return true;
}
