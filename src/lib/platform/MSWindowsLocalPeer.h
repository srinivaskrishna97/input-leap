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

#include <string>

namespace inputleap {

/*! Identifies the local processes that talk to the service, and checks paths on their behalf.

    The service runs with high privileges and takes commands over a local connection, so it
    must only take them from the user at the console (or an administrator) and must not be
    made to read or write files that user could not. All paths are UTF-8.
*/
class MSWindowsLocalPeer {
public:
    /*! Whether the process may control the service: it runs as the user logged on at the
        console, as an administrator, or as the system. */
    static bool is_trusted_process(int pid);

    /*! The canonical form of an existing file or folder, with links and junctions resolved,
        or an empty string if it does not exist. */
    static std::string canonical_path(const std::string& path);

    /// Whether the user at the console can read the file.
    static bool console_user_can_read(const std::string& path);

    /// The console user's local application data folder, or an empty string if unknown.
    static std::string console_user_local_appdata();

    /// The folder of the running program.
    static std::string program_directory();

    /// Whether this process runs with high or system integrity, e.g. started by the service.
    static bool running_elevated();

    /*! Starts a program as the user at the console, in their session and without a window.
        Returns the process handle (a HANDLE), or nullptr if it could not be started. */
    static void* start_as_console_user(const std::string& command_line);
};

} // namespace inputleap
