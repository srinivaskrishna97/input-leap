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

#include "inputleap/ClipboardHelperMessages.h"

#include <gtest/gtest.h>

namespace inputleap {

TEST(ClipboardHelperMessagesTests, capture_round_trip)
{
    std::string contents("\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02hi", 14);
    std::string encoded = encode_clipboard_capture(0x01020304u, contents);
    ASSERT_EQ(encoded.size(), 4u + contents.size());
    EXPECT_EQ(static_cast<unsigned char>(encoded[0]), 0x04);
    EXPECT_EQ(static_cast<unsigned char>(encoded[3]), 0x01);

    std::uint32_t sequence = 0;
    std::string decoded;
    ASSERT_TRUE(decode_clipboard_capture(encoded.data(), encoded.size(), sequence, decoded));
    EXPECT_EQ(sequence, 0x01020304u);
    EXPECT_EQ(decoded, contents);
}

TEST(ClipboardHelperMessagesTests, capture_may_be_empty_but_not_truncated)
{
    std::string encoded = encode_clipboard_capture(7, "");
    std::uint32_t sequence = 0;
    std::string decoded = "stale";
    ASSERT_TRUE(decode_clipboard_capture(encoded.data(), encoded.size(), sequence, decoded));
    EXPECT_EQ(sequence, 7u);
    EXPECT_TRUE(decoded.empty());

    EXPECT_FALSE(decode_clipboard_capture(encoded.data(), 3, sequence, decoded));
    EXPECT_FALSE(decode_clipboard_capture(nullptr, 4, sequence, decoded));
}

TEST(ClipboardHelperMessagesTests, hello_round_trip)
{
    std::string encoded = encode_clipboard_helper_hello(0x0000123456789abcull, 4242);
    ASSERT_EQ(encoded.size(), 12u);
    std::uint64_t window = 0;
    std::uint32_t pid = 0;
    ASSERT_TRUE(decode_clipboard_helper_hello(encoded.data(), encoded.size(), window, pid));
    EXPECT_EQ(window, 0x0000123456789abcull);
    EXPECT_EQ(pid, 4242u);
    EXPECT_FALSE(decode_clipboard_helper_hello(encoded.data(), 11, window, pid));
    EXPECT_FALSE(decode_clipboard_helper_hello(encoded.data(), 13, window, pid));
}

} // namespace inputleap
