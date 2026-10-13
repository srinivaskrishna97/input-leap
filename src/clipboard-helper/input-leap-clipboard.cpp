/*
 * InputLeap -- mouse and keyboard sharing utility
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The clipboard helper, see ClipboardHelperMessages.h.
 *
 * usage: input-leap-clipboard <core process id> <core window>
 *
 * A server or client that the service started runs with high privileges, and Windows withholds
 * clipboard contents that applications hand over on request from such a process. The core
 * therefore starts this program as the user at the console. It watches the clipboard, captures
 * the contents with the same converters as the core, and sends them to the core's clipboard
 * window. It exits when the core does.
 */

#include "inputleap/Clipboard.h"
#include "inputleap/ClipboardData.h"
#include "inputleap/ClipboardHelperMessages.h"
#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsDebugOutputter.h"
#include "arch/Arch.h"
#include "arch/win32/ArchMiscWindows.h"
#include "base/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

// the logging macros refer to inputleap::Log unqualified
using namespace inputleap;

namespace {

const wchar_t* const kHelperWindowClass = L"InputLeapClipboardHelper";
const UINT_PTR kCaptureTimer = 1;

// like the core's worker: applications often change the clipboard several times in quick
// succession, so a capture starts only after the clipboard has been quiet for a short while
const UINT kCaptureDelayMs = 100;
const UINT kRetryDelayMs = 50;
const int kMaxRetries = 40;

HWND g_window = nullptr;
HWND g_core = nullptr;
int g_retries = 0;

bool send_to_core(ULONG_PTR magic, const std::string& payload)
{
    COPYDATASTRUCT data;
    data.dwData = magic;
    data.cbData = static_cast<DWORD>(payload.size());
    data.lpData = const_cast<char*>(payload.data());
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(g_core, WM_COPYDATA, reinterpret_cast<WPARAM>(g_window),
                             reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                             10000, &result)) {
        LOG_WARN("cannot send to the core (error %lu)", GetLastError());
        return false;
    }
    return result != 0;
}

void schedule_capture(UINT delay_ms)
{
    // restarts the timer if it is already running
    SetTimer(g_window, kCaptureTimer, delay_ms, nullptr);
}

void capture_and_send()
{
    // contents put on the clipboard by InputLeap came from another screen
    if (MSWindowsClipboard::is_owned_by_us()) {
        return;
    }

    DWORD sequence = GetClipboardSequenceNumber();
    MSWindowsClipboard source(g_window);
    Clipboard clipboard;
    if (!Clipboard::copy(&clipboard, &source)) {
        // another application holds the clipboard open, usually only briefly
        if (++g_retries <= kMaxRetries) {
            schedule_capture(kRetryDelayMs);
        }
        else {
            LOG_WARN("cannot read the clipboard");
            g_retries = 0;
        }
        return;
    }
    g_retries = 0;

    if (GetClipboardSequenceNumber() != sequence) {
        // the clipboard changed while it was being read
        schedule_capture(kCaptureDelayMs);
        return;
    }

    MarshalledClipboardPtr data = marshall_clipboard(clipboard);
    LOG_DEBUG("captured clipboard sequence %lu, size=%zu", static_cast<unsigned long>(sequence),
              data->size());
    if (!send_to_core(kClipboardHelperCaptureMagic,
                      encode_clipboard_capture(sequence, data->data()))) {
        LOG_WARN("the core did not take the clipboard contents");
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CLIPBOARDUPDATE:
        g_retries = 0;
        schedule_capture(kCaptureDelayMs);
        return 0;

    case WM_TIMER:
        if (wparam == kCaptureTimer) {
            KillTimer(hwnd, kCaptureTimer);
            capture_and_send();
        }
        return 0;

    case WM_COPYDATA: {
        // the core asks for a capture
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (data != nullptr && data->dwData == inputleap::kClipboardHelperRequestMagic) {
            g_retries = 0;
            schedule_capture(USER_TIMER_MINIMUM);
            return TRUE;
        }
        return FALSE;
    }

    case WM_CLOSE:
        KillTimer(hwnd, kCaptureTimer);
        RemoveClipboardFormatListener(hwnd);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// exits when the core exits, so that no helper is left behind
void watch_core(DWORD pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        LOG_WARN("cannot watch the core process %lu", static_cast<unsigned long>(pid));
        return;
    }
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
    PostMessageW(g_window, WM_CLOSE, 0, 0);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    ArchMiscWindows::setInstanceWin32(instance);
    Arch arch;
    arch.init();
    Log log;
    log.insert(new MSWindowsDebugOutputter());
    log.setFilter("DEBUG");

    if (__argc != 3) {
        LOG_ERR("usage: input-leap-clipboard <core process id> <core window>");
        return 1;
    }
    DWORD core_pid = static_cast<DWORD>(std::strtoul(__argv[1], nullptr, 10));
    g_core = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(std::strtoull(__argv[2], nullptr, 10)));
    if (core_pid == 0 || g_core == nullptr || !IsWindow(g_core)) {
        LOG_ERR("the core's clipboard window %s does not exist", __argv[2]);
        return 1;
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &wnd_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kHelperWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        LOG_ERR("cannot register the helper window class (error %lu)", GetLastError());
        return 1;
    }
    // a message-only window is never shown and does not receive broadcast messages
    g_window = CreateWindowExW(0, kHelperWindowClass, L"InputLeap clipboard helper", 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (g_window == nullptr) {
        LOG_ERR("cannot create the helper window (error %lu)", GetLastError());
        return 1;
    }
    AddClipboardFormatListener(g_window);

    std::thread watcher(watch_core, core_pid);
    watcher.detach();

    if (!send_to_core(kClipboardHelperHelloMagic,
                      encode_clipboard_helper_hello(
                          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g_window)),
                          GetCurrentProcessId()))) {
        LOG_WARN("the core did not acknowledge the clipboard helper");
    }
    LOG_INFO("clipboard helper running for process %lu", static_cast<unsigned long>(core_pid));

    // the contents that are on the clipboard now
    schedule_capture(kCaptureDelayMs);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
