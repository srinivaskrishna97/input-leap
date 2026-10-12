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

#include "inputleap/ClipboardFiles.h"

#include <gtest/gtest.h>

#include <fstream>
#include <random>

namespace inputleap {

namespace {

std::string make_list()
{
    ClipboardFileList::Builder builder;
    EXPECT_TRUE(builder.add_file("report.pdf", std::string("%PDF-1.4 ...")));
    EXPECT_TRUE(builder.add_directory("photos"));
    EXPECT_TRUE(builder.add_file("photos/a.png", std::string("PNG\0\x01\x02", 6)));
    EXPECT_TRUE(builder.add_directory("photos/empty"));
    EXPECT_TRUE(builder.add_file("photos/b.png", std::string()));
    return builder.finish();
}

fs::path make_temp_directory()
{
    std::random_device device;
    fs::path path = fs::temp_directory_path() /
                    ("inputleap-clipboard-test-" + std::to_string(device()));
    fs::create_directories(path);
    return path;
}

std::string read_file(const fs::path& path)
{
    std::ifstream stream;
    open_utf8_path(stream, path, std::ios::in | std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(ClipboardFilesTests, builderAndParserRoundTrip)
{
    std::string data = make_list();

    ClipboardFileList list;
    ASSERT_TRUE(list.parse(data));
    ASSERT_EQ(list.entries().size(), 5u);
    EXPECT_EQ(list.file_count(), 3u);
    EXPECT_EQ(list.directory_count(), 2u);
    EXPECT_EQ(list.total_size(), 12u + 6u);

    const auto& entries = list.entries();
    EXPECT_EQ(entries[0].path, "report.pdf");
    EXPECT_FALSE(entries[0].is_directory);
    EXPECT_EQ(entries[0].size, 12u);
    EXPECT_EQ(entries[1].path, "photos");
    EXPECT_TRUE(entries[1].is_directory);
    EXPECT_EQ(entries[2].path, "photos/a.png");
    EXPECT_EQ(entries[2].size, 6u);
    EXPECT_EQ(entries[2].offset, 12u);

    const char* begin = nullptr;
    std::size_t size = 0;
    ASSERT_TRUE(list.contents(data, entries[2], begin, size));
    EXPECT_EQ(std::string(begin, size), std::string("PNG\0\x01\x02", 6));
    ASSERT_TRUE(list.contents(data, entries[4], begin, size));
    EXPECT_EQ(size, 0u);
    EXPECT_FALSE(list.contents(data, entries[1], begin, size));
}

TEST(ClipboardFilesTests, builderRejectsBadAndDuplicatePaths)
{
    ClipboardFileList::Builder builder;
    EXPECT_TRUE(builder.add_file("a.txt", std::string("a")));
    EXPECT_FALSE(builder.add_file("a.txt", std::string("again")));
    EXPECT_FALSE(builder.add_directory("../up"));
    EXPECT_FALSE(builder.add_file("/absolute", std::string()));
    EXPECT_EQ(builder.count(), 1u);
    EXPECT_EQ(builder.total_size(), 1u);
}

TEST(ClipboardFilesTests, parserRejectsMalformedData)
{
    std::string data = make_list();
    ClipboardFileList list;

    EXPECT_FALSE(list.parse(std::string()));
    EXPECT_FALSE(list.parse("ILFX" + data.substr(4)));

    // truncated anywhere
    for (std::size_t size = 0; size < data.size(); size += 7) {
        EXPECT_FALSE(list.parse(data.substr(0, size))) << "size " << size;
        EXPECT_TRUE(list.entries().empty());
    }

    // an absurd entry count
    std::string bad = data;
    bad[8] = '\x7f';
    EXPECT_FALSE(list.parse(bad));

    // an unsafe path
    bad = data;
    bad.replace(bad.find("report.pdf"), 10, "../../pdf.");
    EXPECT_FALSE(list.parse(bad));

    // a file whose contents lie beyond the data
    bad = data;
    bad.resize(bad.size() - 3);
    EXPECT_FALSE(list.parse(bad));

    // the original still parses
    EXPECT_TRUE(list.parse(data));
}

TEST(ClipboardFilesTests, safePaths)
{
    EXPECT_TRUE(ClipboardFileList::is_safe_path("a"));
    EXPECT_TRUE(ClipboardFileList::is_safe_path("dir/sub dir/file name.txt"));
    EXPECT_TRUE(ClipboardFileList::is_safe_path("\xc3\xa9t\xc3\xa9.txt"));
    EXPECT_TRUE(ClipboardFileList::is_safe_path("console.txt"));
    EXPECT_TRUE(ClipboardFileList::is_safe_path("com10"));

    EXPECT_FALSE(ClipboardFileList::is_safe_path(""));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("/a"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("a/"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("a//b"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("."));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("a/../b"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("a\\b"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("c:/a"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("a?b"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path(std::string("a\nb")));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("trailing."));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("trailing "));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("CON"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("dir/nul.txt"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path("com1"));
    EXPECT_FALSE(ClipboardFileList::is_safe_path(std::string(300, 'x')));
}

TEST(ClipboardFilesTests, describe)
{
    ClipboardFileList list;
    ASSERT_TRUE(list.parse(make_list()));
    EXPECT_EQ(describe_clipboard_files(list), "1 file and 1 folder");

    ClipboardFileList::Builder builder;
    builder.add_file("only.txt", std::string("x"));
    ASSERT_TRUE(list.parse(builder.finish()));
    EXPECT_EQ(describe_clipboard_files(list), "only.txt");

    builder.add_file("a", std::string());
    builder.add_file("b", std::string());
    builder.add_file("c", std::string());
    ASSERT_TRUE(list.parse(builder.finish()));
    EXPECT_EQ(describe_clipboard_files(list), "3 files");

    ASSERT_TRUE(list.parse(builder.finish()));
    EXPECT_EQ(describe_clipboard_files(list), "no files");
}

TEST(ClipboardFilesTests, readAndWriteFiles)
{
    fs::path source = make_temp_directory();
    fs::path target = make_temp_directory() / "received";

    // a file and a directory tree, including an empty directory
    {
        std::ofstream stream;
        open_utf8_path(stream, source / "notes.txt", std::ios::out | std::ios::binary);
        stream << "hello";
    }
    fs::create_directories(source / "tree" / "sub");
    fs::create_directories(source / "tree" / "empty");
    {
        std::ofstream stream;
        open_utf8_path(stream, source / "tree" / "sub" / "data.bin", std::ios::out | std::ios::binary);
        stream << std::string("\0\xff\x10", 3);
    }

    std::string data;
    ASSERT_TRUE(read_clipboard_files({source / "notes.txt", source / "tree"}, 1024, data));

    ClipboardFileList list;
    ASSERT_TRUE(list.parse(data));
    EXPECT_EQ(list.file_count(), 2u);
    EXPECT_EQ(list.directory_count(), 3u);
    EXPECT_EQ(list.total_size(), 8u);

    std::vector<fs::path> written = write_clipboard_files(data, target);
    ASSERT_EQ(written.size(), 2u);
    EXPECT_EQ(written[0], target / "notes.txt");
    EXPECT_EQ(written[1], target / "tree");
    EXPECT_EQ(read_file(target / "notes.txt"), "hello");
    EXPECT_EQ(read_file(target / "tree" / "sub" / "data.bin"), std::string("\0\xff\x10", 3));
    EXPECT_TRUE(fs::is_directory(target / "tree" / "empty"));

    // too large for the limit
    std::string too_large;
    EXPECT_FALSE(read_clipboard_files({source / "notes.txt"}, 4, too_large));

    // malformed data writes nothing
    EXPECT_TRUE(write_clipboard_files("garbage", target / "none").empty());
    EXPECT_FALSE(fs::exists(target / "none"));

    std::error_code ec;
    fs::remove_all(source, ec);
    fs::remove_all(target.parent_path(), ec);
}

} // namespace inputleap
