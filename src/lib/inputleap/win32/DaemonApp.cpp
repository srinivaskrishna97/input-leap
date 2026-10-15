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

#include "inputleap/win32/DaemonApp.h"

#include "inputleap/App.h"
#include "inputleap/ArgParser.h"
#include "inputleap/ServerArgs.h"
#include "inputleap/ClientArgs.h"
#include "inputleap/ServiceCommand.h"
#include "platform/MSWindowsLocalPeer.h"
#include "ipc/IpcClientProxy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcLogOutputter.h"
#include "net/SocketMultiplexer.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/EventQueue.h"
#include "base/log_outputters.h"
#include "base/Log.h"
#include "common/DataDirectories.h"

#include "arch/win32/ArchMiscWindows.h"
#include "arch/win32/XArchWindows.h"
#include "inputleap/Screen.h"
#include "platform/MSWindowsScreen.h"
#include "platform/MSWindowsDebugOutputter.h"
#include "platform/MSWindowsWatchdog.h"
#include "platform/MSWindowsEventQueueBuffer.h"
#include "platform/MSWindowsUtil.h"
#include "io/filesystem.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <iostream>
#include <sstream>

namespace inputleap {

DaemonApp* DaemonApp::s_instance = nullptr;

int
mainLoopStatic()
{
    DaemonApp::s_instance->mainLoop(true);
    return kExitSuccess;
}

int
mainLoopStatic(int, const char**)
{
    return ArchMiscWindows::runDaemon(mainLoopStatic);
}

DaemonApp::DaemonApp() :
    m_ipcServer(nullptr),
    m_ipcLogOutputter(nullptr),
    m_watchdog(nullptr),
    m_events(nullptr),
    m_fileLogOutputter(nullptr)
{
    s_instance = this;
}

DaemonApp::~DaemonApp()
{
}

int
DaemonApp::run(int argc, char** argv)
{
    // win32 instance needed for threading, etc.
    ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));

    Arch arch;
    arch.init();

    Log log;
    EventQueue events;
    m_events = &events;

    bool uninstall = false;
    try
    {
        // sends debug messages to visual studio console window.
        log.insert(new MSWindowsDebugOutputter());

        // default log level to system setting.
        std::string logLevel = arch.setting("LogLevel");
        if (logLevel != "")
            log.setFilter(logLevel.c_str());

        bool foreground = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg(argv[i]);

            if (arg == "/f" || arg == "-f") {
                foreground = true;
            }
            else if (arg == "/install") {
                uninstall = true;
                arch.installDaemon();
                return kExitSuccess;
            }
            else if (arg == "/uninstall") {
                arch.uninstallDaemon();
                return kExitSuccess;
            }
            else {
                std::stringstream ss;
                ss << "Unrecognized argument: " << arg;
                foregroundError(ss.str().c_str());
                return kExitArgs;
            }
        }

        if (foreground) {
            // add a console to catch Ctrl+C and run process in foreground
            // instead of daemonizing. useful for debugging.
            if (IsDebuggerPresent())
                AllocConsole();
            mainLoop(false);
        }
        else {
            arch.daemonize("InputLeap", mainLoopStatic);
        }

        return kExitSuccess;
    }
    catch (std::runtime_error& e) {
        std::string message = e.what();
        if (uninstall && (message.find("The service has not been started") != std::string::npos)) {
            // TODO: if we're keeping this use error code instead (what is it?!).
            // HACK: this message happens intermittently, not sure where from but
            // it's quite misleading for the user. they thing something has gone
            // horribly wrong, but it's just the service manager reporting a false
            // positive (the service has actually shut down in most cases).
        }
        else {
            foregroundError(message.c_str());
        }
        return kExitFailed;
    }
    catch (std::exception& e) {
        foregroundError(e.what());
        return kExitFailed;
    }
    catch (...) {
        foregroundError("Unrecognized error.");
        return kExitFailed;
    }
}

void
DaemonApp::mainLoop(bool daemonized)
{
    try
    {
        DAEMON_RUNNING(true);

        if (daemonized) {
            m_fileLogOutputter = new FileLogOutputter(logFilename().c_str());
            CLOG->insert(m_fileLogOutputter);
            if (!m_logFilenameWarning.empty()) {
                LOG_WARN("%s", m_logFilenameWarning.c_str());
            }
        }

        // create socket multiplexer.  this must happen after daemonization
        // on unix because threads evaporate across a fork().
        SocketMultiplexer multiplexer;

        // uses event queue, must be created here.
        m_ipcServer = new IpcServer(m_events, &multiplexer);

        // send logging to gui via ipc, log system adopts outputter.
        m_ipcLogOutputter = new IpcLogOutputter(*m_ipcServer, kIpcClientGui, true);
        CLOG->insert(m_ipcLogOutputter);

        m_watchdog = new MSWindowsWatchdog(daemonized, false, *m_ipcServer, *m_ipcLogOutputter);
        m_watchdog->setFileLogOutputter(m_fileLogOutputter);

        m_events->add_handler(EventType::IPC_SERVER_MESSAGE_RECEIVED, m_ipcServer,
                              [this](const auto& event) { handle_ipc_message(event); });

        // the daemon runs what it is told with high privileges, so only the user at the console
        // (or an administrator) may tell it anything or read its log
        m_ipcServer->set_connection_filter(
            [](int pid) { return MSWindowsLocalPeer::is_trusted_process(pid); });
        m_ipcServer->listen();

        // install the platform event queue to handle service stop events.
        m_events->set_buffer(std::make_unique<MSWindowsEventQueueBuffer>(m_events));

        std::string command = ARCH->setting("Command");
        bool elevate = ARCH->setting("Elevate") == "1";
        std::string reason;
        if (command != "" && !check_command(command, reason)) {
            LOG_WARN("not running the last known command: %s", reason.c_str());
            command.clear();
        }
        if (command != "") {
            LOG_INFO("using last known command: %s", command.c_str());
            m_watchdog->setCommand(command, elevate);
        }

        m_watchdog->startAsync();

        m_events->loop();

        m_watchdog->stop();
        delete m_watchdog;

        m_events->remove_handler(EventType::IPC_SERVER_MESSAGE_RECEIVED, m_ipcServer);

        CLOG->remove(m_ipcLogOutputter);
        delete m_ipcLogOutputter;
        delete m_ipcServer;

        DAEMON_RUNNING(false);
    }
    catch (std::exception& e) {
        LOG_CRIT("An error occurred: %s", e.what());
    }
    catch (...) {
        LOG_CRIT("An unknown error occurred.\n");
    }
}

void
DaemonApp::foregroundError(const char* message)
{
    MessageBox(nullptr, message, "InputLeap Service", MB_OK | MB_ICONERROR);
}

bool DaemonApp::check_command(const std::string& command, std::string& reason)
{
    ServiceCommandPolicy policy;
    policy.program_dir = MSWindowsLocalPeer::canonical_path(MSWindowsLocalPeer::program_directory());
    policy.log_dir = MSWindowsLocalPeer::canonical_path(inputleap::DataDirectories::global().u8string());
    policy.profile_root = MSWindowsLocalPeer::canonical_path(MSWindowsLocalPeer::console_user_local_appdata());
    policy.case_insensitive_paths = true;
    policy.canonical = &MSWindowsLocalPeer::canonical_path;
    policy.user_can_read = &MSWindowsLocalPeer::console_user_can_read;
    if (policy.program_dir.empty() || policy.log_dir.empty()) {
        reason = "the program or log folder cannot be determined";
        return false;
    }
    if (policy.profile_root.empty()) {
        reason = "the user at the console cannot be determined";
        return false;
    }

    ServiceCommandCheck check = check_service_command(command, policy);
    reason = check.reason;
    return check.ok;
}

std::string DaemonApp::core_log_filename(const std::string& requested)
{
    // the server or client always logs to a file, so that there is something to look at after
    // a problem; the file is in the log folder, which check_command() made sure of
    std::string filename = requested;
    if (filename.empty()) {
        filename = (inputleap::DataDirectories::global() / "input-leap.log").u8string();
    }
    MSWindowsUtil::createDirectory(filename, true);
    return filename;
}

namespace {

// whether the file is directly in the folder, comparing the paths as written without regard to
// case or the kind of separators
bool is_in_folder(const std::string& file, const std::string& folder)
{
    auto normalized = [](std::string path) {
        std::replace(path.begin(), path.end(), '/', '\\');
        while (path.size() > 3 && path.back() == '\\') {
            path.pop_back();
        }
        std::transform(path.begin(), path.end(), path.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return path;
    };
    std::string parent = normalized(file);
    auto separator = parent.find_last_of('\\');
    if (separator == std::string::npos) {
        return false;
    }
    parent.erase(separator);
    return parent == normalized(folder);
}

} // namespace

std::string
DaemonApp::logFilename()
{
    const fs::path& folder = inputleap::DataDirectories::global();
    std::string logFilename = ARCH->setting("LogFilename");
    if (logFilename.empty()) {
        logFilename = (folder / LOG_FILENAME).u8string();
    }

    // a path from an older version, e.g. in Barrier's program folder, may point to a folder
    // that does not exist; the log then goes to the log folder, so that there is one
    std::string reason;
    if (!is_in_folder(logFilename, folder.u8string())) {
        reason = "it is not in the log folder " + folder.u8string();
    }
    else {
        MSWindowsUtil::createDirectory(logFilename, true);
        std::error_code ec;
        if (!fs::is_directory(fs::u8path(logFilename).parent_path(), ec)) {
            reason = "its folder cannot be created";
        }
    }
    const std::string fallback = (folder / "input-leap.log").u8string();
    if (!reason.empty() && logFilename != fallback) {
        m_logFilenameWarning = "not logging to " + logFilename + " because " + reason +
                               "; logging to " + fallback + " instead";
        logFilename = fallback;
        MSWindowsUtil::createDirectory(logFilename, true);
        try {
            ARCH->setting("LogFilename", logFilename);
        }
        catch (std::runtime_error& e) {
            m_logFilenameWarning += std::string(" (cannot save it: ") + e.what() + ")";
        }
    }
    return logFilename;
}

void DaemonApp::handle_ipc_message(const Event& e)
{
    const IpcMessage& m = e.get_data_as<IpcMessage>();
    switch (m.type()) {
        case kIpcCommand: {
            const IpcCommandMessage& cm = static_cast<const IpcCommandMessage&>(m);
            std::string command = cm.command();

            // if empty quotes, clear.
            if (command == "\"\"") {
                command.clear();
            }

            if (!command.empty()) {
                std::string reason;
                if (!check_command(command, reason)) {
                    LOG_WARN("refused a command: %s", reason.c_str());
                    break;
                }
                LOG_DEBUG("new command, elevate=%d command=%s", cm.elevate(), command.c_str());

                std::vector<std::string> argsArray;
                ArgParser::splitCommandString(command, argsArray);
                ArgParser argParser(nullptr);
                const char** argv = argParser.getArgv(argsArray);
                ServerArgs serverArgs;
                ClientArgs clientArgs;
                int argc = static_cast<int>(argsArray.size());
                bool server = argsArray[0].find("input-leaps") != std::string::npos ? true : false;
                ArgsBase* argBase = nullptr;

                if (server) {
                    argParser.parseServerArgs(serverArgs, argc, argv);
                    argBase = &serverArgs;
                }
                else {
                    argParser.parseClientArgs(clientArgs, argc, argv);
                    argBase = &clientArgs;
                }

                delete[] argv;

                std::string logLevel(argBase->m_logFilter);
                if (!logLevel.empty()) {
                    try {
                        // change log level based on that in the command string
                        // and change to that log level now.
                        ARCH->setting("LogLevel", logLevel);
                        CLOG->setFilter(logLevel.c_str());
                    }
                    catch (std::runtime_error& e) {
                        LOG_ERR("failed to save LogLevel setting, %s", e.what());
                    }
                }

                // eg. no log-to-file while running in foreground
                if (m_fileLogOutputter != nullptr) {
                    std::string logFilename = core_log_filename(
                        argBase->m_logFile != nullptr ? std::string(argBase->m_logFile) : std::string());
                    ARCH->setting("LogFilename", logFilename);
                    m_watchdog->setFileLogOutputter(m_fileLogOutputter);
                    if (argBase->m_logFile != nullptr) {
                        command = ArgParser::assembleCommand(argsArray, "--log", 1);
                        LOG_DEBUG("removed log file argument from command, elevate=%d command=%s",
                                  cm.elevate(), command.c_str());
                    }
                    LOG_INFO("logging the server or client to %s", logFilename.c_str());
                    m_fileLogOutputter->setLogFilename(logFilename.c_str());
                }
            }
            else {
                LOG_DEBUG("empty command, elevate=%d", cm.elevate());
            }

            try {
                // store command in system settings. this is used when the daemon
                // next starts.
                ARCH->setting("Command", command);

                // TODO: it would be nice to store bools/ints...
                ARCH->setting("Elevate", std::string(cm.elevate() ? "1" : "0"));
            }
            catch (std::runtime_error& e) {
                LOG_ERR("failed to save settings, %s", e.what());
            }

            // tell the relauncher about the new command. this causes the
            // relauncher to stop the existing command and start the new
            // command.
            m_watchdog->setCommand(command, cm.elevate());

            break;
        }

        case kIpcHello:
            const auto& hm = static_cast<const IpcHelloMessage&>(m);
            std::string type;
            switch (hm.clientType()) {
                case kIpcClientGui: type = "gui"; break;
                case kIpcClientNode: type = "node"; break;
                default: type = "unknown"; break;
            }

            LOG_DEBUG("ipc hello, type=%s", type.c_str());

            const char * serverstatus = m_watchdog->isProcessActive() ? "active" : "not active";

            // using CLOG_PRINT here allows the GUI to see that the server status
            // regardless of which log level is set
            LOG_PRINT("server status: %s", serverstatus);

            m_ipcLogOutputter->notifyBuffer();
            break;
    }
}

} // namespace inputleap
