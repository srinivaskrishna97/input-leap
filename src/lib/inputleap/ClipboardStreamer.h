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
#include "inputleap/ClipboardChunk.h"
#include "inputleap/ClipboardData.h"
#include "inputleap/clipboard_types.h"
#include "io/filesystem.h"

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace inputleap {

class EventQueueTimer;
class IEventQueue;

/// The most clipboard data sent in one chunk message.
constexpr std::size_t kClipboardChunkSize = 32 * 1024;

/// The default amount of clipboard data that a ClipboardStreamer keeps in the output buffer.
constexpr std::size_t kClipboardStreamerWindowSize = 2 * kClipboardChunkSize;

/*! Sends clipboard contents over a connection without delaying other messages.

Clipboard contents are sent as a sequence of chunk messages. Writing all chunks at once queues
the whole clipboard (tens of megabytes for a screenshot) ahead of any mouse or keyboard message
written afterwards, so input freezes until the clipboard has been transmitted and then jumps.

The streamer keeps at most window_size bytes of clipboard data in the connection's output buffer
and writes the next window when the stream reports that its output has been flushed. A timer
with an increasing interval acts as a fallback in case no such notification arrives. Messages
written in the meantime are therefore delayed by at most one window.

Transfers are sent one at a time because the receiver reassembles one clipboard at a time.
Sending new contents for a clipboard replaces a pending or partially sent transfer of the same
clipboard. The receiver discards partial data when it sees the start of the new transfer.
*/
class ClipboardStreamer : public EventTarget {
public:
    /*! Writes a clipboard chunk message to the connection. \p encoded is true for chunks of
        transfers queued with send_encoded(), which use kMsgDClipboardData instead of
        kMsgDClipboard.
    */
    using ChunkWriter = std::function<void(const ClipboardChunk& chunk, bool encoded)>;

    /*! \p stream_target is the event target of the connection's stream. The streamer listens for
        STREAM_OUTPUT_FLUSHED events on it.
    */
    ClipboardStreamer(IEventQueue* events, const EventTarget* stream_target, ChunkWriter writer,
                      std::size_t window_size = kClipboardStreamerWindowSize);
    ~ClipboardStreamer();

    ClipboardStreamer(const ClipboardStreamer&) = delete;
    ClipboardStreamer& operator=(const ClipboardStreamer&) = delete;

    /*! Queues \p data for sending as clipboard \p id. The first window is written immediately if
        nothing else is being sent.
    */
    void send(ClipboardID id, std::uint32_t sequence, MarshalledClipboardPtr data);

    /*! Queues already encoded data. \p start_payload is put into the start chunk and \p payload
        is split into the data chunks. This allows senders to use their own encoding of the start
        chunk, e.g. to describe compressed data.
    */
    void send_encoded(ClipboardID id, std::uint32_t sequence, std::string start_payload,
                      std::shared_ptr<const std::string> payload);

    /*! Queues \p size bytes of a file, from \p offset on, read from the file while they are
        sent. \p start_payload is put into the start chunk. If the file cannot be read, the
        transfer ends short and the receiver rejects it. */
    void send_file(ClipboardID id, std::uint32_t sequence, std::string start_payload,
                   const fs::path& file, std::uint64_t offset, std::uint64_t size);

    /// Drops all pending transfers. A partially sent transfer is left incomplete.
    void cancel();

    /// Drops pending transfers of clipboard \p id, e.g. because its contents have changed.
    void cancel(ClipboardID id);

    /// Returns true if nothing is being sent.
    bool is_idle() const { return transfers_.empty(); }

    /// Total number of payload bytes not yet written.
    std::size_t pending_bytes() const;

    /// Writes the next window if a transfer is pending. Called when the stream has been
    /// flushed; exposed for testing.
    void on_output_flushed();

private:
    struct Transfer {
        ClipboardID id = 0;
        std::uint32_t sequence = 0;
        std::string start_payload;
        // Exactly one of these holds the data: marshalled or encoded contents in memory, or a
        // file on disk that is read while it is sent.
        MarshalledClipboardPtr marshalled;
        std::shared_ptr<const std::string> encoded;
        fs::path file;
        std::uint64_t file_offset = 0;
        std::shared_ptr<std::ifstream> file_stream;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        bool started = false;
        bool failed = false;

        bool from_file() const { return !file.empty(); }
        const std::string& bytes() const;
        // reads the next chunk of at most \p max bytes into \p out; false if the file failed
        bool read_chunk(std::size_t max, std::string& out);
    };

    void enqueue(Transfer transfer);
    void pump();
    void arm_fallback_timer();
    void cancel_fallback_timer();
    void on_fallback_timer();

    IEventQueue* events_;
    const EventTarget* stream_target_;
    ChunkWriter writer_;
    std::size_t window_size_;

    std::deque<Transfer> transfers_;
    EventQueueTimer* fallback_timer_ = nullptr;
    double fallback_interval_ = 0;
};

} // namespace inputleap
