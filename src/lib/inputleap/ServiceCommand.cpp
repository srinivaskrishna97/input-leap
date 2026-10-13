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

#include "inputleap/ServiceCommand.h"
#include "inputleap/ArgParser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace inputleap {

namespace {

std::string lowercase(std::string text)
{
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// a path with forward slashes, no trailing slash and optionally no case
std::string normalized(std::string path, bool case_insensitive)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/' && !(path.size() == 3 && path[1] == ':')) {
        path.pop_back();
    }
    return case_insensitive ? lowercase(path) : path;
}

bool same_path(const std::string& a, const std::string& b, bool case_insensitive)
{
    return !a.empty() && normalized(a, case_insensitive) == normalized(b, case_insensitive);
}

bool is_inside(const std::string& path, const std::string& root, bool case_insensitive)
{
    std::string p = normalized(path, case_insensitive);
    std::string r = normalized(root, case_insensitive);
    if (r.empty() || p.empty()) {
        return false;
    }
    if (r.back() == '/') {
        return p.compare(0, r.size(), r) == 0;
    }
    return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/');
}

std::string parent_of(const std::string& path)
{
    std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return "";
    }
    return pos == 0 ? path.substr(0, 1) : path.substr(0, pos);
}

std::string filename_of(const std::string& path)
{
    std::size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool is_log_level(const std::string& level)
{
    static const char* const names[] = {
        "FATAL", "ERROR", "WARNING", "NOTE", "INFO", "DEBUG", "DEBUG1", "DEBUG2", "DEBUG3",
        "DEBUG4", "DEBUG5"
    };
    std::string upper = level;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for (const char* name : names) {
        if (upper == name) {
            return true;
        }
    }
    return false;
}

// a host name or address, optionally in brackets and with a port
bool is_address(const std::string& address)
{
    if (address.empty() || address.size() > 256) {
        return false;
    }
    for (char c : address) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' ||
                  c == '[' || c == ']' || c == '-' || c == '_' || c == '%';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool is_screen_name(const std::string& name)
{
    if (name.empty() || name.size() > 256) {
        return false;
    }
    for (char c : name) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) {
            return false;
        }
    }
    return true;
}

bool is_integer(const std::string& text)
{
    if (text.empty() || text.size() > 10) {
        return false;
    }
    std::size_t start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (start == text.size()) {
        return false;
    }
    return std::all_of(text.begin() + start, text.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

bool is_flag(const std::string& arg)
{
    static const char* const flags[] = {
        "-f", "--no-daemon", "--no-tray", "--ipc", "--no-hooks", "--enable-drag-drop",
        "--no-clipboard-status", "--disable-crypto", "--enable-crypto", "--restart", "-1",
        "--no-restart", "--stop-on-desk-switch"
    };
    for (const char* flag : flags) {
        if (arg == flag) {
            return true;
        }
    }
    return false;
}

} // namespace

ServiceCommandCheck check_service_command(const std::string& command,
                                          const ServiceCommandPolicy& policy)
{
    ServiceCommandCheck result;
    const bool ci = policy.case_insensitive_paths;
    auto canonical = [&policy](const std::string& path) {
        return policy.canonical ? policy.canonical(path) : path;
    };
    auto refuse = [&result](const std::string& reason) {
        result.ok = false;
        result.reason = reason;
        return result;
    };

    std::string copy = command;
    ArgParser::splitCommandString(copy, result.args);
    const std::vector<std::string>& args = result.args;
    if (args.empty() || args[0].empty()) {
        return refuse("the command is empty");
    }

    // the program: the server or the client from the program folder, not a link to elsewhere
    const std::string program = lowercase(filename_of(args[0]));
    if (program == "input-leaps.exe" || program == "input-leaps") {
        result.server = true;
    }
    else if (program == "input-leapc.exe" || program == "input-leapc") {
        result.server = false;
    }
    else {
        return refuse("the program is not the InputLeap server or client: " + args[0]);
    }
    // the given path must be in the program folder: Windows searches for libraries next to the
    // path a program was started with, even when that path is a link to the real program
    if (!same_path(canonical(parent_of(args[0])), policy.program_dir, ci)) {
        return refuse("the program is not in the InputLeap folder: " + args[0]);
    }
    std::string exe = canonical(args[0]);
    if (exe.empty()) {
        return refuse("the program does not exist: " + args[0]);
    }
    if (!same_path(parent_of(exe), policy.program_dir, ci)) {
        return refuse("the program is a link out of the InputLeap folder: " + exe);
    }
    if (lowercase(filename_of(exe)) != program) {
        return refuse("the program is a link to another program: " + exe);
    }

    // the options the GUI uses, and nothing else
    bool have_address = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        std::string value;
        auto take_value = [&]() {
            if (i + 1 >= args.size()) {
                return false;
            }
            value = args[++i];
            return true;
        };

        if (is_flag(arg)) {
            continue;
        }
        if (arg == "-d" || arg == "--debug") {
            if (!take_value() || !is_log_level(value)) {
                return refuse("invalid log level");
            }
        }
        else if (arg == "-n" || arg == "--name") {
            if (!take_value() || !is_screen_name(value)) {
                return refuse("invalid screen name");
            }
        }
        else if (arg == "-l" || arg == "--log") {
            if (!take_value()) {
                return refuse("missing log file");
            }
            std::string name = filename_of(value);
            std::string dir = canonical(parent_of(value));
            if (name.empty() || name == "." || name == ".." || dir.empty() ||
                !same_path(dir, policy.log_dir, ci)) {
                return refuse("the log file is not in the InputLeap log folder: " + value);
            }
            result.log_file = value;
        }
        else if (arg == "--profile-dir") {
            if (!take_value()) {
                return refuse("missing profile folder");
            }
            std::string dir = canonical(value);
            if (dir.empty()) {
                return refuse("the profile folder does not exist: " + value);
            }
            if (!policy.profile_root.empty() && !is_inside(dir, policy.profile_root, ci)) {
                return refuse("the profile folder is not in the user's folder: " + dir);
            }
        }
        else if (result.server && (arg == "-a" || arg == "--address")) {
            if (!take_value() || !is_address(value)) {
                return refuse("invalid listen address");
            }
        }
        else if (result.server && (arg == "-c" || arg == "--config")) {
            if (!take_value()) {
                return refuse("missing configuration file");
            }
            if (policy.user_can_read && !policy.user_can_read(value)) {
                return refuse("the configuration file cannot be read by the user: " + value);
            }
        }
        else if (result.server && arg == "--disable-client-cert-checking") {
            continue;
        }
        else if (!result.server && arg == "--yscroll") {
            if (!take_value() || !is_integer(value)) {
                return refuse("invalid --yscroll value");
            }
        }
        else if (!result.server && i == args.size() - 1 && arg[0] != '-') {
            if (!is_address(arg)) {
                return refuse("invalid server address: " + arg);
            }
            have_address = true;
        }
        else {
            return refuse("unknown option: " + arg);
        }
    }
    if (!result.server && !have_address) {
        return refuse("the client needs a server address");
    }

    result.ok = true;
    return result;
}

} // namespace inputleap
