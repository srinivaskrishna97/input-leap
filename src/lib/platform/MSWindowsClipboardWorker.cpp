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

#include <chrono>
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

// How long a pasting application waits for offered contents to arrive from another screen.
const int kClipboardRenderTimeoutSeconds = 30;

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

    case WM_RENDERFORMAT:
        // an application requests the data of offered contents
        render_format(static_cast<UINT>(wparam));
        return 0;

    case WM_RENDERALLFORMATS:
        // the window is being destroyed while the clipboard holds offered contents
        render_all_formats();
        return 0;

    case WM_DESTROYCLIPBOARD:
        // the clipboard was emptied, so offered contents are gone
        reset_offer();
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

void MSWindowsClipboardWorker::enqueue(Command command)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(std::move(command));
    }
    post(kClipboardWorkerMsgCommands);
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
    Command command;
    command.kind = Command::Kind::Apply;
    command.data = std::move(data);
    enqueue(std::move(command));
}

void MSWindowsClipboardWorker::clear()
{
    Command command;
    command.kind = Command::Kind::Clear;
    enqueue(std::move(command));
}

void MSWindowsClipboardWorker::offer(const ClipboardOffer& offer)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (offer_.active && offer_.offer != offer) {
            // a paste waiting for the previous contents won't get them anymore
            offer_.unavailable = true;
            offer_cv_.notify_all();
        }
    }

    Command command;
    command.kind = Command::Kind::Offer;
    command.offer = offer;
    enqueue(std::move(command));
}

bool MSWindowsClipboardWorker::deliver(const MarshalledClipboardPtr& data)
{
    if (!data) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!offer_.active || !offer_.offer.describes(*data)) {
        return false;
    }
    offer_.data = data;
    offer_cv_.notify_all();
    LOG_DEBUG("received offered clipboard data, size=%zu", data->size());
    return true;
}

void MSWindowsClipboardWorker::offer_unavailable(const ClipboardOffer& offer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (offer_.active && offer_.offer.hash == offer.hash) {
        offer_.unavailable = true;
        offer_cv_.notify_all();
    }
}

void MSWindowsClipboardWorker::offers_unavailable()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (offer_.active && !offer_.data) {
        offer_.unavailable = true;
        offer_cv_.notify_all();
    }
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

        bool ok = false;
        switch (command.kind) {
        case Command::Kind::Apply:
            ok = apply_now(command.data);
            break;
        case Command::Kind::Clear:
            ok = clear_now();
            break;
        case Command::Kind::Offer:
            ok = offer_now(command.offer);
            break;
        }

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

    // replaces offered contents, if any (Windows sends WM_DESTROYCLIPBOARD)
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

bool MSWindowsClipboardWorker::offer_now(const ClipboardOffer& offer)
{
    {
        // the same contents are often offered for both InputLeap clipboards
        std::lock_guard<std::mutex> lock(mutex_);
        if (offer_.active && offer_.offer == offer && GetClipboardOwner() == window_) {
            return true;
        }
    }

    MSWindowsClipboard clipboard(window_);
    if (!clipboard.open(0)) {
        return false;
    }
    if (!clipboard.clear()) {
        clipboard.close();
        return false;
    }
    for (int format = 0; format < IClipboard::kNumFormats; ++format) {
        if ((offer.formats & (1u << format)) != 0) {
            clipboard.addDelayed(static_cast<IClipboard::EFormat>(format));
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        offer_ = OfferState{};
        offer_.active = true;
        offer_.offer = offer;
    }
    clipboard.close();

    last_applied_.reset();
    LOG_DEBUG("offered clipboard contents, size=%u", offer.size);
    return true;
}

void MSWindowsClipboardWorker::reset_offer()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (offer_.active) {
        offer_ = OfferState{};
        offer_cv_.notify_all();
    }
}

MarshalledClipboardPtr MSWindowsClipboardWorker::wait_for_offer_data()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!offer_.active || offer_.unavailable) {
        return nullptr;
    }
    if (offer_.data) {
        return offer_.data;
    }

    const ClipboardOffer offer = offer_.offer;
    if (!offer_.requested) {
        offer_.requested = true;
        lock.unlock();
        LOG_DEBUG("requesting offered clipboard data, size=%u", offer.size);
        events_->add_event(EventType::CLIPBOARD_DATA_REQUESTED, &target_,
                           create_event_data<ClipboardOffer>(offer));
        lock.lock();
    }

    bool done = offer_cv_.wait_for(lock, std::chrono::seconds(kClipboardRenderTimeoutSeconds),
                                   [this, &offer]() {
        return !offer_.active || offer_.offer != offer || offer_.data || offer_.unavailable;
    });

    if (offer_.active && offer_.offer == offer) {
        if (offer_.data) {
            return offer_.data;
        }
        // allow a later paste to request the data again
        offer_.requested = false;
        offer_.unavailable = false;
    }
    if (!done) {
        LOG_WARN("timed out waiting for clipboard data from another screen");
    }
    return nullptr;
}

void MSWindowsClipboardWorker::render_format(UINT win32_format)
{
    MSWindowsClipboard clipboard(window_);
    IClipboard::EFormat format;
    if (!clipboard.getFormatForWin32(win32_format, format)) {
        return;
    }

    MarshalledClipboardPtr data = wait_for_offer_data();
    if (!data) {
        LOG_WARN("clipboard data from another screen is not available");
        return;
    }

    Clipboard contents;
    contents.unmarshall(data->data(), 0);
    if (contents.open(0)) {
        if (contents.has(format)) {
            // the clipboard is already open for the requesting application, so the data is
            // added without opening it
            clipboard.add(format, contents.get(format));
        }
        contents.close();
    }
}

void MSWindowsClipboardWorker::render_all_formats()
{
    MarshalledClipboardPtr data;
    std::uint32_t formats = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (offer_.active) {
            data = offer_.data;
            formats = offer_.offer.formats;
        }
    }
    // the data cannot be requested from the other screen anymore at this point
    if (!data) {
        return;
    }

    if (!OpenClipboard(window_)) {
        return;
    }
    if (GetClipboardOwner() == window_) {
        Clipboard contents;
        contents.unmarshall(data->data(), 0);
        MSWindowsClipboard clipboard(window_);
        if (contents.open(0)) {
            for (int format = 0; format < IClipboard::kNumFormats; ++format) {
                auto eformat = static_cast<IClipboard::EFormat>(format);
                if ((formats & (1u << format)) != 0 && contents.has(eformat)) {
                    clipboard.add(eformat, contents.get(eformat));
                }
            }
            contents.close();
        }
    }
    CloseClipboard();
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

    // prepare the compressed form now, so that sending it later does not have to
    if (data->size() >= kClipboardEagerSize) {
        data->rle32();
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
