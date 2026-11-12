// The launcher's Steering Wheel tab (wheel_tab.cpp): the [wheel] settings of runtime/wheel_config.h, with the wheel's
// live input and force-feedback tests (SDL, as the game reads it, on a thread of its own).
#pragma once
#include <windows.h>

#include <string>

namespace wheeltab {
// the launcher's control factory (x, y, w, h in 96 dpi units; page: the tab) and tooltips
using AddFn = HWND (*)(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int page);
using TipFn = void (*)(HWND c, const wchar_t* text);
constexpr UINT_PTR TIMER_ID = 0x5717;

int build(HWND main, int page, int x, int y, int w, AddFn add, TipFn tip);   // returns the y below the controls
void load(const std::wstring& ini);
void start();               // after load: starts the SDL thread, which finds and opens the wheel in the background
void save(const std::wstring& ini);
void shown(bool visible);   // the tab became visible / hidden: starts / stops the live views (the wheel stays open)
void release();             // before the game starts: closes the wheel (its effects can belong to one process only)
bool command(HWND c, int id, int code);   // WM_COMMAND; true when it was the tab's
bool draw(const DRAWITEMSTRUCT* d);       // WM_DRAWITEM of the live views
void hscroll(HWND bar);                   // WM_HSCROLL of the sliders
void timer();                             // WM_TIMER TIMER_ID
}  // namespace wheeltab
