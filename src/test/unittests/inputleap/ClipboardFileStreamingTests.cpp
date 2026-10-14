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

#include "inputleap/ClipboardFiles.h"
#include "inputleap/ClipboardStreamer.h"
#include "inputleap/ClipboardTransfer.h"
#include "inputleap/protocol_types.h"
#include "io/filesystem.h"
#include "test/mock/inputleap/MockEventQueue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <random>
#include <vector>

namespace inputleap {

using ::testing::NiceMock;

namespace {

fs::path temporary_file(const std::string& name, const std::string& contents)
{
    fs::path path = fs::temp_directory_path() / ("input-leap-test-" + name);
    std::ofstream stream;
    open_utf8_path(stream, path, std::ios::out | std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    return path;
}

} // namespace

TEST(ClipboardFileStreamingTests, a_listing_has_sizes_but_no_contents)
{
    ClipboardFileList::Builder builder;
    ASSERT_TRUE(builder.add_directory("photos"));
    ASSERT_TRUE(builder.add_file_size("photos/a.jpg", 1234567));
    ASSERT_TRUE(builder.add_file_size("notes.txt", 0));
    std::string listing = builder.finish_listing();

    ClipboardFileList list;
    ASSERT_TRUE(list.parse(listing));
    EXPECT_FALSE(list.has_contents());
    EXPECT_EQ(list.entries().size(), 3u);
    EXPECT_EQ(list.file_count(), 2u);
    EXPECT_EQ(list.total_size(), 1234567u);
    EXPECT_EQ(list.entries()[1].size, 1234567u);
    EXPECT_EQ(list.entries()[1].offset, 0u);
    const char* begin = nullptr;
    std::size_t size = 0;
    EXPECT_FALSE(list.contents(listing, list.entries()[1], begin, size));

    // the contents cannot be written from a listing
    fs::path directory = fs::temp_directory_path() / "input-leap-test-listing";
    EXPECT_TRUE(write_clipboard_files(listing, directory).empty());

    // a listing is rejected if it carries data after the entries
    EXPECT_FALSE(list.parse(listing + "junk"));
}

TEST(ClipboardFileStreamingTests, a_listing_is_built_from_files_on_disk)
{
    fs::path a = temporary_file("listing-a.bin", std::string(5000, 'a'));
    fs::path b = temporary_file("listing-b.bin", "");
    std::string listing;
    std::vector<fs::path> paths;
    ASSERT_TRUE(read_clipboard_file_listing({a, b}, listing, paths));

    ClipboardFileList list;
    ASSERT_TRUE(list.parse(listing));
    EXPECT_FALSE(list.has_contents());
    ASSERT_EQ(list.entries().size(), 2u);
    EXPECT_EQ(list.entries()[0].path, a.filename().u8string());
    EXPECT_EQ(list.entries()[0].size, 5000u);
    EXPECT_EQ(list.entries()[1].size, 0u);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], a);
    EXPECT_EQ(paths[1], b);

    // the old form with contents still works for the same files
    std::string full;
    ASSERT_TRUE(read_clipboard_files({a, b}, 1 << 20, full));
    ClipboardFileList full_list;
    ASSERT_TRUE(full_list.parse(full));
    EXPECT_TRUE(full_list.has_contents());
    EXPECT_EQ(full_list.total_size(), 5000u);

    fs::remove(a);
    fs::remove(b);
}

TEST(ClipboardFileStreamingTests, file_piece_headers_round_trip_and_reject_compression)
{
    ClipboardTransferHeader header = make_clipboard_file_header(0x0123456789abcdefull, 7, 1u << 20, 4096);
    std::string text = header.format();
    EXPECT_EQ(text.substr(0, 2), "2 ");

    ClipboardTransferHeader parsed;
    ASSERT_TRUE(ClipboardTransferHeader::parse(text, parsed));
    EXPECT_EQ(parsed.kind, ClipboardTransferHeader::Kind::File);
    EXPECT_EQ(parsed.hash, 0x0123456789abcdefull);
    EXPECT_EQ(parsed.file_index, 7u);
    EXPECT_EQ(parsed.file_offset, 1u << 20);
    EXPECT_EQ(parsed.size, 4096u);
    EXPECT_EQ(parsed.encoded_size, 4096u);

    // contents headers are unchanged
    ClipboardTransferHeader contents;
    contents.size = contents.encoded_size = 10;
    contents.hash = 1;
    ASSERT_TRUE(ClipboardTransferHeader::parse(contents.format(), parsed));
    EXPECT_EQ(parsed.kind, ClipboardTransferHeader::Kind::Contents);

    // pieces of files are never compressed, and need their index and offset
    EXPECT_FALSE(ClipboardTransferHeader::parse("2 1 100 200 0123456789abcdef 1 0", parsed));
    EXPECT_FALSE(ClipboardTransferHeader::parse("2 0 100 100 0123456789abcdef", parsed));
    EXPECT_FALSE(ClipboardTransferHeader::parse("2 0 100 200 0123456789abcdef 1 0", parsed));

    ClipboardTransferHeader unavailable = make_unavailable_clipboard_file_header(5, 3);
    ASSERT_TRUE(ClipboardTransferHeader::parse(unavailable.format(), parsed));
    EXPECT_EQ(parsed.kind, ClipboardTransferHeader::Kind::File);
    EXPECT_EQ(parsed.encoding, ClipboardEncoding::Unavailable);
    EXPECT_EQ(parsed.file_index, 3u);
}

TEST(ClipboardFileStreamingTests, the_receiver_hands_file_pieces_to_the_sink)
{
    ClipboardTransferReceiver receiver(1 << 20);
    std::map<std::uint64_t, std::string> written;
    std::uint32_t index = 99;
    receiver.set_file_sink([&](const ClipboardTransferHeader& header, std::uint64_t offset,
                               const std::string& chunk) {
        index = header.file_index;
        written[offset] = chunk;
        return true;
    });

    auto header = make_clipboard_file_header(42, 2, 1000, 10);
    EXPECT_EQ(receiver.add(kDataStart, header.format()), ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.add(kDataChunk, "abcd"), ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.received(), 4u);
    EXPECT_EQ(receiver.expected(), 10u);
    EXPECT_EQ(receiver.add(kDataChunk, "efghij"), ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.add(kDataEnd, ""), ClipboardTransferReceiver::Status::Complete);
    EXPECT_EQ(receiver.header().kind, ClipboardTransferHeader::Kind::File);
    EXPECT_EQ(index, 2u);
    EXPECT_EQ(written[1000], "abcd");
    EXPECT_EQ(written[1004], "efghij");

    // a piece that ends short fails, as does one that is too long
    EXPECT_EQ(receiver.add(kDataStart, header.format()), ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.add(kDataChunk, "abcd"), ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.add(kDataEnd, ""), ClipboardTransferReceiver::Status::Failed);
    EXPECT_EQ(receiver.add(kDataStart, header.format()), ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.add(kDataChunk, std::string(11, 'x')), ClipboardTransferReceiver::Status::Failed);

    // an unavailable file
    auto unavailable = make_unavailable_clipboard_file_header(42, 2);
    EXPECT_EQ(receiver.add(kDataStart, unavailable.format()), ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.add(kDataEnd, ""), ClipboardTransferReceiver::Status::Unavailable);

    // without a sink, pieces are refused
    ClipboardTransferReceiver without_sink(1 << 20);
    EXPECT_EQ(without_sink.add(kDataStart, header.format()), ClipboardTransferReceiver::Status::Failed);
}

TEST(ClipboardFileStreamingTests, the_streamer_sends_a_file_from_disk_in_chunks)
{
    std::string contents(200 * 1024 + 123, '\0');
    std::mt19937 random(7);
    for (char& c : contents) {
        c = static_cast<char>(random());
    }
    fs::path file = temporary_file("streamer.bin", contents);

    NiceMock<MockEventQueue> events;
    EventTarget stream_target;
    std::vector<ClipboardChunk> chunks;
    std::vector<bool> encoded;
    ClipboardStreamer streamer(&events, &stream_target,
                               [&](const ClipboardChunk& chunk, bool is_encoded) {
                                   chunks.push_back(chunk);
                                   encoded.push_back(is_encoded);
                               });

    // the middle of the file, from an offset
    const std::uint64_t offset = 1000;
    const std::uint64_t size = contents.size() - 1500;
    auto header = make_clipboard_file_header(9, 0, offset, size);
    streamer.send_file(0, 0, header.format(), file, offset, size);
    for (int i = 0; i < 10000 && !streamer.is_idle(); ++i) {
        streamer.on_output_flushed();
    }
    ASSERT_TRUE(streamer.is_idle());

    ASSERT_GE(chunks.size(), 3u);
    EXPECT_EQ(chunks.front().mark_, kDataStart);
    EXPECT_EQ(chunks.front().data_, header.format());
    EXPECT_EQ(chunks.back().mark_, kDataEnd);
    std::string sent;
    for (const auto& chunk : chunks) {
        if (chunk.mark_ == kDataChunk) {
            EXPECT_LE(chunk.data_.size(), kClipboardChunkSize);
            sent += chunk.data_;
        }
    }
    EXPECT_EQ(sent, contents.substr(offset, size));
    for (bool is_encoded : encoded) {
        EXPECT_TRUE(is_encoded);
    }

    // a file that cannot be read ends the transfer short
    chunks.clear();
    streamer.send_file(0, 0, header.format(), file / "missing", 0, 100);
    for (int i = 0; i < 100 && !streamer.is_idle(); ++i) {
        streamer.on_output_flushed();
    }
    ASSERT_TRUE(streamer.is_idle());
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].mark_, kDataStart);
    EXPECT_EQ(chunks[1].mark_, kDataEnd);

    fs::remove(file);
}

} // namespace inputleap
