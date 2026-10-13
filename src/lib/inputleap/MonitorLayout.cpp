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

#include <algorithm>
#include <cstdio>

namespace inputleap {

namespace {

std::int32_t clamp_into(std::int32_t value, std::int32_t low, std::int32_t high)
{
    return std::min(std::max(value, low), high);
}

void put_be32(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

std::uint32_t get_be32(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

} // namespace

void MonitorLayout::add(const MonitorRect& monitor)
{
    if (monitor.w <= 0 || monitor.h <= 0 || monitors_.size() >= kMaxMonitors) {
        return;
    }
    monitors_.push_back(monitor);
}

const MonitorRect* MonitorLayout::monitor_at(std::int32_t x, std::int32_t y) const
{
    for (const MonitorRect& monitor : monitors_) {
        if (monitor.contains(x, y)) {
            return &monitor;
        }
    }
    return nullptr;
}

bool MonitorLayout::contains(std::int32_t x, std::int32_t y) const
{
    return monitors_.empty() || monitor_at(x, y) != nullptr;
}

void MonitorLayout::clamp(std::int32_t& x, std::int32_t& y) const
{
    if (contains(x, y)) {
        return;
    }
    std::int64_t best = -1;
    std::int32_t bestX = x;
    std::int32_t bestY = y;
    for (const MonitorRect& monitor : monitors_) {
        std::int32_t cx = clamp_into(x, monitor.x, monitor.x + monitor.w - 1);
        std::int32_t cy = clamp_into(y, monitor.y, monitor.y + monitor.h - 1);
        std::int64_t dx = static_cast<std::int64_t>(cx) - x;
        std::int64_t dy = static_cast<std::int64_t>(cy) - y;
        std::int64_t distance = dx * dx + dy * dy;
        if (best < 0 || distance < best) {
            best = distance;
            bestX = cx;
            bestY = cy;
        }
    }
    x = bestX;
    y = bestY;
}

void MonitorLayout::constrain(std::int32_t x0, std::int32_t y0, std::int32_t& x1,
                              std::int32_t& y1) const
{
    if (contains(x1, y1)) {
        return;
    }
    const MonitorRect* monitor = monitor_at(x0, y0);
    if (monitor == nullptr) {
        // the cursor is not on a monitor, which should not happen; get it onto one
        clamp(x1, y1);
        return;
    }

    // as far as the monitor the cursor is on allows, and the other coordinate stays free when
    // that lands on a monitor (the cursor then slides along the edge)
    std::int32_t cx = clamp_into(x1, monitor->x, monitor->x + monitor->w - 1);
    std::int32_t cy = clamp_into(y1, monitor->y, monitor->y + monitor->h - 1);
    if (contains(x1, cy)) {
        y1 = cy;
    }
    else if (contains(cx, y1)) {
        x1 = cx;
    }
    else {
        x1 = cx;
        y1 = cy;
    }
}

std::string MonitorLayout::describe() const
{
    std::string result;
    for (const MonitorRect& monitor : monitors_) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%dx%d@%d,%d", monitor.w, monitor.h, monitor.x,
                      monitor.y);
        if (!result.empty()) {
            result += ' ';
        }
        result += buffer;
    }
    return result;
}

std::string MonitorLayout::encode() const
{
    std::string out;
    put_be32(out, static_cast<std::uint32_t>(monitors_.size()));
    for (const MonitorRect& monitor : monitors_) {
        put_be32(out, static_cast<std::uint32_t>(monitor.x));
        put_be32(out, static_cast<std::uint32_t>(monitor.y));
        put_be32(out, static_cast<std::uint32_t>(monitor.w));
        put_be32(out, static_cast<std::uint32_t>(monitor.h));
    }
    return out;
}

bool MonitorLayout::decode(const std::string& data, MonitorLayout& layout)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 4) {
        return false;
    }
    std::uint32_t count = get_be32(p);
    if (count == 0 || count > kMaxMonitors || data.size() != 4 + 16 * static_cast<std::size_t>(count)) {
        return false;
    }
    MonitorLayout result;
    for (std::uint32_t i = 0; i < count; ++i) {
        const unsigned char* entry = p + 4 + 16 * i;
        MonitorRect monitor;
        monitor.x = static_cast<std::int32_t>(get_be32(entry));
        monitor.y = static_cast<std::int32_t>(get_be32(entry + 4));
        monitor.w = static_cast<std::int32_t>(get_be32(entry + 8));
        monitor.h = static_cast<std::int32_t>(get_be32(entry + 12));
        if (monitor.w <= 0 || monitor.h <= 0) {
            return false;
        }
        result.add(monitor);
    }
    layout = result;
    return true;
}

} // namespace inputleap
