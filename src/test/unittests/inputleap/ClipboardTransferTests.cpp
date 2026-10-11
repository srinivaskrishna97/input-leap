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

#include "inputleap/ClipboardCodec.h"
#include "inputleap/ClipboardTransfer.h"
#include "inputleap/Clipboard.h"
#include "inputleap/protocol_types.h"

#include <gtest/gtest.h>

#include <random>

namespace inputleap {

namespace {

// Something that looks like a screenshot: 32-bit pixels with large uniform areas.
std::string make_screenshot_like(std::size_t width, std::size_t height)
{
    std::string pixels;
    pixels.reserve(width * height * 4);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            bool text = (y % 20 < 3) && (x % 7 == 0);
            unsigned char shade = text ? 0x20 : (y < height / 10 ? 0x40 : 0xf0);
            pixels.push_back(static_cast<char>(shade));
            pixels.push_back(static_cast<char>(shade));
            pixels.push_back(static_cast<char>(shade));
            pixels.push_back(static_cast<char>(0xff));
        }
    }
    return pixels;
}

std::string random_bytes(std::size_t size, unsigned seed)
{
    std::mt19937 rng(seed);
    std::string data(size, '\0');
    for (auto& c : data) {
        c = static_cast<char>(rng() & 0xff);
    }
    return data;
}

std::string roundtrip(const std::string& data)
{
    std::string encoded = rle32_encode(data);
    EXPECT_LE(encoded.size(), rle32_max_encoded_size(data.size()));
    std::string decoded;
    EXPECT_TRUE(rle32_decode(encoded, data.size(), decoded));
    return decoded;
}

ClipboardTransferReceiver::Status receive_all(ClipboardTransferReceiver& receiver,
                                              const ClipboardTransferPayload& payload,
                                              std::size_t chunk_size = 1000)
{
    auto status = receiver.add(kDataStart, payload.header.format());
    if (status != ClipboardTransferReceiver::Status::InProgress) {
        return status;
    }
    const std::string& bytes = *payload.bytes;
    for (std::size_t pos = 0; pos < bytes.size(); pos += chunk_size) {
        status = receiver.add(kDataChunk, bytes.substr(pos, chunk_size));
        if (status != ClipboardTransferReceiver::Status::InProgress) {
            return status;
        }
    }
    return receiver.add(kDataEnd, "");
}

} // namespace

TEST(ClipboardCodecTests, roundtrip_edge_cases)
{
    for (std::size_t size = 0; size < 40; ++size) {
        std::string same(size, 'a');
        EXPECT_EQ(roundtrip(same), same) << size;
        std::string random = random_bytes(size, static_cast<unsigned>(size));
        EXPECT_EQ(roundtrip(random), random) << size;
    }
}

TEST(ClipboardCodecTests, roundtrip_long_runs_and_literals)
{
    std::string data;
    data += std::string(4 * 1000, 'x');          // runs longer than one packet
    data += random_bytes(4 * 300 + 3, 7);         // literals longer than one packet
    data += std::string(4 * 2, 'y');              // too short for a run
    data += std::string(4 * 3, 'z');              // shortest run
    data += random_bytes(1, 9);
    EXPECT_EQ(roundtrip(data), data);
}

TEST(ClipboardCodecTests, compresses_screenshots)
{
    std::string pixels = make_screenshot_like(1920, 1080);
    std::string encoded = rle32_encode(pixels);
    EXPECT_LT(encoded.size(), pixels.size() / 4);
    EXPECT_EQ(roundtrip(pixels), pixels);
}

TEST(ClipboardCodecTests, random_data_barely_grows)
{
    std::string data = random_bytes(100000, 3);
    EXPECT_LE(rle32_encode(data).size(), rle32_max_encoded_size(data.size()));
    EXPECT_LT(rle32_max_encoded_size(data.size()), data.size() + data.size() / 400);
}

TEST(ClipboardCodecTests, rejects_malformed_data)
{
    std::string decoded;
    std::string encoded = rle32_encode(std::string(4 * 100, 'q') + "tail");

    // wrong size
    EXPECT_FALSE(rle32_decode(encoded, 4 * 100 + 3, decoded));
    EXPECT_FALSE(rle32_decode(encoded, 4 * 100 + 5, decoded));
    EXPECT_FALSE(rle32_decode(encoded, 4 * 1000, decoded));

    // truncated
    EXPECT_FALSE(rle32_decode(encoded.substr(0, encoded.size() - 1), 4 * 100 + 4, decoded));
    EXPECT_FALSE(rle32_decode(encoded.substr(0, 2), 4 * 100 + 4, decoded));

    // literal packet that claims more data than present
    EXPECT_FALSE(rle32_decode(std::string(1, '\x05') + "abcd", 24, decoded));

    // run that overflows the output
    EXPECT_FALSE(rle32_decode(std::string(1, '\xff') + "abcd", 8, decoded));

    // trailing garbage
    EXPECT_FALSE(rle32_decode(encoded + "x", 4 * 100 + 4, decoded));

    EXPECT_TRUE(rle32_decode(encoded, 4 * 100 + 4, decoded));
}

TEST(ClipboardTransferTests, header_roundtrip)
{
    ClipboardTransferHeader header;
    header.encoding = ClipboardEncoding::Rle32;
    header.encoded_size = 12345;
    header.size = 25000000;
    header.hash = 0x0123456789abcdefULL;

    std::string text = header.format();
    EXPECT_EQ(text, "1 1 12345 25000000 0123456789abcdef");

    ClipboardTransferHeader parsed;
    ASSERT_TRUE(ClipboardTransferHeader::parse(text, parsed));
    EXPECT_EQ(parsed.encoding, header.encoding);
    EXPECT_EQ(parsed.encoded_size, header.encoded_size);
    EXPECT_EQ(parsed.size, header.size);
    EXPECT_EQ(parsed.hash, header.hash);
}

TEST(ClipboardTransferTests, header_rejects_garbage)
{
    ClipboardTransferHeader header;
    EXPECT_FALSE(ClipboardTransferHeader::parse("", header));
    EXPECT_FALSE(ClipboardTransferHeader::parse("12345", header));
    EXPECT_FALSE(ClipboardTransferHeader::parse("2 0 1 1 0123456789abcdef", header));
    EXPECT_FALSE(ClipboardTransferHeader::parse("1 7 1 1 0123456789abcdef", header));
    EXPECT_FALSE(ClipboardTransferHeader::parse("1 0 1 1 0123456789ABCDEF", header));
    EXPECT_FALSE(ClipboardTransferHeader::parse("1 0 1 1 0123", header));
}

TEST(ClipboardTransferTests, small_contents_are_not_compressed)
{
    auto data = make_marshalled_clipboard("small");
    auto payload = make_clipboard_transfer(data, true);
    EXPECT_EQ(payload.header.encoding, ClipboardEncoding::None);
    EXPECT_EQ(*payload.bytes, "small");
    // the payload shares the data instead of copying it
    EXPECT_EQ(payload.bytes.get(), &data->data());
}

TEST(ClipboardTransferTests, transfers_compressed_contents)
{
    auto data = make_marshalled_clipboard(make_screenshot_like(800, 600));
    auto payload = make_clipboard_transfer(data, true);
    EXPECT_EQ(payload.header.encoding, ClipboardEncoding::Rle32);
    EXPECT_LT(payload.bytes->size(), data->size() / 4);

    ClipboardTransferReceiver receiver(100 * 1000 * 1000);
    ASSERT_EQ(receive_all(receiver, payload), ClipboardTransferReceiver::Status::Complete);
    auto received = receiver.take();
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->data(), data->data());
    EXPECT_EQ(received->hash(), data->hash());
    // the encoded form is kept for relaying
    EXPECT_EQ(*received->rle32(), *payload.bytes);
}

TEST(ClipboardTransferTests, transfers_uncompressed_contents)
{
    auto data = make_marshalled_clipboard(random_bytes(50000, 11));
    auto payload = make_clipboard_transfer(data, true);
    EXPECT_EQ(payload.header.encoding, ClipboardEncoding::None);

    ClipboardTransferReceiver receiver(100 * 1000 * 1000);
    ASSERT_EQ(receive_all(receiver, payload), ClipboardTransferReceiver::Status::Complete);
    EXPECT_EQ(receiver.take()->data(), data->data());
}

TEST(ClipboardTransferTests, compression_can_be_disabled)
{
    auto data = make_marshalled_clipboard(make_screenshot_like(200, 200));
    auto payload = make_clipboard_transfer(data, false);
    EXPECT_EQ(payload.header.encoding, ClipboardEncoding::None);
    EXPECT_EQ(payload.bytes->size(), data->size());
}

TEST(ClipboardTransferTests, unavailable)
{
    auto payload = make_unavailable_clipboard_transfer(42);
    ClipboardTransferReceiver receiver(1000);
    EXPECT_EQ(receive_all(receiver, payload), ClipboardTransferReceiver::Status::Unavailable);
    EXPECT_EQ(receiver.header().hash, 42u);
}

TEST(ClipboardTransferTests, rejects_oversized_contents)
{
    auto data = make_marshalled_clipboard(std::string(5000, 'x'));
    auto payload = make_clipboard_transfer(data, false);
    ClipboardTransferReceiver receiver(4000);
    EXPECT_EQ(receive_all(receiver, payload), ClipboardTransferReceiver::Status::Failed);
}

TEST(ClipboardTransferTests, rejects_corrupted_contents)
{
    auto data = make_marshalled_clipboard(std::string(5000, 'x'));
    auto payload = make_clipboard_transfer(data, false);

    auto corrupted = payload;
    std::string bytes = *payload.bytes;
    bytes[100] = 'y';
    corrupted.bytes = std::make_shared<const std::string>(bytes);

    ClipboardTransferReceiver receiver(100000);
    EXPECT_EQ(receive_all(receiver, corrupted), ClipboardTransferReceiver::Status::Failed);

    // too much data
    auto longer = payload;
    longer.bytes = std::make_shared<const std::string>(*payload.bytes + "extra");
    EXPECT_EQ(receive_all(receiver, longer), ClipboardTransferReceiver::Status::Failed);

    // a new transfer replaces an interrupted one
    EXPECT_EQ(receiver.add(kDataStart, payload.header.format()),
              ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receiver.add(kDataChunk, payload.bytes->substr(0, 10)),
              ClipboardTransferReceiver::Status::InProgress);
    EXPECT_EQ(receive_all(receiver, payload), ClipboardTransferReceiver::Status::Complete);
}

TEST(ClipboardTransferTests, chunks_without_start_fail)
{
    ClipboardTransferReceiver receiver(1000);
    EXPECT_EQ(receiver.add(kDataChunk, "abc"), ClipboardTransferReceiver::Status::Failed);
    EXPECT_EQ(receiver.add(kDataEnd, ""), ClipboardTransferReceiver::Status::Failed);
}

TEST(ClipboardOfferTests, describes_contents)
{
    Clipboard clipboard;
    clipboard.open(0);
    clipboard.clear();
    clipboard.add(IClipboard::kText, "text");
    clipboard.add(IClipboard::kBitmap, std::string(100, 'b'));
    clipboard.close();

    auto data = marshall_clipboard(clipboard);
    auto offer = ClipboardOffer::describe(*data);
    EXPECT_EQ(offer.formats, (1u << IClipboard::kText) | (1u << IClipboard::kBitmap));
    EXPECT_EQ(offer.size, data->size());
    EXPECT_EQ(offer.hash, data->hash());
    EXPECT_TRUE(offer.describes(*data));
    EXPECT_FALSE(offer.describes(*make_marshalled_clipboard("other")));

    EXPECT_EQ(ClipboardOffer::describe(*empty_marshalled_clipboard()).formats, 0u);
}

TEST(ClipboardOfferTests, malformed_data_has_no_formats)
{
    EXPECT_EQ(marshalled_clipboard_formats(""), 0u);
    EXPECT_EQ(marshalled_clipboard_formats(std::string("\0\0\0\1", 4)), 0u);
    // claims 10 bytes of data but has 2
    EXPECT_EQ(marshalled_clipboard_formats(std::string("\0\0\0\1\0\0\0\0\0\0\0\x0a" "ab", 14)),
              0u);
}

} // namespace inputleap
