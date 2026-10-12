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

#include "platform/MSWindowsClipboardStatusWindow.h"

#include "inputleap/IClipboard.h"
#include "base/Log.h"

#include <cstdio>
#include <future>

namespace inputleap {

namespace {

const wchar_t* const kStatusWindowClassName = L"InputLeapClipboardStatus";

const UINT kMsgUpdate = WM_APP + 1;
const UINT_PTR kShowTimer = 1;
const UINT_PTR kHideTimer = 2;

// a transfer must take at least this long before the box appears
const UINT kShowDelayMs = 500;
// how long a failure stays on the screen
const UINT kFailureDisplayMs = 2500;
// the box is repainted at most this often
const std::chrono::milliseconds kPaintInterval(80);

// size and placement in pixels at 96 dpi
const int kWidth = 340;
const int kHeight = 78;
const int kMargin = 16;
const int kPadding = 14;
const int kBarHeight = 6;

std::once_flag g_status_window_class_once;
ATOM g_status_window_class = 0;

std::wstring format_bytes(std::uint64_t bytes)
{
    wchar_t text[32];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::swprintf(text, 32, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ull * 1024) {
        std::swprintf(text, 32, L"%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::swprintf(text, 32, L"%.0f KB", bytes / 1024.0);
    }
    return text;
}

const wchar_t* title_for(const ClipboardTransferProgress& progress)
{
    const std::uint32_t images = (1u << IClipboard::kBitmap) | (1u << IClipboard::kPNG) |
                                 (1u << IClipboard::kJpeg) | (1u << IClipboard::kTiff) |
                                 (1u << IClipboard::kWebp);
    if (progress.formats & (1u << IClipboard::kFiles)) {
        return L"Receiving files from another screen";
    }
    if (progress.formats & images) {
        return L"Receiving an image from another screen";
    }
    if (progress.formats & ((1u << IClipboard::kText) | (1u << IClipboard::kHTML))) {
        return L"Receiving text from another screen";
    }
    return L"Receiving the clipboard from another screen";
}

HFONT make_font(int points, int dpi, bool bold)
{
    LOGFONTW font = {};
    font.lfHeight = -MulDiv(points, dpi, 72);
    font.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&font);
}

} // namespace

MSWindowsClipboardStatusWindow::MSWindowsClipboardStatusWindow()
{
    auto started = std::make_shared<std::promise<bool>>();
    auto started_future = started->get_future();
    thread_ = std::thread([this, started]() {
        bool ok = create_window();
        started->set_value(ok);
        if (ok) {
            thread_main();
        }
    });
    if (!started_future.get()) {
        LOG_WARN("failed to create the clipboard status window");
    }
}

MSWindowsClipboardStatusWindow::~MSWindowsClipboardStatusWindow()
{
    if (window_ != nullptr) {
        PostMessageW(window_, WM_CLOSE, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MSWindowsClipboardStatusWindow::update(const ClipboardTransferProgress& progress)
{
    if (window_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = progress;
    }
    PostMessageW(window_, kMsgUpdate, 0, 0);
}

bool MSWindowsClipboardStatusWindow::create_window()
{
    HINSTANCE instance = GetModuleHandleW(nullptr);

    std::call_once(g_status_window_class_once, [instance]() {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &MSWindowsClipboardStatusWindow::wnd_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = kStatusWindowClassName;
        g_status_window_class = RegisterClassExW(&window_class);
    });
    if (g_status_window_class == 0) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (screen != nullptr) {
        dpi_ = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    if (dpi_ <= 0) {
        dpi_ = 96;
    }
    title_font_ = make_font(10, dpi_, true);
    text_font_ = make_font(9, dpi_, false);

    // never activated, always on top, not in the task bar, translucent, and clicks go through
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                              WS_EX_LAYERED | WS_EX_TRANSPARENT,
                              kStatusWindowClassName, L"InputLeap clipboard transfer", WS_POPUP,
                              0, 0, scale(kWidth), scale(kHeight), nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        return false;
    }
    SetLayeredWindowAttributes(window_, 0, 235, LWA_ALPHA);
    return true;
}

void MSWindowsClipboardStatusWindow::thread_main()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (title_font_ != nullptr) {
        DeleteObject(title_font_);
    }
    if (text_font_ != nullptr) {
        DeleteObject(text_font_);
    }
}

LRESULT CALLBACK MSWindowsClipboardStatusWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                                          LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<MSWindowsClipboardStatusWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (self->window_ == nullptr) {
        self->window_ = hwnd;
    }
    return self->handle_message(msg, wparam, lparam);
}

LRESULT MSWindowsClipboardStatusWindow::handle_message(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case kMsgUpdate:
        on_update();
        return 0;

    case WM_TIMER:
        if (wparam == kShowTimer) {
            KillTimer(window_, kShowTimer);
            if (transfer_active_) {
                show();
            }
        } else if (wparam == kHideTimer) {
            KillTimer(window_, kHideTimer);
            hide();
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(window_, &ps);
        if (hdc != nullptr) {
            paint(hdc);
            EndPaint(window_, &ps);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        KillTimer(window_, kShowTimer);
        KillTimer(window_, kHideTimer);
        DestroyWindow(window_);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window_, msg, wparam, lparam);
}

void MSWindowsClipboardStatusWindow::on_update()
{
    ClipboardTransferProgress progress;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress = latest_;
    }
    auto now = std::chrono::steady_clock::now();

    switch (progress.state) {
    case ClipboardTransferProgress::State::Receiving:
        if (!transfer_active_) {
            transfer_active_ = true;
            started_ = now;
            SetTimer(window_, kShowTimer, kShowDelayMs, nullptr);
        }
        shown_ = progress;
        if (visible_ && now - painted_ >= kPaintInterval) {
            painted_ = now;
            InvalidateRect(window_, nullptr, FALSE);
        }
        break;

    case ClipboardTransferProgress::State::Completed:
        transfer_active_ = false;
        KillTimer(window_, kShowTimer);
        hide();
        break;

    case ClipboardTransferProgress::State::Failed:
        transfer_active_ = false;
        KillTimer(window_, kShowTimer);
        shown_ = progress;
        // a failure is only worth showing if the transfer was taking a while
        if (visible_ || now - started_ >= std::chrono::milliseconds(kShowDelayMs)) {
            show();
            painted_ = now;
            InvalidateRect(window_, nullptr, FALSE);
            SetTimer(window_, kHideTimer, kFailureDisplayMs, nullptr);
        }
        break;
    }
}

void MSWindowsClipboardStatusWindow::show()
{
    // bottom right corner of the work area of the primary monitor
    RECT work = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int width = scale(kWidth);
    int height = scale(kHeight);
    SetWindowPos(window_, HWND_TOPMOST, work.right - width - scale(kMargin),
                 work.bottom - height - scale(kMargin), width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void MSWindowsClipboardStatusWindow::hide()
{
    if (visible_) {
        ShowWindow(window_, SW_HIDE);
        visible_ = false;
    }
}

void MSWindowsClipboardStatusWindow::paint(HDC hdc)
{
    RECT client;
    GetClientRect(window_, &client);

    HBRUSH background = CreateSolidBrush(RGB(32, 32, 32));
    HBRUSH border = CreateSolidBrush(RGB(80, 80, 80));
    HBRUSH track = CreateSolidBrush(RGB(70, 70, 70));
    HBRUSH fill = CreateSolidBrush(shown_.state == ClipboardTransferProgress::State::Failed
                                       ? RGB(200, 60, 60) : RGB(0, 120, 215));
    FillRect(hdc, &client, background);
    FrameRect(hdc, &client, border);

    SetBkMode(hdc, TRANSPARENT);
    int padding = scale(kPadding);
    RECT line = {client.left + padding, client.top + scale(10), client.right - padding,
                 client.top + scale(32)};

    HGDIOBJ old_font = SelectObject(hdc, title_font_);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, title_for(shown_), -1, &line,
              DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    std::wstring detail;
    if (shown_.state == ClipboardTransferProgress::State::Failed) {
        detail = L"The transfer failed. Copy the contents again to retry.";
    } else if (shown_.total > 0) {
        detail = format_bytes(shown_.received) + L" of " + format_bytes(shown_.total);
    } else {
        detail = format_bytes(shown_.received);
    }
    line.top = client.top + scale(34);
    line.bottom = client.top + scale(52);
    SelectObject(hdc, text_font_);
    SetTextColor(hdc, RGB(200, 200, 200));
    DrawTextW(hdc, detail.c_str(), -1, &line,
              DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, old_font);

    RECT bar = {client.left + padding, client.bottom - scale(kPadding) - scale(kBarHeight),
                client.right - padding, client.bottom - scale(kPadding)};
    FillRect(hdc, &bar, track);
    if (shown_.state == ClipboardTransferProgress::State::Failed) {
        FillRect(hdc, &bar, fill);
    } else if (shown_.total > 0) {
        RECT done = bar;
        double fraction = static_cast<double>(shown_.received) / static_cast<double>(shown_.total);
        if (fraction > 1.0) {
            fraction = 1.0;
        }
        done.right = bar.left + static_cast<int>((bar.right - bar.left) * fraction);
        FillRect(hdc, &done, fill);
    }

    DeleteObject(background);
    DeleteObject(border);
    DeleteObject(track);
    DeleteObject(fill);
}

} // namespace inputleap
