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

#include "platform/MSWindowsLocalPeer.h"

#include "io/filesystem.h"
#include "base/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shlobj.h>
#include <Tlhelp32.h>
#include <Userenv.h>
#include <Wtsapi32.h>

#include <cwchar>
#include <vector>

namespace inputleap {

namespace {

struct Handle {
    HANDLE value = nullptr;
    ~Handle()
    {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    bool ok() const { return value != nullptr && value != INVALID_HANDLE_VALUE; }
};

std::wstring to_wide(const std::string& utf8)
{
    return fs::u8path(utf8).wstring();
}

std::string to_utf8(const std::wstring& wide)
{
    return fs::path(wide).u8string();
}

// the user at the console: their token when running as the system (the service), otherwise
// our own token, as the service then runs as that user for debugging
bool console_user_token(Handle& token)
{
    if (WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token.value)) {
        return true;
    }
    token.value = nullptr;
    return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE,
                            &token.value) != 0;
}

// the user of a token, as a SID inside the returned buffer
bool token_user(HANDLE token, std::vector<BYTE>& buffer)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        return false;
    }
    buffer.resize(size);
    return GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
}

PSID sid_of(std::vector<BYTE>& buffer)
{
    return reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
}

bool is_administrator(HANDLE token)
{
    // membership is checked on an impersonation token
    Handle impersonation;
    if (!DuplicateToken(token, SecurityIdentification, &impersonation.value)) {
        return false;
    }
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &administrators)) {
        return false;
    }
    BOOL member = FALSE;
    bool checked = CheckTokenMembership(impersonation.value, administrators, &member) != 0;
    FreeSid(administrators);
    return checked && member;
}

} // namespace

bool MSWindowsLocalPeer::is_trusted_process(int pid)
{
    if (pid <= 0) {
        return false;
    }
    Handle process;
    process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process.ok()) {
        LOG_DEBUG("cannot open process %d to check who runs it", pid);
        return false;
    }
    Handle token;
    if (!OpenProcessToken(process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &token.value)) {
        return false;
    }
    std::vector<BYTE> user;
    if (!token_user(token.value, user)) {
        return false;
    }
    if (IsWellKnownSid(sid_of(user), WinLocalSystemSid)) {
        // the server or client that the service started
        return true;
    }

    Handle console;
    std::vector<BYTE> console_user;
    if (console_user_token(console) && token_user(console.value, console_user) &&
        EqualSid(sid_of(user), sid_of(console_user))) {
        return true;
    }
    return is_administrator(token.value);
}

std::string MSWindowsLocalPeer::canonical_path(const std::string& path)
{
    if (path.empty()) {
        return "";
    }
    // opening with backup semantics works for folders too; links and junctions are followed
    Handle file;
    file.value = CreateFileW(to_wide(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (!file.ok()) {
        return "";
    }
    std::vector<wchar_t> buffer(1024);
    DWORD length = GetFinalPathNameByHandleW(file.value, buffer.data(), static_cast<DWORD>(buffer.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetFinalPathNameByHandleW(file.value, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (length == 0 || length >= buffer.size()) {
        return "";
    }
    std::wstring result(buffer.data(), length);
    // the result has the \\?\ prefix, or \\?\UNC\ for network paths
    if (result.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        result = L"\\\\" + result.substr(8);
    }
    else if (result.compare(0, 4, L"\\\\?\\") == 0) {
        result = result.substr(4);
    }
    return to_utf8(result);
}

bool MSWindowsLocalPeer::console_user_can_read(const std::string& path)
{
    Handle token;
    if (!console_user_token(token)) {
        return false;
    }
    // open the file as the user; the check applies to every folder on the way as well
    if (!ImpersonateLoggedOnUser(token.value)) {
        LOG_DEBUG("cannot impersonate the console user to check a file");
        return false;
    }
    Handle file;
    file.value = CreateFileW(to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    RevertToSelf();
    return file.ok();
}

std::string MSWindowsLocalPeer::console_user_local_appdata()
{
    Handle token;
    if (!console_user_token(token)) {
        return "";
    }
    PWSTR folder = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, token.value, &folder) != S_OK) {
        return "";
    }
    std::string result = to_utf8(folder);
    CoTaskMemFree(folder);
    return result;
}

bool MSWindowsLocalPeer::running_elevated()
{
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &size);
    if (size == 0) {
        return false;
    }
    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, buffer.data(), size, &size)) {
        return false;
    }
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
    DWORD count = *GetSidSubAuthorityCount(sid);
    if (count == 0) {
        return false;
    }
    return *GetSidSubAuthority(sid, count - 1) >= SECURITY_MANDATORY_HIGH_RID;
}

namespace {

// a primary token of the user at the console: from the session when running as the system,
// otherwise the token of the user's shell in this session
bool console_user_primary_token(Handle& token)
{
    if (WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token.value)) {
        return true;
    }
    token.value = nullptr;

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Handle snapshot;
    snapshot.value = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (!snapshot.ok()) {
        return false;
    }
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.value, &entry); more;
         more = Process32NextW(snapshot.value, &entry)) {
        DWORD process_session = 0;
        if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0 ||
            !ProcessIdToSessionId(entry.th32ProcessID, &process_session) ||
            process_session != session) {
            continue;
        }
        Handle process;
        process.value = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process.ok()) {
            continue;
        }
        Handle shell_token;
        if (!OpenProcessToken(process.value, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
                              &shell_token.value)) {
            continue;
        }
        if (DuplicateTokenEx(shell_token.value, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation,
                             TokenPrimary, &token.value)) {
            return true;
        }
        token.value = nullptr;
    }
    return false;
}

} // namespace

void* MSWindowsLocalPeer::start_as_console_user(const std::string& command_line)
{
    Handle token;
    if (!console_user_primary_token(token)) {
        LOG_DEBUG("cannot get a token of the console user to start a program");
        return nullptr;
    }
    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, token.value, FALSE)) {
        LOG_DEBUG("cannot create the environment for the console user (error %lu)", GetLastError());
        return nullptr;
    }

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    wchar_t desktop[] = L"winsta0\\Default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));

    // CreateProcess may modify the command line
    std::wstring wide = to_wide(command_line);
    std::vector<wchar_t> command(wide.begin(), wide.end());
    command.push_back(L'\0');

    BOOL ok = CreateProcessAsUserW(token.value, nullptr, command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment,
                                   nullptr, &startup, &process);
    DWORD error = GetLastError();
    DestroyEnvironmentBlock(environment);
    if (!ok) {
        LOG_DEBUG("cannot start %s as the console user (error %lu)", command_line.c_str(), error);
        return nullptr;
    }
    CloseHandle(process.hThread);
    return process.hProcess;
}

std::string MSWindowsLocalPeer::program_directory()
{
    std::vector<wchar_t> buffer(1024);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return "";
    }
    return to_utf8(fs::path(std::wstring(buffer.data(), length)).parent_path().wstring());
}

} // namespace inputleap
