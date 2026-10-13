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

#include "inputleap/MonitorLayout.h"

#include <gtest/gtest.h>

namespace inputleap {

namespace {

// a laptop display with a smaller external display to its right, aligned at the top
MonitorLayout laptop()
{
    MonitorLayout layout;
    layout.add(MonitorRect{0, 0, 1920, 1080});
    layout.add(MonitorRect{1920, 0, 1366, 768});
    return layout;
}

} // namespace

TEST(MonitorLayoutTests, an_empty_layout_contains_everything_and_changes_nothing)
{
    MonitorLayout layout;
    EXPECT_TRUE(layout.empty());
    EXPECT_TRUE(layout.contains(-5000, 77));
    std::int32_t x = 4000, y = 900;
    layout.clamp(x, y);
    EXPECT_EQ(x, 4000);
    EXPECT_EQ(y, 900);
    layout.constrain(100, 100, x, y);
    EXPECT_EQ(x, 4000);
    EXPECT_EQ(y, 900);
}

TEST(MonitorLayoutTests, knows_which_points_are_on_a_monitor)
{
    MonitorLayout layout = laptop();
    EXPECT_TRUE(layout.contains(0, 0));
    EXPECT_TRUE(layout.contains(1919, 1079));
    EXPECT_TRUE(layout.contains(1920, 767));
    EXPECT_TRUE(layout.contains(3285, 0));
    EXPECT_FALSE(layout.contains(1920, 768));
    EXPECT_FALSE(layout.contains(3285, 1079));
    EXPECT_FALSE(layout.contains(-1, 0));
    EXPECT_FALSE(layout.contains(3286, 0));
}

TEST(MonitorLayoutTests, clamps_to_the_nearest_monitor)
{
    MonitorLayout layout = laptop();
    // in the empty corner below the external display, closest to its bottom edge
    std::int32_t x = 2500, y = 900;
    layout.clamp(x, y);
    EXPECT_EQ(x, 2500);
    EXPECT_EQ(y, 767);
    // in the empty corner but closer to the laptop display
    x = 1930;
    y = 1000;
    layout.clamp(x, y);
    EXPECT_EQ(x, 1919);
    EXPECT_EQ(y, 1000);
    // beyond the right edge, low: the nearest point is the external display's corner
    x = 4000;
    y = 1079;
    layout.clamp(x, y);
    EXPECT_EQ(x, 3285);
    EXPECT_EQ(y, 767);
    // already on a monitor
    x = 100;
    y = 100;
    layout.clamp(x, y);
    EXPECT_EQ(x, 100);
    EXPECT_EQ(y, 100);
}

TEST(MonitorLayoutTests, a_move_into_the_empty_part_slides_along_the_edge)
{
    MonitorLayout layout = laptop();
    // moving right at a height where the external display does not exist: x stops at the
    // laptop display's edge, y stays free
    std::int32_t x = 1950, y = 910;
    layout.constrain(1910, 900, x, y);
    EXPECT_EQ(x, 1919);
    EXPECT_EQ(y, 910);
    // moving down and right at the bottom of the external display: y stops, x goes on
    x = 2520;
    y = 790;
    layout.constrain(2500, 760, x, y);
    EXPECT_EQ(x, 2520);
    EXPECT_EQ(y, 767);
    // moving between the displays where they touch is free
    x = 1950;
    y = 500;
    layout.constrain(1910, 500, x, y);
    EXPECT_EQ(x, 1950);
    EXPECT_EQ(y, 500);
    // into a corner where neither coordinate can go on: both stop
    x = 3300;
    y = 800;
    layout.constrain(3280, 760, x, y);
    EXPECT_EQ(x, 3285);
    EXPECT_EQ(y, 767);
    // a move beyond the screen stops at the edge; whether it is a crossing to another screen
    // is decided by the server before the layout is consulted
    x = -10;
    y = 500;
    layout.constrain(5, 500, x, y);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 500);
}

TEST(MonitorLayoutTests, encodes_and_decodes)
{
    MonitorLayout layout;
    layout.add(MonitorRect{-1920, 0, 1920, 1080});
    layout.add(MonitorRect{0, 0, 2560, 1440});
    std::string encoded = layout.encode();
    EXPECT_EQ(encoded.size(), 4u + 2 * 16u);

    MonitorLayout decoded;
    ASSERT_TRUE(MonitorLayout::decode(encoded, decoded));
    EXPECT_EQ(decoded, layout);
    EXPECT_EQ(decoded.describe(), "1920x1080@-1920,0 2560x1440@0,0");

    // malformed layouts are refused and leave the target alone
    MonitorLayout untouched = laptop();
    EXPECT_FALSE(MonitorLayout::decode("", untouched));
    EXPECT_FALSE(MonitorLayout::decode(encoded.substr(0, 20), untouched));
    EXPECT_FALSE(MonitorLayout::decode(std::string(4, '\0'), untouched));
    std::string zero_size = MonitorLayout().encode();
    EXPECT_FALSE(MonitorLayout::decode(zero_size, untouched));
    EXPECT_EQ(untouched, laptop());
}

TEST(MonitorLayoutTests, ignores_monitors_without_area_and_too_many)
{
    MonitorLayout layout;
    layout.add(MonitorRect{0, 0, 0, 1080});
    layout.add(MonitorRect{0, 0, 1920, 0});
    EXPECT_TRUE(layout.empty());
    for (int i = 0; i < 100; ++i) {
        layout.add(MonitorRect{i * 100, 0, 100, 100});
    }
    EXPECT_EQ(layout.monitors().size(), MonitorLayout::kMaxMonitors);
}

} // namespace inputleap
