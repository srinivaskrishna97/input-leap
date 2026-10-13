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

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

namespace inputleap {

namespace {

// a Windows installation: the program folder, the log folder, the user's profile, and a link
// in the user's folder that pretends to be the server
ServiceCommandPolicy windows_policy()
{
    ServiceCommandPolicy policy;
    policy.program_dir = "C:\\Program Files\\InputLeap";
    policy.log_dir = "C:\\ProgramData\\InputLeap";
    policy.profile_root = "C:\\Users\\alice\\AppData\\Local";
    policy.case_insensitive_paths = true;
    policy.canonical = [](const std::string& path) {
        static const std::map<std::string, std::string> existing = {
            {"c:/program files/inputleap/input-leaps.exe", "C:\\Program Files\\InputLeap\\input-leaps.exe"},
            {"c:/program files/inputleap/input-leapc.exe", "C:\\Program Files\\InputLeap\\input-leapc.exe"},
            {"c:/program files/inputleap", "C:\\Program Files\\InputLeap"},
            {"c:/programdata/inputleap", "C:\\ProgramData\\InputLeap"},
            {"c:/users/alice/appdata/local/inputleap", "C:\\Users\\alice\\AppData\\Local\\InputLeap"},
            {"c:/users/alice/appdata/local", "C:\\Users\\alice\\AppData\\Local"},
            {"c:/users/alice/desktop", "C:\\Users\\alice\\Desktop"},
            // a link in the user's folder that points at the real server
            {"c:/users/alice/input-leaps.exe", "C:\\Program Files\\InputLeap\\input-leaps.exe"},
            // a link named like the server that points at another program
            {"c:/program files/inputleap/link/input-leaps.exe", "C:\\Windows\\System32\\cmd.exe"},
            {"c:/program files/inputleap/link", "C:\\Program Files\\InputLeap\\link"},
            // the user's profile folder when it is a link to the Windows folder
            {"c:/users/alice/appdata/local/evil", "C:\\Windows\\System32"},
            {"c:/users/alice/input-leap.sgc", "C:\\Users\\alice\\input-leap.sgc"},
        };
        std::string key = path;
        std::replace(key.begin(), key.end(), '\\', '/');
        for (char& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        auto found = existing.find(key);
        return found == existing.end() ? std::string() : found->second;
    };
    policy.user_can_read = [](const std::string& path) {
        return path.find("secret") == std::string::npos;
    };
    return policy;
}

const char* kServerCommand =
    "\"C:/Program Files/InputLeap/input-leaps.exe\" -f --no-tray --debug DEBUG1 --name PC --ipc "
    "--profile-dir \"C:\\Users\\alice\\AppData\\Local\\InputLeap\" --disable-client-cert-checking "
    "-c C:/Users/alice/input-leap.sgc --address :24800";

const char* kClientCommand =
    "\"C:/Program Files/InputLeap/input-leapc.exe\" -f --no-tray --debug INFO --name Laptop --ipc "
    "--stop-on-desk-switch --profile-dir \"C:\\Users\\alice\\AppData\\Local\\InputLeap\" "
    "--log \"C:\\ProgramData\\InputLeap\\input-leap.log\" [192.168.21.25]:24800";

} // namespace

TEST(ServiceCommandTests, accepts_the_servers_command_from_the_gui)
{
    auto check = check_service_command(kServerCommand, windows_policy());
    EXPECT_TRUE(check.ok) << check.reason;
    EXPECT_TRUE(check.server);
    EXPECT_EQ(check.args.size(), 15u);
    EXPECT_EQ(check.args[0], "C:/Program Files/InputLeap/input-leaps.exe");
    EXPECT_TRUE(check.log_file.empty());
}

TEST(ServiceCommandTests, accepts_the_clients_command_from_the_gui)
{
    auto check = check_service_command(kClientCommand, windows_policy());
    EXPECT_TRUE(check.ok) << check.reason;
    EXPECT_FALSE(check.server);
    EXPECT_EQ(check.log_file, "C:\\ProgramData\\InputLeap\\input-leap.log");
}

TEST(ServiceCommandTests, refuses_other_programs)
{
    auto policy = windows_policy();
    EXPECT_FALSE(check_service_command("", policy).ok);
    EXPECT_FALSE(check_service_command("cmd.exe /c whoami", policy).ok);
    EXPECT_FALSE(check_service_command("\"C:\\Windows\\System32\\cmd.exe\" /c whoami", policy).ok);
    // the right name in the wrong folder, even as a link to the real program
    EXPECT_FALSE(check_service_command("C:/Users/alice/input-leaps.exe -f", policy).ok);
    // the right name in the program folder as a link to another program
    EXPECT_FALSE(check_service_command("\"C:/Program Files/InputLeap/link/input-leaps.exe\" -f",
                                       policy).ok);
    // a program that does not exist
    EXPECT_FALSE(check_service_command("\"C:/Program Files/InputLeap/input-leapd.exe\" -f",
                                       policy).ok);
}

TEST(ServiceCommandTests, refuses_options_that_run_or_load_other_code)
{
    auto policy = windows_policy();
    const std::string server = "\"C:/Program Files/InputLeap/input-leaps.exe\" -f";
    EXPECT_FALSE(check_service_command(server + " --screen-change-script C:/evil.bat", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --plugin-dir C:/evil", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --drop-dir C:/evil", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --unknown", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --debug", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --debug LOUD", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --name \"a\nb\"", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --address \"a b\"", policy).ok);
    // client options are not server options and the other way around
    EXPECT_FALSE(check_service_command(server + " --yscroll 120", policy).ok);
    EXPECT_FALSE(check_service_command(
        "\"C:/Program Files/InputLeap/input-leapc.exe\" -f -c C:/x.sgc pc:24800", policy).ok);
}

TEST(ServiceCommandTests, keeps_files_in_their_folders)
{
    auto policy = windows_policy();
    const std::string server = "\"C:/Program Files/InputLeap/input-leaps.exe\" -f";
    // the log file may only be in the log folder
    EXPECT_TRUE(check_service_command(server + " --log C:/ProgramData/InputLeap/core.log", policy).ok);
    EXPECT_TRUE(check_service_command(server + " --log c:\\programdata\\inputleap\\CORE.LOG", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --log C:/Windows/System32/drivers/etc/hosts", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --log C:/ProgramData/InputLeap/../evil.log", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --log C:/ProgramData/InputLeap/", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --log C:/ProgramData/InputLeap", policy).ok);
    // the profile folder must be inside the user's folder, after resolving links
    EXPECT_TRUE(check_service_command(server + " --profile-dir C:/Users/alice/AppData/Local", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --profile-dir C:/Users/alice/Desktop", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --profile-dir C:/Users/alice/AppData/Local/evil", policy).ok);
    EXPECT_FALSE(check_service_command(server + " --profile-dir C:/Users/alice/AppData/Local/missing", policy).ok);
    // the configuration file must be readable by the user
    EXPECT_FALSE(check_service_command(server + " -c C:/Users/bob/secret.sgc", policy).ok);
}

TEST(ServiceCommandTests, the_client_needs_one_address)
{
    auto policy = windows_policy();
    const std::string client = "\"C:/Program Files/InputLeap/input-leapc.exe\" -f";
    EXPECT_FALSE(check_service_command(client, policy).ok);
    EXPECT_TRUE(check_service_command(client + " pc", policy).ok);
    EXPECT_TRUE(check_service_command(client + " [fe80::1%12]:24800", policy).ok);
    EXPECT_TRUE(check_service_command(client + " --yscroll -120 192.168.21.25:24800", policy).ok);
    EXPECT_FALSE(check_service_command(client + " \"pc; whoami\"", policy).ok);
    EXPECT_FALSE(check_service_command(client + " pc --name", policy).ok);
}

TEST(ServiceCommandTests, paths_compare_exactly_on_case_sensitive_systems)
{
    ServiceCommandPolicy policy;
    policy.program_dir = "/usr/bin";
    policy.log_dir = "/var/log/input-leap";
    policy.canonical = [](const std::string& path) {
        if (path == "/usr/bin/input-leaps" || path == "/usr/bin" || path == "/var/log/input-leap") {
            return path;
        }
        return std::string();
    };
    EXPECT_TRUE(check_service_command("/usr/bin/input-leaps -f --log /var/log/input-leap/s.log", policy).ok);
    EXPECT_FALSE(check_service_command("/usr/bin/input-leaps -f --log /var/log/INPUT-LEAP/s.log", policy).ok);
    EXPECT_FALSE(check_service_command("/usr/bin/Input-Leaps -f", policy).ok);
}

} // namespace inputleap
