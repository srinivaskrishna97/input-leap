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

#include "base/EventTarget.h"
#include "inputleap/ClipboardData.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace inputleap {

class IEventQueue;

/*! Reads and writes the Windows clipboard on a dedicated thread.

Converting large clipboard contents such as screenshots takes a noticeable amount of time.
Doing it on the main thread blocks input processing, which makes the mouse cursor freeze and
then jump. The worker therefore captures the clipboard in the background shortly after it
changes, so that the contents are ready by the time they need to be sent to another screen, and
applies contents received from other screens in the background.

Contents of other screens can also be offered without their data. The worker then puts only
the formats on the clipboard (delayed rendering). When an application pastes, Windows asks the
worker for the data, the worker sends a CLIPBOARD_DATA_REQUESTED event and waits until the data
is supplied with deliver(). Only the pasting application waits; input handling continues.

All writes to the clipboard go through the worker so that they are applied in order.
*/
class MSWindowsClipboardWorker {
public:
    explicit MSWindowsClipboardWorker(IEventQueue* events);
    ~MSWindowsClipboardWorker();

    MSWindowsClipboardWorker(const MSWindowsClipboardWorker&) = delete;
    MSWindowsClipboardWorker& operator=(const MSWindowsClipboardWorker&) = delete;

    /*! Target of the events that the worker sends: CLIPBOARD_CAPTURED when a capture has
        completed and CLIPBOARD_DATA_REQUESTED (with a ClipboardOffer) when an application
        requests the data of offered contents.
    */
    const EventTarget* event_target() const { return &target_; }

    /// Returns false if the worker thread could not be started.
    bool is_running() const { return window_ != nullptr; }

    /*! Requests a capture of the clipboard. Called whenever the clipboard changes. The capture
        is delayed slightly so that several changes in quick succession are captured once.
        Contents that were put on the clipboard by InputLeap are not captured.
    */
    void request_capture();

    /*! Requests a capture if the clipboard has changed since the last capture and no capture is
        pending. Used as a fallback for missed clipboard change notifications.
    */
    void poll();

    /*! Returns the captured contents if they are the current contents of the clipboard, or
        nullptr otherwise, e.g. while a capture is in progress.
    */
    MarshalledClipboardPtr current_snapshot() const;

    /// Replaces the contents of the clipboard. Applied asynchronously.
    void apply(MarshalledClipboardPtr data);

    /// Empties the clipboard and marks it as owned by InputLeap. Applied asynchronously.
    void clear();

    /// Puts offered contents on the clipboard without their data. Applied asynchronously.
    void offer(const ClipboardOffer& offer);

    /*! Supplies the data of the offered contents. Returns false if \p data does not belong to
        the offer that is on the clipboard, in which case it should be applied with apply().
    */
    bool deliver(const MarshalledClipboardPtr& data);

    /// Tells the worker that the data of \p offer cannot be obtained.
    void offer_unavailable(const ClipboardOffer& offer);

    /*! Tells the worker that the data of the offered contents cannot be obtained anymore,
        e.g. because the connection to the other screens has been closed. Pastes then fail
        right away instead of waiting.
    */
    void offers_unavailable();

private:
    struct Command {
        enum class Kind { Apply, Clear, Offer };
        Kind kind = Kind::Apply;
        MarshalledClipboardPtr data;
        ClipboardOffer offer;
    };

    // contents offered without data that are currently on the clipboard
    struct OfferState {
        bool active = false;
        ClipboardOffer offer;
        MarshalledClipboardPtr data;
        bool requested = false;
        bool unavailable = false;
    };

    bool create_window();
    void thread_main();
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    void post(UINT msg);
    void enqueue(Command command);
    void process_commands();
    bool apply_now(const MarshalledClipboardPtr& data);
    bool clear_now();
    bool offer_now(const ClipboardOffer& offer);
    void capture_now();
    void schedule_capture(UINT delay_ms);

    void render_format(UINT win32_format);
    void render_all_formats();
    MarshalledClipboardPtr wait_for_offer_data();
    void reset_offer();

    IEventQueue* events_;
    EventTarget target_;

    std::thread thread_;
    HWND window_ = nullptr;

    // state shared between the threads
    mutable std::mutex mutex_;
    std::condition_variable offer_cv_;
    std::deque<Command> commands_;
    MarshalledClipboardPtr snapshot_;
    DWORD snapshot_sequence_ = 0;
    bool capture_pending_ = false;
    OfferState offer_;

    // state used only on the worker thread
    MarshalledClipboardPtr last_applied_;
    DWORD last_applied_sequence_ = 0;
    int capture_retries_ = 0;
    int apply_retries_ = 0;
};

} // namespace inputleap
