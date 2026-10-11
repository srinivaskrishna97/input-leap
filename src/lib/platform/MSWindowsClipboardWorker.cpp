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

#include "platform/MSWindowsClipboardWorker.h"

#include "platform/MSWindowsClipboard.h"
#include "inputleap/Clipboard.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <future>

namespace inputleap {

namespace {

const wchar_t* const kClipboardWorkerClassName = L"InputLeapClipboardWorker";

// posted to the worker window
const UINT kClipboardWorkerMsgCommands = WM_APP + 1;
const UINT kClipboardWorkerMsgCapture = WM_APP + 2;

// timer identifiers
const UINT_PTR kClipboardWorkerCaptureTimer = 1;
const UINT_PTR kClipboardWorkerRetryTimer = 2;

// Applications often update the clipboard several times in quick succession, so a capture is
// started only after the clipboard has been quiet for a short while.
const UINT kClipboardCaptureDelayMs = 100;

// Other applications hold the clipboard open only briefly, so failed attempts to open it are
// retried for a while.
const UINT kClipboardRetryDelayMs = 50;
const int kClipboardMaxCaptureRetries = 40;
const int kClipboardMaxApplyRetries = 40;

std::once_flag g_clipboard_worker_class_once;
ATOM g_clipboard_worker_class = 0;

} // namespace

MSWindowsClipboardWorker::MSWindowsClipboardWorker(IEventQueue* events) :
    events_{events}
{
    // shared so that it stays alive until both threads are done with it
    auto started = std::make_shared<std::promise<bool>>();
    auto started_future = started->get_future();
    thread_ = std::thread([this, started]() {
        bool ok = create_window();
        started->set_value(ok);
        if (!ok) {
            return;
        }
        thread_main();
    });

    if (!started_future.get()) {
        LOG_WARN("failed to create the clipboard worker window");
    }
}

MSWindowsClipboardWorker::~MSWindowsClipboardWorker()
{
    if (window_ != nullptr) {
        PostMessageW(window_, WM_CLOSE, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool MSWindowsClipboardWorker::create_window()
{
    HINSTANCE instance = GetModuleHandleW(nullptr);

    std::call_once(g_clipboard_worker_class_once, [instance]() {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &MSWindowsClipboardWorker::wnd_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kClipboardWorkerClassName;
        g_clipboard_worker_class = RegisterClassExW(&window_class);
    });

    if (g_clipboard_worker_class == 0) {
        return false;
    }

    // a message-only window is never shown and does not receive broadcast messages
    window_ = CreateWindowExW(0, kClipboardWorkerClassName, L"InputLeap clipboard", 0,
                              0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    return window_ != nullptr;
}

void MSWindowsClipboardWorker::thread_main()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK MSWindowsClipboardWorker::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                                    LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self = reinterpret_cast<MSWindowsClipboardWorker*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && self->window_ == hwnd) {
        return self->handle_message(msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT MSWindowsClipboardWorker::handle_message(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case kClipboardWorkerMsgCommands:
        process_commands();
        return 0;

    case kClipboardWorkerMsgCapture:
        capture_retries_ = 0;
        schedule_capture(kClipboardCaptureDelayMs);
        return 0;

    case WM_TIMER:
        if (wparam == kClipboardWorkerCaptureTimer) {
            KillTimer(window_, kClipboardWorkerCaptureTimer);
            capture_now();
        } else if (wparam == kClipboardWorkerRetryTimer) {
            KillTimer(window_, kClipboardWorkerRetryTimer);
            process_commands();
        }
        return 0;

    case WM_CLOSE:
        KillTimer(window_, kClipboardWorkerCaptureTimer);
        KillTimer(window_, kClipboardWorkerRetryTimer);
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

void MSWindowsClipboardWorker::post(UINT msg)
{
    if (window_ != nullptr) {
        PostMessageW(window_, msg, 0, 0);
    }
}

void MSWindowsClipboardWorker::request_capture()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_pending_ = true;
    }
    post(kClipboardWorkerMsgCapture);
}

void MSWindowsClipboardWorker::poll()
{
    DWORD sequence = GetClipboardSequenceNumber();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_pending_ || (snapshot_ && snapshot_sequence_ == sequence)) {
            return;
        }
    }
    if (MSWindowsClipboard::is_owned_by_us()) {
        return;
    }
    LOG_DEBUG("clipboard changed without notification");
    request_capture();
}

MarshalledClipboardPtr MSWindowsClipboardWorker::current_snapshot() const
{
    DWORD sequence = GetClipboardSequenceNumber();
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_ && snapshot_sequence_ == sequence) {
        return snapshot_;
    }
    return nullptr;
}

void MSWindowsClipboardWorker::apply(MarshalledClipboardPtr data)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Command command;
        command.kind = Command::Kind::Apply;
        command.data = std::move(data);
        commands_.push_back(std::move(command));
    }
    post(kClipboardWorkerMsgCommands);
}

void MSWindowsClipboardWorker::clear()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Command command;
        command.kind = Command::Kind::Clear;
        commands_.push_back(std::move(command));
    }
    post(kClipboardWorkerMsgCommands);
}

void MSWindowsClipboardWorker::process_commands()
{
    while (true) {
        Command command;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (commands_.empty()) {
                return;
            }
            command = std::move(commands_.front());
            commands_.pop_front();

            // only the latest of several queued updates matters
            while (command.kind == Command::Kind::Apply && !commands_.empty() &&
                   commands_.front().kind == Command::Kind::Apply) {
                command = std::move(commands_.front());
                commands_.pop_front();
            }
        }

        bool ok = command.kind == Command::Kind::Apply ? apply_now(command.data) : clear_now();
        if (!ok) {
            if (apply_retries_ < kClipboardMaxApplyRetries) {
                // another application holds the clipboard open, try again shortly
                ++apply_retries_;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    commands_.push_front(std::move(command));
                }
                SetTimer(window_, kClipboardWorkerRetryTimer, kClipboardRetryDelayMs, nullptr);
                return;
            }
            LOG_WARN("failed to update the clipboard");
        }
        apply_retries_ = 0;
    }
}

bool MSWindowsClipboardWorker::apply_now(const MarshalledClipboardPtr& data)
{
    const auto& contents = data ? data : empty_marshalled_clipboard();

    // The same contents are often set twice in a row because Windows has a single clipboard
    // that serves both InputLeap clipboards.
    if (last_applied_ && same_clipboard_contents(last_applied_, contents) &&
        last_applied_sequence_ == GetClipboardSequenceNumber()) {
        LOG_DEBUG("clipboard already up to date");
        return true;
    }

    Clipboard source;
    source.unmarshall(contents->data(), 0);

    MSWindowsClipboard destination(window_);
    if (!Clipboard::copy(&destination, &source)) {
        return false;
    }

    last_applied_ = contents;
    last_applied_sequence_ = GetClipboardSequenceNumber();
    LOG_DEBUG("updated clipboard, size=%zu", contents->size());
    return true;
}

bool MSWindowsClipboardWorker::clear_now()
{
    MSWindowsClipboard clipboard(window_);
    if (!clipboard.open(0)) {
        return false;
    }
    clipboard.clear();
    clipboard.close();
    last_applied_.reset();
    return true;
}

void MSWindowsClipboardWorker::schedule_capture(UINT delay_ms)
{
    // restarts the timer if it is already running
    SetTimer(window_, kClipboardWorkerCaptureTimer, delay_ms, nullptr);
}

void MSWindowsClipboardWorker::capture_now()
{
    DWORD sequence = GetClipboardSequenceNumber();

    // contents put on the clipboard by InputLeap came from another screen
    if (MSWindowsClipboard::is_owned_by_us()) {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_pending_ = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_ && snapshot_sequence_ == sequence) {
            capture_pending_ = false;
            return;
        }
    }

    MSWindowsClipboard source(window_);
    Clipboard clipboard;
    if (!Clipboard::copy(&clipboard, &source)) {
        if (++capture_retries_ <= kClipboardMaxCaptureRetries) {
            schedule_capture(kClipboardRetryDelayMs);
            return;
        }
        LOG_WARN("failed to read the clipboard");
        std::lock_guard<std::mutex> lock(mutex_);
        capture_pending_ = false;
        return;
    }

    auto data = marshall_clipboard(clipboard);

    if (GetClipboardSequenceNumber() != sequence) {
        // the clipboard changed while it was being read
        schedule_capture(kClipboardCaptureDelayMs);
        return;
    }

    capture_retries_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = data;
        snapshot_sequence_ = sequence;
        capture_pending_ = false;
    }

    LOG_DEBUG("captured clipboard, size=%zu", data->size());
    events_->add_event(EventType::CLIPBOARD_CAPTURED, &target_);
}

} // namespace inputleap
