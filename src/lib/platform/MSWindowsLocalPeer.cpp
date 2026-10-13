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
#include <Wtsapi32.h>

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
