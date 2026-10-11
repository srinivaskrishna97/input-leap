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

#include "inputleap/ClipboardStreamer.h"

#include "inputleap/protocol_types.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <algorithm>

namespace inputleap {

namespace {

// The fallback timer starts short and backs off while the stream does not report that its
// output has been flushed, so that a stalled connection does not accumulate unbounded amounts
// of clipboard data in its output buffer.
constexpr double kStreamerMinFallbackInterval = 0.02;
constexpr double kStreamerMaxFallbackInterval = 1.0;

} // namespace

const std::string& ClipboardStreamer::Transfer::bytes() const
{
    if (marshalled) {
        return marshalled->data();
    }
    return *encoded;
}

ClipboardStreamer::ClipboardStreamer(IEventQueue* events, const EventTarget* stream_target,
                                     ChunkWriter writer, std::size_t window_size) :
    events_{events},
    stream_target_{stream_target},
    writer_{std::move(writer)},
    window_size_{std::max<std::size_t>(window_size, 1)}
{
    events_->add_handler(EventType::STREAM_OUTPUT_FLUSHED, stream_target_,
                         [this](const auto&) { on_output_flushed(); });
    events_->add_handler(EventType::TIMER, this,
                         [this](const auto&) { on_fallback_timer(); });
}

ClipboardStreamer::~ClipboardStreamer()
{
    cancel_fallback_timer();
    events_->remove_handler(EventType::STREAM_OUTPUT_FLUSHED, stream_target_);
    events_->remove_handler(EventType::TIMER, this);
}

void ClipboardStreamer::send(ClipboardID id, std::uint32_t sequence, MarshalledClipboardPtr data)
{
    if (!data) {
        data = empty_marshalled_clipboard();
    }
    Transfer transfer;
    transfer.id = id;
    transfer.sequence = sequence;
    transfer.start_payload = std::to_string(data->size());
    transfer.marshalled = std::move(data);
    enqueue(std::move(transfer));
}

void ClipboardStreamer::send_encoded(ClipboardID id, std::uint32_t sequence,
                                     std::string start_payload,
                                     std::shared_ptr<const std::string> payload)
{
    if (!payload) {
        payload = std::make_shared<const std::string>();
    }
    Transfer transfer;
    transfer.id = id;
    transfer.sequence = sequence;
    transfer.start_payload = std::move(start_payload);
    transfer.encoded = std::move(payload);
    enqueue(std::move(transfer));
}

void ClipboardStreamer::cancel()
{
    transfers_.clear();
    cancel_fallback_timer();
    fallback_interval_ = 0;
}

std::size_t ClipboardStreamer::pending_bytes() const
{
    std::size_t result = 0;
    for (const auto& transfer : transfers_) {
        result += transfer.bytes().size() - transfer.offset;
    }
    return result;
}

void ClipboardStreamer::on_output_flushed()
{
    if (transfers_.empty()) {
        return;
    }
    fallback_interval_ = 0;
    pump();
}

void ClipboardStreamer::enqueue(Transfer transfer)
{
    bool was_idle = transfers_.empty();

    // Newer contents of a clipboard supersede older ones, including a partially sent transfer:
    // the receiver drops the partial data once it sees the start chunk of the new transfer.
    transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(),
                                    [&](const Transfer& t) { return t.id == transfer.id; }),
                     transfers_.end());

    LOG_DEBUG("queued clipboard %d for sending, size=%zu", transfer.id, transfer.bytes().size());
    transfers_.push_back(std::move(transfer));

    if (was_idle) {
        fallback_interval_ = 0;
        pump();
    }
}

void ClipboardStreamer::pump()
{
    cancel_fallback_timer();

    std::size_t budget = window_size_;
    while (!transfers_.empty()) {
        Transfer& transfer = transfers_.front();
        const std::string& bytes = transfer.bytes();

        if (!transfer.started) {
            ClipboardChunk chunk;
            chunk.id_ = transfer.id;
            chunk.sequence_ = transfer.sequence;
            chunk.mark_ = kDataStart;
            chunk.data_ = transfer.start_payload;
            writer_(chunk);
            transfer.started = true;
        }

        if (transfer.offset < bytes.size()) {
            if (budget == 0) {
                break;
            }
            std::size_t size = std::min(kChunkSize, bytes.size() - transfer.offset);

            ClipboardChunk chunk;
            chunk.id_ = transfer.id;
            chunk.sequence_ = transfer.sequence;
            chunk.mark_ = kDataChunk;
            chunk.data_.assign(bytes, transfer.offset, size);
            writer_(chunk);

            transfer.offset += size;
            budget -= std::min(budget, size);
            continue;
        }

        writer_(ClipboardChunk::end(transfer.id, transfer.sequence));
        LOG_DEBUG("sent clipboard %d, size=%zu", transfer.id, bytes.size());
        transfers_.pop_front();
    }

    if (transfers_.empty()) {
        fallback_interval_ = 0;
    } else {
        arm_fallback_timer();
    }
}

void ClipboardStreamer::arm_fallback_timer()
{
    if (fallback_interval_ <= 0) {
        fallback_interval_ = kStreamerMinFallbackInterval;
    } else {
        fallback_interval_ = std::min(fallback_interval_ * 2, kStreamerMaxFallbackInterval);
    }
    fallback_timer_ = events_->newOneShotTimer(fallback_interval_, this);
}

void ClipboardStreamer::cancel_fallback_timer()
{
    if (fallback_timer_ != nullptr) {
        events_->deleteTimer(fallback_timer_);
        fallback_timer_ = nullptr;
    }
}

void ClipboardStreamer::on_fallback_timer()
{
    cancel_fallback_timer();
    if (!transfers_.empty()) {
        pump();
    }
}

} // namespace inputleap
