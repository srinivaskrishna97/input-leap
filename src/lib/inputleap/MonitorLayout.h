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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inputleap {

/// A monitor of a screen, in the screen's coordinates
struct MonitorRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;

    bool contains(std::int32_t px, std::int32_t py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    bool operator==(const MonitorRect& other) const
    {
        return x == other.x && y == other.y && w == other.w && h == other.h;
    }
};

/*! The monitors of a screen.

    A screen reports its shape as one rectangle around all of its monitors. When the monitors
    differ in size, parts of that rectangle have no monitor, and the screen's own cursor can
    never be there. A client that reports its monitors (kMsgDMonitors) lets the server keep
    the cursor on them: a move into the empty part slides along the edge of the monitor the
    cursor is on, and a crossing that would land in the empty part lands on the nearest
    monitor instead. An empty layout means the monitors are not known, and the whole shape is
    treated as one monitor, as it always was.
*/
class MonitorLayout {
public:
    static constexpr std::size_t kMaxMonitors = 64;

    bool empty() const { return monitors_.empty(); }
    const std::vector<MonitorRect>& monitors() const { return monitors_; }

    /// Adds a monitor. Monitors without area and beyond kMaxMonitors are ignored.
    void add(const MonitorRect& monitor);
    void clear() { monitors_.clear(); }

    /// Whether the point is on a monitor. Always true for an empty layout.
    bool contains(std::int32_t x, std::int32_t y) const;

    /// Moves the point to the nearest point on a monitor, if it is on none.
    void clamp(std::int32_t& x, std::int32_t& y) const;

    /*! The position reached by a move from (x0, y0), a point on a monitor, towards (x1, y1):
        (x1, y1) itself when it is on a monitor, otherwise as far along as the edges of that
        monitor allow, with the other coordinate still free when the result is on a monitor. */
    void constrain(std::int32_t x0, std::int32_t y0, std::int32_t& x1, std::int32_t& y1) const;

    /// e.g. "1920x1080@0,0 1366x768@1920,0"
    std::string describe() const;

    /// The layout for kMsgDMonitors: a count and four 32-bit integers per monitor, big endian.
    std::string encode() const;
    static bool decode(const std::string& data, MonitorLayout& layout);

    bool operator==(const MonitorLayout& other) const { return monitors_ == other.monitors_; }
    bool operator!=(const MonitorLayout& other) const { return !(*this == other); }

private:
    const MonitorRect* monitor_at(std::int32_t x, std::int32_t y) const;

    std::vector<MonitorRect> monitors_;
};

} // namespace inputleap
