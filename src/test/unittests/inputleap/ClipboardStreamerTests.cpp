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
#include "test/mock/inputleap/MockEventQueue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

namespace inputleap {

using ::testing::NiceMock;

class ClipboardStreamerTest : public ::testing::Test {
protected:
    ClipboardStreamerTest() :
        streamer_{&events_, &stream_target_,
                  [this](const ClipboardChunk& chunk, bool encoded) {
                      chunks_.push_back(chunk);
                      encoded_.push_back(encoded);
                  }}
    {
    }

    // Reassembles the chunks the way the receiver does: a start chunk discards partial data.
    // Returns the completed transfers.
    std::vector<std::pair<ClipboardID, std::string>> received()
    {
        std::vector<std::pair<ClipboardID, std::string>> result;
        std::string data;
        std::size_t expected = 0;
        for (const auto& chunk : chunks_) {
            if (chunk.mark_ == kDataStart) {
                data.clear();
                expected = std::stoul(chunk.data_);
            } else if (chunk.mark_ == kDataChunk) {
                data += chunk.data_;
            } else if (chunk.mark_ == kDataEnd) {
                EXPECT_EQ(data.size(), expected);
                result.emplace_back(chunk.id_, data);
            }
        }
        return result;
    }

    void flush_until_idle()
    {
        for (int i = 0; i < 10000 && !streamer_.is_idle(); ++i) {
            streamer_.on_output_flushed();
        }
        ASSERT_TRUE(streamer_.is_idle());
    }

    std::size_t data_bytes_written() const
    {
        std::size_t result = 0;
        for (const auto& chunk : chunks_) {
            if (chunk.mark_ == kDataChunk) {
                result += chunk.data_.size();
            }
        }
        return result;
    }

    static MarshalledClipboardPtr data_of_size(std::size_t size, char fill)
    {
        std::string data(size, fill);
        for (std::size_t i = 0; i < size; i += 997) {
            data[i] = static_cast<char>(i);
        }
        return make_marshalled_clipboard(std::move(data));
    }

    NiceMock<MockEventQueue> events_;
    EventTarget stream_target_;
    std::vector<ClipboardChunk> chunks_;
    std::vector<bool> encoded_;
    ClipboardStreamer streamer_;
};

TEST_F(ClipboardStreamerTest, writes_only_first_window_immediately)
{
    auto data = data_of_size(1000 * 1000, 'a');
    streamer_.send(0, 7, data);

    ASSERT_FALSE(chunks_.empty());
    EXPECT_EQ(chunks_[0].mark_, kDataStart);
    EXPECT_EQ(chunks_[0].data_, "1000000");
    EXPECT_EQ(chunks_[0].sequence_, 7u);
    EXPECT_EQ(data_bytes_written(), ClipboardStreamer::kDefaultWindowSize);
    EXPECT_FALSE(streamer_.is_idle());
    EXPECT_EQ(streamer_.pending_bytes(), data->size() - ClipboardStreamer::kDefaultWindowSize);
}

TEST_F(ClipboardStreamerTest, each_flush_writes_one_more_window)
{
    auto data = data_of_size(1000 * 1000, 'a');
    streamer_.send(0, 0, data);
    streamer_.on_output_flushed();
    EXPECT_EQ(data_bytes_written(), 2 * ClipboardStreamer::kDefaultWindowSize);
    for (const auto& chunk : chunks_) {
        if (chunk.mark_ == kDataChunk) {
            EXPECT_LE(chunk.data_.size(), ClipboardStreamer::kChunkSize);
        }
    }
}

TEST_F(ClipboardStreamerTest, sends_complete_data)
{
    auto data = data_of_size(300 * 1000 + 17, 'b');
    streamer_.send(1, 0, data);
    flush_until_idle();

    auto transfers = received();
    ASSERT_EQ(transfers.size(), 1u);
    EXPECT_EQ(transfers[0].first, 1);
    EXPECT_EQ(transfers[0].second, data->data());
    EXPECT_EQ(chunks_.back().mark_, kDataEnd);
}

TEST_F(ClipboardStreamerTest, small_data_is_sent_at_once)
{
    streamer_.send(0, 0, make_marshalled_clipboard("short"));
    EXPECT_TRUE(streamer_.is_idle());
    auto transfers = received();
    ASSERT_EQ(transfers.size(), 1u);
    EXPECT_EQ(transfers[0].second, "short");
}

TEST_F(ClipboardStreamerTest, empty_data_has_no_data_chunks)
{
    streamer_.send(0, 0, make_marshalled_clipboard(""));
    ASSERT_EQ(chunks_.size(), 2u);
    EXPECT_EQ(chunks_[0].mark_, kDataStart);
    EXPECT_EQ(chunks_[0].data_, "0");
    EXPECT_EQ(chunks_[1].mark_, kDataEnd);
}

TEST_F(ClipboardStreamerTest, newer_contents_replace_partial_transfer)
{
    streamer_.send(0, 0, data_of_size(1000 * 1000, 'a'));
    auto newer = data_of_size(100 * 1000, 'n');
    streamer_.send(0, 0, newer);
    flush_until_idle();

    auto transfers = received();
    ASSERT_EQ(transfers.size(), 1u);
    EXPECT_EQ(transfers[0].second, newer->data());
}

TEST_F(ClipboardStreamerTest, different_clipboards_are_sent_one_after_another)
{
    auto first = data_of_size(200 * 1000, 'f');
    auto second = data_of_size(150 * 1000, 's');
    streamer_.send(0, 0, first);
    streamer_.send(1, 0, second);
    flush_until_idle();

    auto transfers = received();
    ASSERT_EQ(transfers.size(), 2u);
    EXPECT_EQ(transfers[0].first, 0);
    EXPECT_EQ(transfers[0].second, first->data());
    EXPECT_EQ(transfers[1].first, 1);
    EXPECT_EQ(transfers[1].second, second->data());

    // chunks of the two transfers are not interleaved
    bool seen_second_start = false;
    for (const auto& chunk : chunks_) {
        if (chunk.id_ == 1 && chunk.mark_ == kDataStart) {
            seen_second_start = true;
        }
        if (seen_second_start) {
            EXPECT_EQ(chunk.id_, 1);
        }
    }
}

TEST_F(ClipboardStreamerTest, cancel_stops_sending)
{
    streamer_.send(0, 0, data_of_size(1000 * 1000, 'a'));
    auto written = chunks_.size();
    streamer_.cancel();
    EXPECT_TRUE(streamer_.is_idle());
    streamer_.on_output_flushed();
    EXPECT_EQ(chunks_.size(), written);
}

TEST_F(ClipboardStreamerTest, encoded_transfer_uses_given_start_payload)
{
    auto payload = std::make_shared<const std::string>(100 * 1000, 'e');
    streamer_.send_encoded(0, 3, "custom header", payload);
    flush_until_idle();

    ASSERT_FALSE(chunks_.empty());
    EXPECT_EQ(chunks_[0].mark_, kDataStart);
    EXPECT_EQ(chunks_[0].data_, "custom header");
    std::string data;
    for (const auto& chunk : chunks_) {
        if (chunk.mark_ == kDataChunk) {
            data += chunk.data_;
        }
    }
    EXPECT_EQ(data, *payload);
    for (bool encoded : encoded_) {
        EXPECT_TRUE(encoded);
    }
}

TEST_F(ClipboardStreamerTest, plain_transfers_are_not_encoded)
{
    streamer_.send(0, 0, data_of_size(100 * 1000, 'p'));
    flush_until_idle();
    for (bool encoded : encoded_) {
        EXPECT_FALSE(encoded);
    }
}

TEST_F(ClipboardStreamerTest, cancel_one_clipboard)
{
    auto first = data_of_size(200 * 1000, 'f');
    auto second = data_of_size(150 * 1000, 's');
    streamer_.send(0, 0, first);
    streamer_.send(1, 0, second);
    streamer_.cancel(0);
    flush_until_idle();

    // the partially sent first transfer is never completed, the second one is
    auto transfers = received();
    ASSERT_EQ(transfers.size(), 1u);
    EXPECT_EQ(transfers[0].first, 1);
    EXPECT_EQ(transfers[0].second, second->data());
}

} // namespace inputleap
