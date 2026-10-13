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

#include <functional>
#include <string>
#include <vector>

namespace inputleap {

/*! What the service may run.

    The service runs the command it is given with high privileges, so the command is checked
    against this policy first, see check_service_command(). The folders are canonical paths,
    i.e. what \c canonical returns for them.
*/
struct ServiceCommandPolicy {
    /// The folder that holds the server and client programs
    std::string program_dir;
    /// The folder that the log file must be in
    std::string log_dir;
    /// The folder that the profile folder must be inside of. Empty allows any folder.
    std::string profile_root;
    /// Whether paths compare without regard to case (Windows)
    bool case_insensitive_paths = false;
    /*! Returns the canonical form of an existing file or folder, with links resolved, or an
        empty string if it does not exist. */
    std::function<std::string(const std::string&)> canonical;
    /// Whether the user who sent the command can read the file
    std::function<bool(const std::string&)> user_can_read;
};

struct ServiceCommandCheck {
    /// Whether the command may be run
    bool ok = false;
    /// Whether it starts the server (otherwise the client)
    bool server = false;
    /// Why it may not be run
    std::string reason;
    /// The command split into its arguments
    std::vector<std::string> args;
    /// The log file given with --log, if any
    std::string log_file;
};

/*! Checks a command that was sent to the service.

    Only the InputLeap server and client from the program folder may be run, with the options
    the GUI uses. Paths are checked after resolving links: the log file must be in the log
    folder and the profile folder inside the profile root, so that the service cannot be made
    to write elsewhere. The configuration file must be readable by the user who asked.
*/
ServiceCommandCheck check_service_command(const std::string& command,
                                          const ServiceCommandPolicy& policy);

} // namespace inputleap
