/*  InputLeap -- mouse and keyboard sharing utility
    Copyright (C) InputLeap contributors

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "inputleap/clipboard_types.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <chrono>
#include <mutex>
#include <thread>

namespace inputleap {

/*! A small box in the corner of the screen that shows the progress of clipboard contents
    arriving from another screen, like a download. It appears only when a transfer takes a
    noticeable time, never takes the focus and lets clicks through. It has its own thread so
    that it keeps up while the other threads are busy with the transfer.
*/
class MSWindowsClipboardStatusWindow {
public:
    MSWindowsClipboardStatusWindow();
    ~MSWindowsClipboardStatusWindow();

    MSWindowsClipboardStatusWindow(const MSWindowsClipboardStatusWindow&) = delete;
    MSWindowsClipboardStatusWindow& operator=(const MSWindowsClipboardStatusWindow&) = delete;

    /// Reports the latest state of a transfer. May be called from any thread.
    void update(const ClipboardTransferProgress& progress);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    bool create_window();
    void thread_main();
    void on_update();
    void show();
    void hide();
    void paint(HDC hdc);
    int scale(int pixels) const { return pixels * dpi_ / 96; }

    std::thread thread_;
    HWND window_ = nullptr;

    std::mutex mutex_;
    ClipboardTransferProgress latest_;

    // used on the window thread only
    ClipboardTransferProgress shown_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point painted_;
    bool transfer_active_ = false;
    bool visible_ = false;
    int dpi_ = 96;
    HFONT title_font_ = nullptr;
    HFONT text_font_ = nullptr;
};

} // namespace inputleap
