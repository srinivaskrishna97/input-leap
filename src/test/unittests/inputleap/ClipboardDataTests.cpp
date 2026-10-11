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

#include "inputleap/ClipboardData.h"
#include "inputleap/Clipboard.h"

#include <gtest/gtest.h>

namespace inputleap {

namespace {

std::uint64_t hash_of(const std::string& s)
{
    return MarshalledClipboard::compute_hash(s.data(), s.size());
}

} // namespace

// The hash identifies clipboard contents across machines, so it must not change between
// versions or platforms. These values were computed with an independent implementation.
TEST(ClipboardDataTests, hash_matches_reference_values)
{
    EXPECT_EQ(hash_of(""), 0xf565b1a477745650ULL);
    EXPECT_EQ(hash_of("a"), 0x8d22ce194c042932ULL);
    EXPECT_EQ(hash_of("InputLeap"), 0x87574fcdc754ea6fULL);
    EXPECT_EQ(hash_of(std::string(4, '\0')), 0xe3cd39f0e711d41bULL);

    std::string bytes;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int i = 0; i < 256; ++i) {
            bytes.push_back(static_cast<char>(i));
        }
    }
    bytes += "xyz";
    EXPECT_EQ(hash_of(bytes), 0xb0b267e6789ed724ULL);
}

TEST(ClipboardDataTests, hash_depends_on_every_byte)
{
    std::string data(1000, 'x');
    auto base = hash_of(data);
    for (std::size_t i : {std::size_t{0}, std::size_t{7}, std::size_t{8}, std::size_t{999}}) {
        std::string changed = data;
        changed[i] = 'y';
        EXPECT_NE(hash_of(changed), base) << "position " << i;
    }
    EXPECT_NE(hash_of(data + std::string(1, '\0')), base);
}

TEST(ClipboardDataTests, same_contents)
{
    auto a = make_marshalled_clipboard("contents");
    auto b = make_marshalled_clipboard("contents");
    auto c = make_marshalled_clipboard("other");

    EXPECT_TRUE(same_clipboard_contents(a, a));
    EXPECT_TRUE(same_clipboard_contents(a, b));
    EXPECT_FALSE(same_clipboard_contents(a, c));
    EXPECT_FALSE(same_clipboard_contents(a, nullptr));
    EXPECT_FALSE(same_clipboard_contents(nullptr, a));
    EXPECT_TRUE(same_clipboard_contents(nullptr, nullptr));
}

TEST(ClipboardDataTests, marshall_matches_clipboard_marshall)
{
    Clipboard clipboard;
    clipboard.open(0);
    clipboard.clear();
    clipboard.add(IClipboard::kText, "hello");
    clipboard.add(IClipboard::kHTML, "<b>hello</b>");
    clipboard.close();

    auto data = marshall_clipboard(clipboard);
    EXPECT_EQ(data->data(), clipboard.marshall());
    EXPECT_EQ(data->size(), clipboard.marshall().size());

    Clipboard copy;
    copy.unmarshall(data->data(), 0);
    copy.open(0);
    EXPECT_EQ(copy.get(IClipboard::kText), "hello");
    EXPECT_EQ(copy.get(IClipboard::kHTML), "<b>hello</b>");
    copy.close();
}

TEST(ClipboardDataTests, empty_clipboard_has_no_formats)
{
    const auto& empty = empty_marshalled_clipboard();
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->data(), std::string(4, '\0'));
    EXPECT_EQ(empty.get(), empty_marshalled_clipboard().get());
}

} // namespace inputleap
