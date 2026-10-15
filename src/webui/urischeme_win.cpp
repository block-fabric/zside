// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/urischeme.h>

#include <logging.h>

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace webui {
namespace {
std::wstring Widen(const std::string& text)
{
    if (text.empty()) return {};
    const int n{MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

bool SetString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    HKEY handle;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const bool ok{RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS};
    RegCloseKey(handle);
    return ok;
}

std::wstring GetString(HKEY root, const std::wstring& key)
{
    wchar_t buffer[2048];
    DWORD size{sizeof(buffer)};
    if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

//! The program of a command line ("C:\...\chains-app.exe" "%1").
std::wstring ProgramOf(const std::wstring& command)
{
    int argc{0};
    LPWSTR* argv{CommandLineToArgvW(command.c_str(), &argc)};
    std::wstring out{argv && argc > 0 ? argv[0] : L""};
    if (argv) LocalFree(argv);
    return out;
}
} // namespace

bool RegisterUriScheme(const std::string& scheme, const std::string& description)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    const std::wstring key{L"Software\\Classes\\" + Widen(scheme)};
    const std::wstring command_key{key + L"\\shell\\open\\command"};
    const std::wstring program{ProgramOf(GetString(HKEY_CURRENT_USER, command_key))};
    if (!program.empty()) {
        if (_wcsicmp(program.c_str(), exe) == 0) return true;
        // Another program the user has: kept.
        if (GetFileAttributesW(program.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    }
    const std::wstring quoted{L"\"" + std::wstring{exe} + L"\""};
    const bool ok{SetString(HKEY_CURRENT_USER, key, nullptr, L"URL:" + Widen(description)) &&
                  SetString(HKEY_CURRENT_USER, key, L"URL Protocol", L"") &&
                  SetString(HKEY_CURRENT_USER, key + L"\\DefaultIcon", nullptr, quoted + L",0") &&
                  SetString(HKEY_CURRENT_USER, command_key, nullptr, quoted + L" \"%1\"")};
    if (ok) LogInfo("webui: %s: payment URIs open with this program", scheme);
    else LogWarning("webui: cannot register the %s: URI scheme", scheme);
    return ok;
}

} // namespace webui
