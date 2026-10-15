/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
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

#include "arch/Arch.h"
#include "base/Fwd.h"
#include "ipc/IpcServer.h"

#include <string>

namespace inputleap {

class IpcLogOutputter;
class MSWindowsWatchdog;

class DaemonApp {

public:
    DaemonApp();
    virtual ~DaemonApp();
    int run(int argc, char** argv);
    void mainLoop(bool daemonized);

private:
    void daemonize();
    void foregroundError(const char* message);
    std::string logFilename();
    void handle_ipc_message(const Event& event);
    // whether the command may be run, with the reason if not
    bool check_command(const std::string& command, std::string& reason);
    // the log file for the server or client; the default if the given one is not acceptable
    std::string core_log_filename(const std::string& requested);

public:
    static DaemonApp* s_instance;

    MSWindowsWatchdog* m_watchdog;

private:
    IpcServer* m_ipcServer;
    IpcLogOutputter* m_ipcLogOutputter;
    IEventQueue* m_events;
    FileLogOutputter* m_fileLogOutputter;
    // why logFilename() did not use the stored log file, logged once the log file is open
    std::string m_logFilenameWarning;
};

#define LOG_FILENAME "input-leapd.log"

} // namespace inputleap
