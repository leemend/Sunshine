/**
 * @file src/platform/windows/connection_gate_hotkey.cpp
 */
#include "connection_gate_hotkey.h"

#include <thread>

#include <windows.h>

#include "../../connection_gate.h"
#include "../../logging.h"

using namespace std::literals;

namespace connection_gate_hotkey {

  namespace {
    constexpr int HOTKEY_ID = 1;

    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
      if (msg == WM_HOTKEY && wparam == HOTKEY_ID) {
        // Simple toggle: if the gate is currently letting connections through (whatever the
        // reason - open, or an auto mode that hasn't closed yet), close it outright. Otherwise
        // open it outright. Deliberately not "cycle through all four modes" - this is meant for
        // the "I need to flip this open/closed right now" case, not full mode selection (that's
        // what the systray/web UI are for).
        bool currently_open = connection_gate::is_open();
        connection_gate::set_mode(currently_open ? connection_gate::mode_e::closed : connection_gate::mode_e::open);
        BOOST_LOG(info) << "connection_gate: toggled via global hotkey to \""sv
                         << connection_gate::mode_to_string(connection_gate::current_mode()) << "\""sv;
        return 0;
      }

      return DefWindowProc(hwnd, msg, wparam, lparam);
    }

    void run() {
      const wchar_t *class_name = L"SunshineConnectionGateHotkeyWnd";

      WNDCLASSW wc {};
      wc.lpfnWndProc = wnd_proc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = class_name;

      if (!RegisterClassW(&wc)) {
        BOOST_LOG(warning) << "connection_gate_hotkey: failed to register window class, error "sv << GetLastError();
        return;
      }

      // Message-only window (HWND_MESSAGE parent) - no visible window needed, this only exists
      // to receive WM_HOTKEY on its own dedicated message queue.
      HWND hwnd = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
      if (!hwnd) {
        BOOST_LOG(warning) << "connection_gate_hotkey: failed to create message window, error "sv << GetLastError();
        return;
      }

      // Ctrl+Alt+Shift+O. MOD_NOREPEAT so holding the key down doesn't fire repeated toggles.
      if (!RegisterHotKey(hwnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'O')) {
        BOOST_LOG(warning) << "connection_gate_hotkey: failed to register hotkey (Ctrl+Alt+Shift+O may already be in use by another application), error "sv << GetLastError();
        return;
      }

      BOOST_LOG(info) << "connection_gate_hotkey: registered Ctrl+Alt+Shift+O to toggle the connection gate"sv;

      MSG msg;
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }

      UnregisterHotKey(hwnd, HOTKEY_ID);
    }
  }  // namespace

  void init() {
    std::thread(run).detach();
  }

}  // namespace connection_gate_hotkey
