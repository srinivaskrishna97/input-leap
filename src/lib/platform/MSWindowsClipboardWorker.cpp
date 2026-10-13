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
#include "inputleap/ClipboardFiles.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <chrono>
#include <cstring>
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

// Right after files are copied in Explorer, the clipboard holds only OLE's markers, which
// InputLeap does not recognize, and the GUI sends the paths of the files a moment later. A
// capture that finds nothing waits this long for them, so that one copy makes one offer.
const UINT kClipboardEmptyCaptureWaitMs = 50;
const int kClipboardMaxEmptyCaptureWaits = 5;

// Identifies the file lists that the GUI sends with WM_COPYDATA, see ClipboardFileForwarder
const ULONG_PTR kClipboardFileListMagic = 0x494C4650; // "ILFP"

// How long a pasting application waits for offered contents to arrive from another screen:
// a fixed time plus an allowance that grows with the size of the contents.
const int kClipboardRenderTimeoutSeconds = 30;
const std::uint64_t kClipboardRenderBytesPerSecond = 512 * 1024;

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
    if (window_ == nullptr) {
        return false;
    }

    // InputLeap usually runs with higher privileges than the applications that put data on
    // the clipboard.  Windows then withholds file lists (CF_HDROP) from it unless the window
    // allows the messages that carry them across privilege levels.
    const UINT kMsgCopyGlobalData = 0x0049;
    const UINT kMsgCopyData = 0x004A;
    const UINT kMsgDropFiles = 0x0233;
    for (UINT msg : {kMsgCopyGlobalData, kMsgCopyData, kMsgDropFiles}) {
        if (!ChangeWindowMessageFilterEx(window_, msg, MSGFLT_ALLOW, nullptr)) {
            LOG_DEBUG("cannot allow message 0x%x for the clipboard window (error %lu)", msg,
                      GetLastError());
        }
    }
    return true;
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
        empty_capture_waits_ = 0;
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

    case WM_COPYDATA:
        // the GUI, which runs as the user, sends the paths of the files on the clipboard
        return on_copy_data(reinterpret_cast<const COPYDATASTRUCT*>(lparam)) ? TRUE : FALSE;

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

bool MSWindowsClipboardWorker::on_copy_data(const COPYDATASTRUCT* data)
{
    if (data == nullptr || data->dwData != kClipboardFileListMagic || data->lpData == nullptr ||
        data->cbData < sizeof(DWORD)) {
        return false;
    }

    DWORD sequence = 0;
    std::memcpy(&sequence, data->lpData, sizeof(DWORD));

    // the paths, each terminated by a null and the list by another one
    const auto* chars = reinterpret_cast<const wchar_t*>(
        static_cast<const char*>(data->lpData) + sizeof(DWORD));
    std::size_t count = (data->cbData - sizeof(DWORD)) / sizeof(wchar_t);
    std::vector<fs::path> paths;
    std::wstring current;
    for (std::size_t i = 0; i < count; ++i) {
        if (chars[i] != L'\0') {
            current.push_back(chars[i]);
        } else if (current.empty()) {
            break;
        } else {
            paths.emplace_back(current);
            current.clear();
        }
    }
    if (paths.empty()) {
        return false;
    }
    LOG_DEBUG("received %zu file paths from the GUI for clipboard sequence %lu", paths.size(),
              static_cast<unsigned long>(sequence));

    if (MSWindowsClipboard::is_owned_by_us()) {
        // the files came from another screen
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        user_files_ = std::move(paths);
        user_files_sequence_ = sequence;
        // A snapshot taken without the files is replaced by a new capture. It stays available
        // until then, because it may have been offered and requested already.
        capture_pending_ = true;
    }
    capture_retries_ = 0;
    schedule_capture(kClipboardCaptureDelayMs);
    return true;
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
    // the clipboard changed just now, which is how old its contents will be
    DWORD sequence = GetClipboardSequenceNumber();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notified_ = true;
        notified_sequence_ = sequence;
        notified_time_ = std::chrono::steady_clock::now();
    }
    post_capture();
}

void MSWindowsClipboardWorker::post_capture()
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
    post_capture();
}

std::uint32_t MSWindowsClipboardWorker::snapshot_age_ms() const
{
    DWORD sequence = GetClipboardSequenceNumber();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!snapshot_ || snapshot_sequence_ != sequence || !snapshot_age_known_) {
        return kClipboardAgeUnknown;
    }
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - snapshot_changed_).count();
    if (age < 0) {
        return 0;
    }
    if (age >= static_cast<long long>(kClipboardAgeUnknown)) {
        return kClipboardAgeUnknown - 1;
    }
    return static_cast<std::uint32_t>(age);
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

    // The data often arrives right after the offer, before the worker has put the offer on the
    // clipboard. It then goes with the offer, as applying it instead would replace the offer.
    if (!commands_.empty()) {
        Command& latest = commands_.back();
        if (latest.kind == Command::Kind::Offer && latest.offer.describes(*data)) {
            latest.data = data;
            LOG_DEBUG("received offered clipboard data, size=%zu", data->size());
            return true;
        }
    }

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
            ok = offer_now(command.offer, command.data);
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

bool MSWindowsClipboardWorker::offer_now(const ClipboardOffer& offer,
                                         const MarshalledClipboardPtr& data)
{
    {
        // the same contents are often offered for both InputLeap clipboards
        std::lock_guard<std::mutex> lock(mutex_);
        if (offer_.active && offer_.offer == offer && GetClipboardOwner() == window_) {
            if (data && !offer_.data) {
                offer_.data = data;
                offer_cv_.notify_all();
            }
            return true;
        }
    }

    // The contents may already be on the clipboard with their data, e.g. when the data was
    // applied because it arrived while the worker was busy putting an earlier offer of the
    // same contents on the clipboard. Offering them again would throw the data away.
    if (last_applied_ && offer.describes(*last_applied_) &&
        last_applied_sequence_ == GetClipboardSequenceNumber()) {
        return true;
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
    if ((offer.formats & (1u << IClipboard::kFiles)) != 0) {
        // tells Explorer to copy, rather than move, the files when they are pasted
        SetClipboardData(MSWindowsClipboard::preferredDropEffectFormat(), nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        offer_ = OfferState{};
        offer_.active = true;
        offer_.offer = offer;
        offer_.data = data;
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

    auto timeout = std::chrono::seconds(
        kClipboardRenderTimeoutSeconds +
        static_cast<long long>(offer.size / kClipboardRenderBytesPerSecond));
    bool done = offer_cv_.wait_for(lock, timeout,
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
    if (win32_format == MSWindowsClipboard::preferredDropEffectFormat()) {
        clipboard.addPreferredDropEffect();
        return;
    }
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
            if ((formats & (1u << IClipboard::kFiles)) != 0) {
                clipboard.addPreferredDropEffect();
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
        // the snapshot is up to date, unless the GUI has sent the files on the clipboard since
        // it was taken without them
        bool files_missing = user_files_sequence_ == sequence && !user_files_.empty() &&
                             !snapshot_has_files_;
        if (snapshot_ && snapshot_sequence_ == sequence && !files_missing) {
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

    // Windows hides file lists from this process when it has higher privileges than the
    // application that copied the files, which is the case when the service started it.  The
    // GUI, which runs as the user, sends the paths of the files instead.
    std::vector<fs::path> user_files;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (user_files_sequence_ == sequence) {
            user_files = user_files_;
        }
    }
    bool has_files = false;
    if (clipboard.open(0)) {
        if (!user_files.empty() && !clipboard.has(IClipboard::kFiles)) {
            std::string files;
            if (read_clipboard_files(user_files, kClipboardFilesMaxBytes, files)) {
                clipboard.add(IClipboard::kFiles, files);
                LOG_INFO("files on clipboard: %zu items, %zu bytes (paths from the GUI)",
                         user_files.size(), files.size());
            }
        }
        has_files = clipboard.has(IClipboard::kFiles);
        clipboard.close();
    }

    auto data = marshall_clipboard(clipboard);

    if (data->size() <= 4 && user_files.empty() &&
        empty_capture_waits_ < kClipboardMaxEmptyCaptureWaits) {
        // nothing recognized yet: give the GUI a moment to send the paths of copied files
        ++empty_capture_waits_;
        schedule_capture(kClipboardEmptyCaptureWaitMs);
        return;
    }

    if (data->size() <= 4) {
        // nothing recognized: log which formats the clipboard has, to see what was missed
        std::string formats;
        if (OpenClipboard(window_)) {
            for (UINT format = EnumClipboardFormats(0); format != 0;
                 format = EnumClipboardFormats(format)) {
                wchar_t name[64] = {};
                int length = GetClipboardFormatNameW(format, name, 64);
                std::string entry = std::to_string(format);
                if (length > 0) {
                    entry += ':';
                    for (int i = 0; i < length; ++i) {
                        entry += name[i] < 128 ? static_cast<char>(name[i]) : '?';
                    }
                }
                formats += (formats.empty() ? "" : ",") + entry;
            }
            CloseClipboard();
        } else {
            formats = "(clipboard busy)";
        }
        LOG_DEBUG("clipboard has no known formats; win32 formats: %s", formats.c_str());
    }

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
    empty_capture_waits_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = data;
        snapshot_sequence_ = sequence;
        snapshot_has_files_ = has_files;
        snapshot_age_known_ = notified_ && notified_sequence_ == sequence;
        snapshot_changed_ = notified_time_;
        capture_pending_ = false;
    }

    LOG_DEBUG("captured clipboard, size=%zu", data->size());
    events_->add_event(EventType::CLIPBOARD_CAPTURED, &target_);
}

} // namespace inputleap
