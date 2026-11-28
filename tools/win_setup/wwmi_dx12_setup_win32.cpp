// ===========================================================================
//  wwmi_dx12_setup_win32.cpp -- Standalone zero-dependency Windows GUI + CLI
//  Setup & Manager for WWMI-DX12-RT.
//
//  Features:
//    * Double-click GUI (High-DPI Win32 native UI, Chinese/English adaptive)
//    * Headless CLI mode (--cli doctor | install | uninstall | init-wwmi)
//    * Auto-discovers Kuro Official Launcher (CN / Global / Bilibili) & Steam
//      via Registry + libraryfolders.vdf + drive scan.
//    * Auto-discovers existing XXMI Launcher WWMI directory (%APPDATA%, etc.).
//    * 2-file external BasePath mount (writes ONLY dxgi.dll + 2-line ReShade.ini
//      into Client\Binaries\Win64\; refuses to overwrite foreign dxgi.dll).
//    * Safe ledger-tracked uninstall (stages removed bootstrap files into
//      runtime\uninstall-staging\batch-<timestamp>\ with manifest.json).
// ===========================================================================
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include "ledger_json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace {

constexpr const wchar_t* kShippingExe = L"Client-Win64-Shipping.exe";

bool g_zh_ui = false;

bool DetectChineseUi(const std::wstring& cli_lang = {})
{
    auto parse_lang = [](std::wstring v) -> int {
        for (wchar_t& c : v) c = (wchar_t)towlower(c);
        while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
        if (v.rfind(L"en", 0) == 0) return 0;
        if (v.rfind(L"zh", 0) == 0) return 1;
        return -1;
    };
    if (!cli_lang.empty()) {
        const int r = parse_lang(cli_lang);
        if (r >= 0) return r == 1;
    }
    wchar_t env[64]{};
    if (GetEnvironmentVariableW(L"WWMI_SETUP_LANG", env, (DWORD)std::size(env)) > 0) {
        const int r = parse_lang(env);
        if (r >= 0) return r == 1;
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

inline const wchar_t* TrW(const wchar_t* zh, const wchar_t* en) noexcept
{
    return g_zh_ui ? zh : en;
}

inline const char* TrA(const char* zh, const char* en) noexcept
{
    return g_zh_ui ? zh : en;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string JsonEscape(const std::wstring& w)
{
    const std::string s = WideToUtf8(w);
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0xf];
        } else out.push_back((char)c);
    }
    return out;
}

std::wstring TrimWide(std::wstring s)
{
    auto is_ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"'; };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    return s;
}

std::wstring NormWinPath(const fs::path& p)
{
    wchar_t buf[32768]{};
    const DWORD n = GetFullPathNameW(p.c_str(), (DWORD)std::size(buf), buf, nullptr);
    std::wstring s = (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : p.lexically_normal().wstring();
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring LowerWide(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring FoldWindowsLockPath(const std::wstring& s)
{
    if (s.empty()) return {};
    const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr, 0);
    if (needed > 0) {
        std::wstring out((size_t)needed, L'\0');
        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.c_str(), (int)s.size(), out.data(), needed, nullptr, nullptr, 0) == needed)
            return out;
    }
    std::wstring fallback = s;
    for (wchar_t& c : fallback)
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    return fallback;
}

bool WriteUtf8File(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), (std::streamsize)content.size());
    return f.good();
}

std::string ReadUtf8File(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string Hex(const unsigned char* bytes, size_t size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i != size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0f]);
    }
    return out;
}

std::string ReadFileChecked(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("无法读取：" + WideToUtf8(path.wstring()));
    const auto size = f.tellg();
    if (size < 0) throw std::runtime_error("无法读取文件大小：" + WideToUtf8(path.wstring()));
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!f.seekg(0) || !f.read(bytes.data(), static_cast<std::streamsize>(size)))
        throw std::runtime_error("文件读取未完成：" + WideToUtf8(path.wstring()));
    return bytes;
}

std::string Sha256Hex(const std::string& bytes)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("无法初始化 SHA-256");
    unsigned char digest[32]{};
    const auto status = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status != 0) throw std::runtime_error("无法计算 SHA-256");
    return Hex(digest, sizeof(digest));
}

void WinError(const char* action, const fs::path& path, DWORD code = GetLastError())
{
    throw std::runtime_error(std::string(action) + "：" + WideToUtf8(path.wstring()) +
        "（Win32 " + std::to_string(code) + "）");
}

std::string NewToken()
{
    unsigned char token[16]{};
    if (BCryptGenRandom(nullptr, token, sizeof(token), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("无法生成安装事务标识");
    return Hex(token, sizeof(token));
}

bool RegularFileOrMissing(const fs::path& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        WinError("无法检查文件", path, error);
    }
    if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
        throw std::runtime_error(std::string(TrA("拒绝操作非普通文件（含链接/重解析点）：",
            "Refusing to operate on a non-regular file (directory/symlink/reparse point): ")) + WideToUtf8(path.wstring()));
    return true;
}

void WriteNewFile(const fs::path& path, const std::string& bytes)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) WinError("无法创建事务暂存文件", path);
    DWORD written = 0;
    const bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(f);
    const DWORD error = GetLastError();
    const bool closed = CloseHandle(f) != FALSE;
    if (!ok || !closed) WinError("无法写完事务暂存文件", path, ok ? GetLastError() : error);
}

void AtomicWrite(const fs::path& path, const std::string& bytes)
{
    const fs::path temp = path.parent_path() / (L"." + path.filename().wstring() + L"." + Utf8ToWide(NewToken()) + L".tmp");
    WriteNewFile(temp, bytes);
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        WinError("无法提交文件，暂存文件已保留", temp);
}

fs::path CanonicalPath(const fs::path& p);

class NamedMutexLock {
    HANDLE handle_ = nullptr;
    bool owns_ = false;
public:
    NamedMutexLock() = default;
    NamedMutexLock(const std::wstring& name, const fs::path& scope_path, const char* busy_zh, const char* busy_en)
    {
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_) WinError("无法创建安装互斥锁", scope_path);
        const DWORD wait = WaitForSingleObject(handle_, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            owns_ = true;
            return;
        }
        const DWORD err = GetLastError();
        CloseHandle(handle_);
        handle_ = nullptr;
        if (wait == WAIT_TIMEOUT) throw std::runtime_error(TrA(busy_zh, busy_en));
        WinError("无法锁定目标互斥锁", scope_path, err);
    }
    NamedMutexLock(NamedMutexLock&& o) noexcept : handle_(o.handle_), owns_(o.owns_)
    {
        o.handle_ = nullptr;
        o.owns_ = false;
    }
    ~NamedMutexLock()
    {
        if (handle_) {
            if (owns_) ReleaseMutex(handle_);
            CloseHandle(handle_);
        }
    }
    NamedMutexLock(const NamedMutexLock&) = delete;
    NamedMutexLock& operator=(const NamedMutexLock&) = delete;
};

class Win64MutexLock {
    NamedMutexLock inner_;
public:
    explicit Win64MutexLock(const fs::path& win64_dir)
        : inner_(
            L"Local\\WWMI-DX12-RT-Win64-" + Utf8ToWide(Sha256Hex(WideToUtf8(FoldWindowsLockPath(CanonicalPath(win64_dir).wstring()))).substr(0, 24)),
            win64_dir,
            "无法锁定目标 Win64 目录，可能有另一安装/升级/卸载程序正在运行。",
            "Could not lock target Win64 directory; another install/upgrade/uninstall may be running.")
    {}
};

class BaseRootMutexLocks {
    std::vector<NamedMutexLock> locks_;
public:
    explicit BaseRootMutexLocks(std::initializer_list<fs::path> raw_roots)
    {
        std::vector<std::pair<std::wstring, fs::path>> uniq;
        for (const auto& r : raw_roots) {
            if (r.empty()) continue;
            const fs::path canon = CanonicalPath(r);
            const std::wstring key = FoldWindowsLockPath(canon.wstring());
            if (std::none_of(uniq.begin(), uniq.end(), [&](const auto& kv) { return kv.first == key; }))
                uniq.emplace_back(key, canon);
        }
        std::sort(uniq.begin(), uniq.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        locks_.reserve(uniq.size());
        for (const auto& [key, canon] : uniq) {
            const std::wstring name = L"Local\\WWMI-DX12-RT-Base-" + Utf8ToWide(Sha256Hex(WideToUtf8(key)).substr(0, 24));
            locks_.emplace_back(
                name, canon,
                "外部根目录账本正被另一进程锁定，请稍后重试。",
                "External base root ledger is locked by another process; retry shortly.");
        }
    }
};

fs::path SelfBaseRoot()
{
    wchar_t buf[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    fs::path exe_dir = fs::path(std::wstring(buf, n)).parent_path();
    std::error_code ec;
    // An extracted (even incomplete) archive must not fall back to a checkout above it.
    if (fs::is_directory(exe_dir / L"reshade-addons", ec) ||
        fs::is_regular_file(exe_dir / L"BUILD-PROVENANCE.json", ec)) return exe_dir;
    for (auto dir = exe_dir; !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_regular_file(dir / L"scripts\\build.py", ec) &&
            fs::is_regular_file(dir / L"CMakeLists.txt", ec))
            return dir;
        if (dir == dir.parent_path()) break;
    }
    return exe_dir;
}

bool IsGameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) throw std::runtime_error("无法检查游戏进程，请退出游戏后重试");
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool running = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Client-Win64-Shipping.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"Wuthering Waves.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    const DWORD end_error = running ? ERROR_SUCCESS : GetLastError();
    CloseHandle(snap);
    if (!running && end_error != ERROR_NO_MORE_FILES)
        throw std::runtime_error("无法完成游戏进程扫描，请退出游戏后重试");
    return running;
}

fs::path ResolveWin64FromAnyGameDir(const fs::path& input)
{
    if (input.empty()) return {};
    std::error_code ec;
    if (fs::is_regular_file(input / kShippingExe, ec)) return NormWinPath(input);
    for (const fs::path& sub : {
             input / L"Wuthering Waves Game\\Client\\Binaries\\Win64",
             input / L"Client\\Binaries\\Win64",
             input.parent_path() / L"Client\\Binaries\\Win64",
         }) {
        if (fs::is_regular_file(sub / kShippingExe, ec)) return NormWinPath(sub);
    }
    return {};
}

std::vector<fs::path> ParseVdfPaths(const fs::path& vdf_file)
{
    std::vector<fs::path> out;
    const std::string txt = ReadUtf8File(vdf_file);
    if (txt.empty()) return out;
    std::regex re("\"path\"\\s+\"([^\"]+)\"", std::regex::icase);
    for (std::sregex_iterator it(txt.begin(), txt.end(), re), end; it != end; ++it) {
        std::string raw = (*it)[1].str();
        std::string unescaped;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
                unescaped.push_back('\\');
                ++i;
            } else {
                unescaped.push_back(raw[i]);
            }
        }
        out.emplace_back(Utf8ToWide(unescaped));
    }
    return out;
}

std::wstring QueryRegString(HKEY root, const wchar_t* subkey, const wchar_t* value_name)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return {};
    wchar_t buf[2048]{};
    DWORD   sz = sizeof(buf);
    DWORD   type = 0;
    std::wstring res;
    if (RegQueryValueExW(hKey, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        res = TrimWide(buf);
    }
    RegCloseKey(hKey);
    return res;
}

fs::path LedgerGameWin64(const fs::path& base_root);

fs::path AutoDiscoverGameWin64(const fs::path& base_root)
{
    if (const auto hit = LedgerGameWin64(base_root); !hit.empty()) return hit;

    std::vector<fs::path> cands;

    // 2. Registry Uninstall entries (Kuro Official Launcher CN / Global / Bilibili / Epic / WeGame)
    const std::pair<HKEY, const wchar_t*> uninstall_roots[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    auto matches_ww = [](const std::wstring& s) {
        const std::wstring low = LowerWide(s);
        return low.find(L"wuthering") != std::wstring::npos ||
               low.find(L"kuro") != std::wstring::npos ||
               low.find(L"krinstall") != std::wstring::npos ||
               low.find(L"3513350") != std::wstring::npos ||
               low.find(L"\u9e23\u6f6e") != std::wstring::npos;
    };
    for (const auto& [hive, path] : uninstall_roots) {
        HKEY hRoot = nullptr;
        if (RegOpenKeyExW(hive, path, 0, KEY_READ, &hRoot) != ERROR_SUCCESS) continue;
        wchar_t subname[512]{};
        for (DWORD idx = 0;; ++idx) {
            DWORD len = (DWORD)std::size(subname);
            if (RegEnumKeyExW(hRoot, idx, subname, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const std::wstring full_sub = std::wstring(path) + L"\\" + subname;
            const std::wstring disp = QueryRegString(hive, full_sub.c_str(), L"DisplayName");
            if (!matches_ww(subname) && !matches_ww(disp)) continue;
            for (const wchar_t* val_name : { L"InstallLocation", L"InstallPath" }) {
                const std::wstring loc = QueryRegString(hive, full_sub.c_str(), val_name);
                if (!loc.empty()) cands.emplace_back(loc);
            }
            const std::wstring icon = QueryRegString(hive, full_sub.c_str(), L"DisplayIcon");
            if (!icon.empty()) {
                std::wstring p = icon;
                const size_t comma = p.rfind(L',');
                if (comma != std::wstring::npos) p.resize(comma);
                p = TrimWide(p);
                if (!p.empty() && p.front() == L'"' && p.back() == L'"' && p.size() >= 2) p = p.substr(1, p.size() - 2);
                if (!p.empty()) cands.push_back(fs::path(p).parent_path());
            }
        }
        RegCloseKey(hRoot);
    }

    // 3. Steam Registry + libraryfolders.vdf
    for (const auto& [hive, sub, val] : std::vector<std::tuple<HKEY, const wchar_t*, const wchar_t*>>{
             { HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath" },
             { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath" },
         }) {
        const std::wstring sp = QueryRegString(hive, sub, val);
        if (!sp.empty()) {
            const fs::path sroot(sp);
            cands.push_back(sroot / L"steamapps\\common\\Wuthering Waves");
            for (const fs::path& lib : ParseVdfPaths(sroot / L"steamapps\\libraryfolders.vdf"))
                cands.push_back(lib / L"steamapps\\common\\Wuthering Waves");
        }
    }

    // 4. Common fixed-drive paths (C..Z)
    for (wchar_t drv = L'C'; drv <= L'Z'; ++drv) {
        const std::wstring root = std::wstring(1, drv) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        cands.emplace_back(root + L"Wuthering Waves");
        cands.emplace_back(root + L"Wuthering Waves Game");
        cands.emplace_back(root + L"\u9e23\u6f6e");
        cands.emplace_back(root + L"Games\\Wuthering Waves");
        cands.emplace_back(root + L"Games\\\u9e23\u6f6e");
        cands.emplace_back(root + L"Kuro Games\\Wuthering Waves");
        cands.emplace_back(root + L"Program Files\\Wuthering Waves");
        cands.emplace_back(root + L"Program Files\\Kuro Games\\Wuthering Waves");
        cands.emplace_back(root + L"Program Files\\Epic Games\\WutheringWaves");
        cands.emplace_back(root + L"Epic Games\\WutheringWaves");
        cands.emplace_back(root + L"Program Files (x86)\\Steam\\steamapps\\common\\Wuthering Waves");
        cands.emplace_back(root + L"SteamLibrary\\steamapps\\common\\Wuthering Waves");
    }

    for (const fs::path& c : cands) {
        const fs::path hit = ResolveWin64FromAnyGameDir(c);
        if (!hit.empty()) return hit;
    }
    return {};
}

fs::path AutoDiscoverOwnerWwmiDir(const fs::path& win64_hint, const fs::path& base_root);

fs::path AutoDiscoverWwmiDir(const fs::path& base_root, const fs::path& win64_hint = {})
{
    std::error_code ec;
    wchar_t ini_val[4096]{};
    const fs::path local_ini = base_root / L"wwmi_dx12.ini";
    if (fs::is_regular_file(local_ini, ec)) {
        GetPrivateProfileStringW(L"wwmi-dx12", L"wwmi_dir", L"", ini_val, (DWORD)std::size(ini_val), local_ini.c_str());
        const std::wstring trimmed = TrimWide(ini_val);
        if (!trimmed.empty() && fs::is_regular_file(fs::path(trimmed) / L"d3dx.ini", ec))
            return NormWinPath(trimmed);
    }
    if (!win64_hint.empty()) {
        const fs::path owner_wwmi = AutoDiscoverOwnerWwmiDir(win64_hint, base_root);
        if (!owner_wwmi.empty()) return owner_wwmi;
    }

    std::vector<fs::path> cands;
    for (const wchar_t* ev : { L"APPDATA", L"LOCALAPPDATA" }) {
        wchar_t buf[4096]{};
        if (GetEnvironmentVariableW(ev, buf, (DWORD)std::size(buf)) > 0)
            cands.push_back(fs::path(buf) / L"XXMI Launcher\\WWMI");
    }
    cands.push_back(base_root / L"WWMI");
    for (wchar_t drv = L'C'; drv <= L'J'; ++drv) {
        const std::wstring root = std::wstring(1, drv) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        cands.emplace_back(root + L"XXMI\\WWMI");
        cands.emplace_back(root + L"Games\\XXMI\\WWMI");
        cands.emplace_back(root + L"Tools\\XXMI\\WWMI");
    }
    for (const fs::path& c : cands) {
        if (fs::is_regular_file(c / L"d3dx.ini", ec)) return NormWinPath(c);
    }
    return {};
}

bool InitWwmiSkeleton(const fs::path& target_dir, std::wstring& err_out)
{
    std::error_code ec;
    fs::create_directories(target_dir / L"Mods", ec);
    fs::create_directories(target_dir / L"Core", ec);
    fs::create_directories(target_dir / L"ShaderFixes", ec);
    if (ec) {
        err_out = std::wstring(TrW(L"无法创建目录：", L"Failed to create directory: ")) + target_dir.wstring();
        return false;
    }
    const fs::path d3dx = target_dir / L"d3dx.ini";
    if (!fs::exists(d3dx, ec)) {
        const std::string txt =
            "; Standard WWMI directory initialized by WWMI-DX12-RT Setup\r\n"
            "[Include]\r\n"
            "include = Core\\WWMI\\WuWa-Model-Importer.ini\r\n"
            "include_recursive = Mods\r\n"
            "exclude_recursive = DISABLED*\r\n"
            "exclude_recursive = desktop.ini\r\n\r\n"
            "[Hunting]\r\n"
            "hunting = 0\r\n\r\n"
            "[KeyReload]\r\n"
            "Key = no_modifiers VK_F10\r\n"
            "type = reload_config\r\n";
        if (!WriteUtf8File(d3dx, txt)) {
            err_out = std::wstring(TrW(L"无法写入 d3dx.ini：", L"Failed to write d3dx.ini: ")) + d3dx.wstring();
            return false;
        }
    }
    return true;
}

constexpr std::array<const wchar_t*, 2> kBootstrap = {L"dxgi.dll", L"ReShade.ini"};

bool SamePath(const fs::path& a, const fs::path& b)
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

fs::path CanonicalPath(const fs::path& p)
{
    return NormWinPath(fs::weakly_canonical(p));
}

bool InDirectory(const fs::path& path, const fs::path& root)
{
    for (auto p = path; !p.empty(); p = p.parent_path()) {
        if (SamePath(p, root)) return true;
        if (p == p.parent_path()) break;
    }
    return false;
}

bool IsHex(const std::string& str, size_t count)
{
    return str.size() == count && str.find_first_not_of("0123456789abcdef") == std::string::npos;
}

fs::path JournalPath(const fs::path& base)
{
    return base / L"runtime\\install-ledger.json";
}

struct PackageProvenance {
    std::string version = "dev";
    std::string commit;
};

PackageProvenance ReadPackageProvenance(const fs::path& base_root)
{
    PackageProvenance out;
    try {
        const fs::path path = base_root / L"BUILD-PROVENANCE.json";
        if (!RegularFileOrMissing(path)) return out;
        const std::string text = ReadFileChecked(path);
        const auto json = ledger_json::Reader(text).parse();
        if (json.type != ledger_json::Value::Type::Object) return out;
        if (auto it = json.fields.find("version"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            std::string v = it->second.text;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
            if (!v.empty()) out.version = std::move(v);
        }
        if (auto it = json.fields.find("repository_commit"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            out.commit = it->second.text;
        }
    } catch (const std::exception&) {
    }
    return out;
}

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::vector<std::string> prerelease;
};

std::optional<SemVer> ParseSemVer(const std::string& raw)
{
    std::string s = raw;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    static const std::regex re(R"(^[vV]?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$)");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return std::nullopt;
    SemVer v;
    try {
        v.major = std::stoi(m[1].str());
        v.minor = std::stoi(m[2].str());
        v.patch = std::stoi(m[3].str());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (m[4].matched) {
        const std::string pre = m[4].str();
        size_t pos = 0;
        while (pos <= pre.size()) {
            const size_t dot = pre.find('.', pos);
            std::string part = dot == std::string::npos ? pre.substr(pos) : pre.substr(pos, dot - pos);
            if (part.empty()) return std::nullopt;
            v.prerelease.push_back(std::move(part));
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
    }
    return v;
}

std::optional<int> CompareSemVer(const std::string& a, const std::string& b)
{
    const auto pa = ParseSemVer(a);
    const auto pb = ParseSemVer(b);
    if (!pa || !pb) return std::nullopt;
    if (pa->major != pb->major) return pa->major < pb->major ? -1 : 1;
    if (pa->minor != pb->minor) return pa->minor < pb->minor ? -1 : 1;
    if (pa->patch != pb->patch) return pa->patch < pb->patch ? -1 : 1;
    if (pa->prerelease.empty() && pb->prerelease.empty()) return 0;
    if (pa->prerelease.empty()) return 1;
    if (pb->prerelease.empty()) return -1;
    const size_t n = std::min(pa->prerelease.size(), pb->prerelease.size());
    auto is_digits = [](const std::string& t) {
        return !t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    };
    for (size_t i = 0; i < n; ++i) {
        const auto& xa = pa->prerelease[i];
        const auto& xb = pb->prerelease[i];
        if (xa == xb) continue;
        const bool da = is_digits(xa);
        const bool db = is_digits(xb);
        if (da && db) {
            const long long va = std::stoll(xa);
            const long long vb = std::stoll(xb);
            return va < vb ? -1 : 1;
        }
        if (da != db) return da ? -1 : 1;
        return xa < xb ? -1 : 1;
    }
    if (pa->prerelease.size() == pb->prerelease.size()) return 0;
    return pa->prerelease.size() < pb->prerelease.size() ? -1 : 1;
}

struct LedgerInfo {
    enum class State { Missing, Installed, Installing, RollingBack, Uninstalled };
    State state = State::Missing;
    fs::path win64_dir;
    fs::path wwmi_dir;
    std::array<std::string, kBootstrap.size()> sha256;
    std::array<std::string, kBootstrap.size()> previous_sha256;
    std::array<fs::path, kBootstrap.size()> recovery_backup;
    std::string pending_id;
    fs::path previous_owner;
    fs::path rollback_target;
    fs::path recovery_dir;
    std::string installed_version;
    std::string installed_commit;
    std::string upgraded_from;
    std::string migrated_to;
    std::string restored_from;
    std::string restored_pending_id;

    bool active() const { return state == State::Installed || state == State::Installing || state == State::RollingBack; }
    bool pending() const { return state == State::Installing || state == State::RollingBack; }
};

std::wstring FormatInstalledVersionW(const LedgerInfo& ledger)
{
    if (!ledger.installed_version.empty()) return Utf8ToWide(ledger.installed_version);
    return TrW(L"旧版（版本未知）", L"Legacy (version unknown)");
}

fs::path LedgerAbsolutePath(const ledger_json::Value& value)
{
    const auto wide = Utf8ToWide(value.string());
    const fs::path path(wide);
    if (wide.find(L'\0') != std::wstring::npos || !path.is_absolute() ||
        std::find(path.begin(), path.end(), L"..") != path.end())
        throw std::runtime_error("invalid ledger path");
    return path;
}

LedgerInfo ReadLedger(const fs::path& base_root)
{
    const auto path = JournalPath(base_root);
    try {
        LedgerInfo info;
        if (!RegularFileOrMissing(path)) return info;
        const auto text = ReadFileChecked(path);
        if (Utf8ToWide(text).empty()) throw std::runtime_error("invalid UTF-8 ledger");
        const auto json = ledger_json::Reader(text).parse();
        const auto& version = json.at("schema_version");
        if (version.type != ledger_json::Value::Type::Number || version.text != "1")
            throw std::runtime_error("invalid ledger version");
        const auto& state = json.at("state").string();
        if (state == "installed") info.state = LedgerInfo::State::Installed;
        else if (state == "installing") info.state = LedgerInfo::State::Installing;
        else if (state == "rolling_back") info.state = LedgerInfo::State::RollingBack;
        else if (state == "uninstalled") info.state = LedgerInfo::State::Uninstalled;
        else throw std::runtime_error("invalid ledger state");
        info.win64_dir = LedgerAbsolutePath(json.at("win64_dir"));
        if (auto it = json.fields.find("wwmi_dir"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String && !it->second.text.empty()) {
            info.wwmi_dir = LedgerAbsolutePath(it->second);
        }
        if (auto it = json.fields.find("installed_version"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            info.installed_version = it->second.text;
        }
        if (auto it = json.fields.find("installed_commit"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            info.installed_commit = it->second.text;
        }
        if (auto it = json.fields.find("upgraded_from"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            info.upgraded_from = it->second.text;
        }
        if (auto it = json.fields.find("migrated_to"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            info.migrated_to = it->second.text;
        }
        if (auto it = json.fields.find("restored_from"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            info.restored_from = it->second.text;
        }
        if (auto it = json.fields.find("restored_pending_id"); it != json.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            info.restored_pending_id = it->second.text;
        }
        if (info.active()) {
            const auto& records = json.at("win64_files");
            if (records.type != ledger_json::Value::Type::Array || records.elements.size() != kBootstrap.size())
                throw std::runtime_error("incomplete bootstrap inventory");
            for (const auto& item : records.elements) {
                const auto name = Utf8ToWide(item.at("relative").string());
                const auto it = std::find(kBootstrap.begin(), kBootstrap.end(), name);
                if (it == kBootstrap.end() || !SamePath(LedgerAbsolutePath(item.at("path")), info.win64_dir / name))
                    throw std::runtime_error("bootstrap outside Win64 allowlist");
                const size_t i = it - kBootstrap.begin();
                const auto& sha = item.at("sha256").string();
                if (!IsHex(sha, 64) || !info.sha256[i].empty()) throw std::runtime_error("invalid bootstrap digest");
                info.sha256[i] = sha;
                if (info.pending()) {
                    const auto found = item.fields.find("previous_sha256");
                    if (found != item.fields.end()) {
                        info.previous_sha256[i] = found->second.string();
                        if (!IsHex(info.previous_sha256[i], 64)) throw std::runtime_error("invalid previous digest");
                    } else if (info.state == LedgerInfo::State::RollingBack) {
                        throw std::runtime_error("missing previous digest in rolling_back ledger");
                    }
                    const auto rb = item.fields.find("recovery_backup");
                    if (rb != item.fields.end() && rb->second.type == ledger_json::Value::Type::String) {
                        const fs::path rb_path = LedgerAbsolutePath(rb->second);
                        if (!InDirectory(CanonicalPath(rb_path), CanonicalPath(base_root / L"runtime\\recovery")))
                            throw std::runtime_error("recovery backup outside recovery root");
                        info.recovery_backup[i] = rb_path;
                    } else if (info.state == LedgerInfo::State::RollingBack) {
                        throw std::runtime_error("missing recovery backup in rolling_back ledger");
                    }
                }
            }
            if (info.pending()) {
                info.pending_id = json.at("pending_id").string();
                if (!IsHex(info.pending_id, 32)) throw std::runtime_error("invalid transaction ID");
                if (auto rd = json.fields.find("recovery_dir"); rd != json.fields.end() && rd->second.type == ledger_json::Value::Type::String && !rd->second.text.empty()) {
                    const fs::path rd_path = LedgerAbsolutePath(rd->second);
                    if (InDirectory(CanonicalPath(rd_path), CanonicalPath(base_root / L"runtime\\recovery")))
                        info.recovery_dir = rd_path;
                }
                if (info.state == LedgerInfo::State::Installing) {
                    const auto po = json.fields.find("previous_owner");
                    if (po != json.fields.end() && po->second.type == ledger_json::Value::Type::String && !po->second.text.empty()) {
                        info.previous_owner = LedgerAbsolutePath(po->second);
                    }
                } else {
                    info.rollback_target = LedgerAbsolutePath(json.at("rollback_target"));
                }
            }
        }
        return info;
    } catch (const std::exception& e) {
        throw std::runtime_error(g_zh_ui
            ? ("安装账本损坏或无法读取：" + WideToUtf8(path.wstring()) +
               "。请从备份恢复账本；无备份时先人工核对原 Win64 引导文件归属。（" + e.what() + "）")
            : ("Install ledger is corrupted or unreadable: " + WideToUtf8(path.wstring()) +
               ". Restore it from backup or inspect your Win64 bootstrap files manually. (" + e.what() + ")"));
    }
}

fs::path LedgerGameWin64(const fs::path& base_root)
{
    try {
        const auto ledger = ReadLedger(base_root);
        return ResolveWin64FromAnyGameDir(ledger.win64_dir);
    } catch (const std::exception&) {
        return {}; // Inspect/install/uninstall report the ledger error directly.
    }
}

using Hashes = std::array<std::string, kBootstrap.size()>;

bool InDirectory(const fs::path& child, const fs::path& parent);

fs::path VerifiedRecoveryBackupPath(const fs::path& bkp_raw, const std::string& prev_sha, const fs::path& base_root)
{
    if (bkp_raw.empty() || prev_sha.empty()) return {};
    try {
        const fs::path bkp = CanonicalPath(bkp_raw);
        if (!base_root.empty()) {
            const fs::path rec_root = CanonicalPath(base_root / L"runtime\\recovery");
            if (!InDirectory(bkp, rec_root)) return {};
        }
        if (!RegularFileOrMissing(bkp)) return {};
        if (Sha256Hex(ReadFileChecked(bkp)) != prev_sha) return {};
        return bkp;
    } catch (const std::exception&) {
        return {};
    }
}

Hashes OwnedBootstrap(const fs::path& win64_dir, const LedgerInfo& ledger, const fs::path& base_root = {})
{
    Hashes owned{};
    for (size_t i = 0; i < kBootstrap.size(); ++i) {
        const auto path = win64_dir / kBootstrap[i];
        if (!RegularFileOrMissing(path)) continue;
        const auto name = WideToUtf8(kBootstrap[i]);
        if (!ledger.active())
            throw std::runtime_error(g_zh_ui
                ? ("Win64/" + name + " 已存在，但没有对应的活动账本，拒绝覆盖（若已有其他 ReShade/DXGI，请先备份移走原 Win64\\" + name + " 与 ReShade.ini 后重试；WWMI-DX12-RT 需使用配对宿主，第三方 .addon64 如 RenoDX 可放入外部 reshade-addons\\ 或在外部 ReShade.ini 设 LoadGameDirAddons=1 共存。若为旧版换目录升级，请先在旧目录卸载，或覆盖原目录并保留 runtime\\install-ledger.json 后重装）")
                : ("Win64/" + name + " already exists without an active WWMI-DX12-RT ledger; refusing to overwrite. "
                   "If you already use ReShade/DXGI, back up and move Win64\\" + name + " and Win64\\ReShade.ini first, then retry Install. "
                   "WWMI-DX12-RT requires its paired dxgi.dll; to keep third-party .addon64 add-ons (e.g. RenoDX), place them in <ExternalFolder>\\reshade-addons\\ or set LoadGameDirAddons=1 under [ADDON] in <ExternalFolder>\\ReShade.ini. "
                   "If upgrading from another folder, uninstall from the old folder first or extract over it while keeping runtime\\install-ledger.json."));
        owned[i] = Sha256Hex(ReadFileChecked(path));
        std::string allowed_prev;
        if (ledger.pending() && !ledger.previous_sha256[i].empty()) {
            if (ledger.state == LedgerInfo::State::RollingBack || !ledger.recovery_backup[i].empty() || !ledger.previous_owner.empty()) {
                if (!VerifiedRecoveryBackupPath(ledger.recovery_backup[i], ledger.previous_sha256[i], base_root).empty())
                    allowed_prev = ledger.previous_sha256[i];
            } else {
                allowed_prev = ledger.previous_sha256[i];
            }
        }
        if (owned[i] != ledger.sha256[i] && (allowed_prev.empty() || owned[i] != allowed_prev))
            throw std::runtime_error(g_zh_ui
                ? ("Win64/" + name + " 哈希与账本不一致，拒绝修改")
                : ("Win64/" + name + " hash does not match the install ledger; refusing to modify."));
    }
    return owned;
}

enum class MountState { NotInstalled, Installed, Drifted, Installing, RollingBack, UpgradeReady, ActiveElsewhere, Conflict };
enum class InstallActionKind { Install, Upgrade, Rollback, Switch, Refresh, Repair, ActiveElsewhere, Conflict };

struct VerifiedWin64Owner {
    fs::path base_root;
    LedgerInfo ledger;
    Hashes owned_hashes;
    std::wstring raw_stub_base_path;
};

struct InstallActionInfo {
    InstallActionKind action = InstallActionKind::Install;
    MountState mount_state = MountState::NotInstalled;
    std::wstring message;
    std::wstring source_version;
    std::wstring target_version;
    fs::path owner_base_root;
};

std::string TrimAscii(std::string s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string LowerAscii(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool ParseIniSectionName(const std::string& line, std::string& sec_out)
{
    const std::string t = TrimAscii(line);
    if (t.empty() || t.front() != '[') return false;
    const size_t close = t.find(']');
    std::string inner = (close == std::string::npos) ? t : t.substr(0, close);
    while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t' || inner.front() == '[' || inner.front() == ']'))
        inner.erase(inner.begin());
    while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t' || inner.back() == '[' || inner.back() == ']'))
        inner.pop_back();
    sec_out = std::move(inner);
    return true;
}

std::wstring ParseWin64StubBasePath(const std::string& raw_text, bool strict = false)
{
    std::string text = raw_text;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    std::istringstream iss(text);
    bool in_install = false;
    std::wstring found;
    for (std::string line; std::getline(iss, line);) {
        const std::string t = TrimAscii(line);
        if (t.empty() || t.front() == ';' || t.front() == '#' || t.front() == '/') continue;
        std::string sec;
        if (ParseIniSectionName(line, sec)) {
            if (strict && sec != "INSTALL") return {};
            in_install = (sec == "INSTALL");
            continue;
        }
        const size_t eq = line.find('=');
        if (in_install && eq != std::string::npos && TrimAscii(line.substr(0, eq)) == "BasePath") {
            const std::wstring val = TrimWide(Utf8ToWide(line.substr(eq + 1)));
            if (val.empty() || (strict && !found.empty())) return {};
            found = val;
            if (!strict) return found;
            continue;
        }
        if (strict) return {};
    }
    return found;
}

bool IsValidAbsoluteWinPath(const std::wstring& w)
{
    if (w.size() < 3 || w.find(L'\0') != std::wstring::npos || w.find(L'\r') != std::wstring::npos || w.find(L'\n') != std::wstring::npos)
        return false;
    // Reject UNC / device paths (\\server\share, //server/share, \\?\..., \\.\...) BEFORE any filesystem probe.
    if ((w[0] == L'\\' || w[0] == L'/') && (w[1] == L'\\' || w[1] == L'/'))
        return false;
    const wchar_t drv = w[0];
    const bool is_alpha = (drv >= L'A' && drv <= L'Z') || (drv >= L'a' && drv <= L'z');
    if (!is_alpha || w[1] != L':' || (w[2] != L'\\' && w[2] != L'/'))
        return false;
    const std::wstring root = std::wstring(1, drv) + L":\\";
    const UINT dtype = GetDriveTypeW(root.c_str());
    if (dtype == DRIVE_REMOTE || dtype == DRIVE_UNKNOWN || dtype == DRIVE_NO_ROOT_DIR)
        return false;
    const fs::path p(w);
    if (!p.is_absolute()) return false;
    return std::find(p.begin(), p.end(), L"..") == p.end();
}

bool DetectVerifiedWin64Owner(
    const fs::path& win64_dir,
    const fs::path& current_base,
    const fs::path& migrate_from,
    VerifiedWin64Owner& out,
    std::string& reject_reason)
{
    reject_reason.clear();
    const fs::path win64_canon = CanonicalPath(win64_dir);
    const fs::path cur_base_canon = CanonicalPath(current_base);

    std::array<bool, kBootstrap.size()> present{};
    for (size_t i = 0; i < kBootstrap.size(); ++i)
        present[i] = RegularFileOrMissing(win64_canon / kBootstrap[i]);
    if (!present[0] && !present[1]) return false;

    std::wstring raw_base;
    fs::path parsed_base;
    bool stub_is_strict = false;
    if (present[1]) {
        try {
            const std::string stub_raw = ReadFileChecked(win64_canon / kBootstrap[1]);
            raw_base = ParseWin64StubBasePath(stub_raw, true);
            if (!raw_base.empty()) {
                stub_is_strict = true;
                if (IsValidAbsoluteWinPath(raw_base)) parsed_base = CanonicalPath(fs::path(raw_base));
            } else {
                raw_base = ParseWin64StubBasePath(stub_raw, false);
            }
        } catch (const std::exception&) {
        }
    }

    fs::path candidate;
    if (!migrate_from.empty()) {
        if (!IsValidAbsoluteWinPath(migrate_from.wstring())) {
            reject_reason = TrA("--migrate-from 必须是本地绝对路径，拒绝远程/UNC 路径。",
                "--migrate-from must be a local absolute path; remote/UNC paths are rejected.");
            return false;
        }
        if (present[1] && !stub_is_strict) {
            reject_reason = TrA("Win64\\ReShade.ini 不是 WWMI-DX12-RT 的 2 行引导文件（可能为第三方 ReShade 配置），拒绝接管。",
                "Win64\\ReShade.ini is not a minimal WWMI-DX12-RT bootstrap stub; refusing takeover.");
            return false;
        }
        const fs::path explicit_a = CanonicalPath(migrate_from);
        if (!parsed_base.empty() && !SamePath(parsed_base, explicit_a)) {
            reject_reason = g_zh_ui
                ? ("Win64\\ReShade.ini 指向 " + WideToUtf8(parsed_base.wstring()) +
                   "，与 --migrate-from 指定的 " + WideToUtf8(explicit_a.wstring()) + " 不一致，拒绝接管。")
                : ("Win64\\ReShade.ini points to " + WideToUtf8(parsed_base.wstring()) +
                   ", which does not match --migrate-from " + WideToUtf8(explicit_a.wstring()) + "; refusing takeover.");
            return false;
        }
        candidate = explicit_a;
    } else if (!parsed_base.empty() && stub_is_strict) {
        candidate = parsed_base;
    } else {
        if (!raw_base.empty() && stub_is_strict) {
            reject_reason = g_zh_ui
                ? ("Win64\\ReShade.ini 中的 BasePath=" + WideToUtf8(raw_base) + " 不是受信任的本地绝对路径（拒绝 UNC/远程盘），拒绝盲目覆盖。")
                : ("BasePath=" + WideToUtf8(raw_base) + " in Win64\\ReShade.ini is not a valid local path (UNC/remote paths rejected); refusing to overwrite.");
        }
        return false;
    }

    if (SamePath(candidate, cur_base_canon)) return false;

    std::error_code ec;
    if (!fs::is_directory(candidate, ec)) {
        reject_reason = g_zh_ui
            ? ("Win64\\ReShade.ini 指向的旧目录不存在：" + WideToUtf8(candidate.wstring()) + "；拒绝盲目覆盖。")
            : ("Previous directory referenced by Win64\\ReShade.ini does not exist: " + WideToUtf8(candidate.wstring()) + "; refusing to overwrite.");
        return false;
    }

    LedgerInfo ledger_a;
    try {
        ledger_a = ReadLedger(candidate);
    } catch (const std::exception& e) {
        reject_reason = g_zh_ui
            ? ("无法验证旧目录 " + WideToUtf8(candidate.wstring()) + " 的安装账本，拒绝覆盖：" + e.what())
            : ("Could not verify install ledger in " + WideToUtf8(candidate.wstring()) + "; refusing to overwrite: " + e.what());
        return false;
    }
    if (ledger_a.state == LedgerInfo::State::Missing) {
        reject_reason = g_zh_ui
            ? ("旧目录 " + WideToUtf8(candidate.wstring()) + " 缺少有效的 runtime\\install-ledger.json，拒绝盲目覆盖。")
            : ("Previous directory " + WideToUtf8(candidate.wstring()) + " has no runtime\\install-ledger.json; refusing to overwrite.");
        return false;
    }
    if (ledger_a.state == LedgerInfo::State::Installing) {
        reject_reason = g_zh_ui
            ? ("旧目录 " + WideToUtf8(candidate.wstring()) + " 上次安装未完成（state=installing）；请先在旧目录完成修复或卸载后再升级。")
            : ("Previous installation in " + WideToUtf8(candidate.wstring()) + " was interrupted (state=installing); finish repairing or uninstalling it first.");
        return false;
    }
    if (ledger_a.state != LedgerInfo::State::Installed) {
        reject_reason = g_zh_ui
            ? ("旧目录 " + WideToUtf8(candidate.wstring()) + " 的账本非已安装状态，拒绝接管。")
            : ("Ledger in " + WideToUtf8(candidate.wstring()) + " is not in installed state; refusing takeover.");
        return false;
    }
    if (!SamePath(CanonicalPath(ledger_a.win64_dir), win64_canon)) {
        reject_reason = g_zh_ui
            ? ("旧目录 " + WideToUtf8(candidate.wstring()) + " 的账本绑定在 " + WideToUtf8(ledger_a.win64_dir.wstring()) + "，与目标 Win64 不匹配，拒绝接管。")
            : ("Ledger in " + WideToUtf8(candidate.wstring()) + " is bound to " + WideToUtf8(ledger_a.win64_dir.wstring()) + ", which does not match target Win64.");
        return false;
    }
    if (!present[0] || !present[1]) {
        const std::string missing = !present[0] ? "dxgi.dll" : "ReShade.ini";
        reject_reason = g_zh_ui
            ? ("旧安装 " + WideToUtf8(candidate.wstring()) + " 的 Win64 引导文件不完整（缺少 " + missing + "）；请先在旧目录修复或卸载后再升级。")
            : ("Existing install in " + WideToUtf8(candidate.wstring()) + " is missing Win64 bootstrap file (" + missing + "); repair or uninstall it in the old folder first.");
        return false;
    }

    Hashes actual{};
    for (size_t i = 0; i < kBootstrap.size(); ++i) {
        actual[i] = Sha256Hex(ReadFileChecked(win64_canon / kBootstrap[i]));
        if (actual[i] != ledger_a.sha256[i]) {
            const std::string name = WideToUtf8(kBootstrap[i]);
            reject_reason = g_zh_ui
                ? ("Win64/" + name + " 哈希与旧目录 " + WideToUtf8(candidate.wstring()) + " 的账本不一致（可能已被修改或替换），拒绝覆盖。")
                : ("Win64/" + name + " hash does not match ledger in " + WideToUtf8(candidate.wstring()) + " (possibly modified or replaced); refusing to overwrite.");
            return false;
        }
    }

    const fs::path host_dll_a = candidate / L"host\\ReShade64-game-forward.dll";
    if (!RegularFileOrMissing(host_dll_a) || Sha256Hex(ReadFileChecked(host_dll_a)) != actual[0]) {
        reject_reason = g_zh_ui
            ? ("旧目录 " + WideToUtf8(candidate.wstring()) + " 缺少匹配的 host\\ReShade64-game-forward.dll，无法验证 Win64\\dxgi.dll 归属，拒绝覆盖。")
            : ("Previous directory " + WideToUtf8(candidate.wstring()) + " lacks a matching host\\ReShade64-game-forward.dll; refusing takeover.");
        return false;
    }

    out.base_root = candidate;
    out.ledger = std::move(ledger_a);
    out.owned_hashes = actual;
    out.raw_stub_base_path = std::move(raw_base);
    return true;
}

InstallActionKind ClassifyTakeoverKind(
    const VerifiedWin64Owner& owner,
    const fs::path& current_base,
    const LedgerInfo& current_ledger,
    const std::string& package_version)
{
    if (!owner.ledger.upgraded_from.empty() && SamePath(fs::path(Utf8ToWide(owner.ledger.upgraded_from)), current_base))
        return InstallActionKind::Rollback;
    if (!current_ledger.migrated_to.empty() && SamePath(fs::path(Utf8ToWide(current_ledger.migrated_to)), owner.base_root))
        return InstallActionKind::Rollback;
    const auto cmp = CompareSemVer(owner.ledger.installed_version, package_version);
    if (cmp && *cmp < 0) return InstallActionKind::Upgrade;
    if (cmp && *cmp > 0) return InstallActionKind::Rollback;
    if (owner.ledger.installed_version.empty() && ParseSemVer(package_version).has_value())
        return InstallActionKind::Upgrade;
    return InstallActionKind::Switch;
}

std::wstring FormatTakeoverMessage(const VerifiedWin64Owner& owner, InstallActionKind kind, const std::string& pkg_ver)
{
    const std::wstring old_ver = FormatInstalledVersionW(owner.ledger);
    const std::wstring new_ver = Utf8ToWide(pkg_ver);
    if (kind == InstallActionKind::Rollback) {
        return g_zh_ui
            ? (L"检测到已验证的当前版本 " + old_ver + L"（" + owner.base_root.wstring() + L"）；可直接回滚至此版本 (" + old_ver + L" -> " + new_ver + L")。")
            : (L"Verified active install " + old_ver + L" (" + owner.base_root.wstring() + L") detected; ready to roll back (" + old_ver + L" -> " + new_ver + L").");
    }
    if (kind == InstallActionKind::Upgrade) {
        return g_zh_ui
            ? (L"检测到已验证的旧版本 " + old_ver + L"（" + owner.base_root.wstring() + L"）；可直接并排升级 (" + old_ver + L" -> " + new_ver + L")。")
            : (L"Verified previous install " + old_ver + L" (" + owner.base_root.wstring() + L") detected; ready for side-by-side upgrade (" + old_ver + L" -> " + new_ver + L").");
    }
    return g_zh_ui
        ? (L"检测到已验证的另一目录安装 " + old_ver + L"（" + owner.base_root.wstring() + L"）；可直接并排切换至当前目录。")
        : (L"Verified install " + old_ver + L" (" + owner.base_root.wstring() + L") detected; ready for side-by-side switch.");
}

MountState ClassifyMount(
    const fs::path& win64,
    const LedgerInfo& ledger,
    const fs::path& current_base,
    const fs::path& migrate_from,
    const std::string& pkg_ver,
    std::wstring& msg,
    Hashes& owned,
    std::optional<VerifiedWin64Owner>& owner_out)
{
    owner_out.reset();
    if (ledger.active() && !SamePath(CanonicalPath(ledger.win64_dir), win64)) {
        msg = g_zh_ui
            ? (L"账本仍绑定 " + ledger.win64_dir.wstring() + L"；请先卸载旧游戏目录，再安装到新目录。")
            : (L"Ledger is still bound to " + ledger.win64_dir.wstring() + L". Uninstall the previous game directory first.");
        return MountState::ActiveElsewhere;
    }
    if (ledger.active()) {
        try {
            owned = OwnedBootstrap(win64, ledger, current_base);
        } catch (const std::exception&) {
            if (ledger.state == LedgerInfo::State::Installed) {
                VerifiedWin64Owner owner;
                std::string reject;
                if (DetectVerifiedWin64Owner(win64, current_base, migrate_from, owner, reject)) {
                    const auto kind = ClassifyTakeoverKind(owner, current_base, ledger, pkg_ver);
                    msg = FormatTakeoverMessage(owner, kind, pkg_ver);
                    owned = owner.owned_hashes;
                    owner_out = std::move(owner);
                    return MountState::UpgradeReady;
                }
            }
            throw;
        }
        if (ledger.state == LedgerInfo::State::RollingBack) {
            msg = TrW(L"上次版本回滚未完成；请重试【回滚旧版】完成恢复。",
                      L"Previous rollback was interrupted; retry Rollback to finish restoring the old folder.");
            return MountState::RollingBack;
        }
        if (ledger.state == LedgerInfo::State::Installing) {
            msg = TrW(L"上次安装未完成；可在同一目录重试安装或执行卸载。",
                      L"Previous installation was interrupted; retry Install in the same directory or run Uninstall.");
            return MountState::Installing;
        }
        if (owned[0].empty() || owned[1].empty()) {
            msg = TrW(L"账本引导文件有缺失，其余文件归属已确认；可重新安装恢复。",
                      L"Some managed bootstrap files are missing (remaining files verified); re-run Install to repair.");
            return MountState::Drifted;
        }
        msg = TrW(L"已正常挂载 2 文件 BasePath 外部引导桥。", L"Active (2-file external BasePath mount).");
        return MountState::Installed;
    }

    VerifiedWin64Owner owner;
    std::string reject;
    if (DetectVerifiedWin64Owner(win64, current_base, migrate_from, owner, reject)) {
        const auto kind = ClassifyTakeoverKind(owner, current_base, ledger, pkg_ver);
        msg = FormatTakeoverMessage(owner, kind, pkg_ver);
        owned = owner.owned_hashes;
        owner_out = std::move(owner);
        return MountState::UpgradeReady;
    }
    if (!reject.empty()) throw std::runtime_error(reject);

    owned = OwnedBootstrap(win64, ledger, current_base);
    msg = TrW(L"Win64 未挂载引导文件。", L"Not mounted in Win64.");
    return MountState::NotInstalled;
}

InstallActionInfo DescribeInstallAction(
    const fs::path& win64_dir,
    const fs::path& base_root,
    const fs::path& migrate_from = {})
{
    InstallActionInfo info;
    const auto pkg = ReadPackageProvenance(base_root);
    info.target_version = Utf8ToWide(pkg.version);
    try {
        const fs::path win64 = CanonicalPath(win64_dir);
        const fs::path base = CanonicalPath(base_root);
        const auto ledger = ReadLedger(base);
        Hashes owned;
        std::optional<VerifiedWin64Owner> owner;
        info.mount_state = ClassifyMount(win64, ledger, base, migrate_from, pkg.version, info.message, owned, owner);
        if (info.mount_state == MountState::UpgradeReady && owner) {
            info.action = ClassifyTakeoverKind(*owner, base, ledger, pkg.version);
            info.source_version = FormatInstalledVersionW(owner->ledger);
            info.owner_base_root = owner->base_root;
        } else if (info.mount_state == MountState::Installed) {
            info.action = InstallActionKind::Refresh;
            info.source_version = FormatInstalledVersionW(ledger);
            info.owner_base_root = base;
        } else if (info.mount_state == MountState::Drifted || info.mount_state == MountState::Installing) {
            info.action = InstallActionKind::Repair;
            info.source_version = FormatInstalledVersionW(ledger);
            info.owner_base_root = base;
        } else if (info.mount_state == MountState::RollingBack) {
            info.action = InstallActionKind::Rollback;
            info.source_version = FormatInstalledVersionW(ledger);
            info.owner_base_root = base;
        } else if (info.mount_state == MountState::ActiveElsewhere) {
            info.action = InstallActionKind::ActiveElsewhere;
            info.source_version = FormatInstalledVersionW(ledger);
        } else {
            info.action = InstallActionKind::Install;
        }
    } catch (const std::exception& e) {
        info.mount_state = MountState::Conflict;
        info.action = InstallActionKind::Conflict;
        info.message = Utf8ToWide(e.what());
    }
    return info;
}

MountState InspectMountState(const fs::path& win64_dir, const fs::path& base_root, std::wstring& msg_out, const fs::path& migrate_from = {})
{
    const auto info = DescribeInstallAction(win64_dir, base_root, migrate_from);
    msg_out = info.message;
    return info.mount_state;
}

fs::path ResolveOwnerWwmiDir(const VerifiedWin64Owner& owner)
{
    std::error_code ec;
    const fs::path ini = owner.base_root / L"wwmi_dx12.ini";
    if (fs::is_regular_file(ini, ec)) {
        wchar_t val[4096]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"wwmi_dir", L"", val, (DWORD)std::size(val), ini.c_str());
        const std::wstring trimmed = TrimWide(val);
        if (!trimmed.empty() && IsValidAbsoluteWinPath(trimmed) && fs::is_directory(trimmed, ec))
            return CanonicalPath(trimmed);
    }
    if (!owner.ledger.wwmi_dir.empty() && fs::is_directory(owner.ledger.wwmi_dir, ec))
        return CanonicalPath(owner.ledger.wwmi_dir);
    return {};
}

fs::path AutoDiscoverOwnerWwmiDir(const fs::path& win64_hint, const fs::path& base_root)
{
    try {
        const fs::path win64 = ResolveWin64FromAnyGameDir(win64_hint);
        if (win64.empty()) return {};
        VerifiedWin64Owner owner;
        std::string reject;
        if (!DetectVerifiedWin64Owner(win64, base_root, {}, owner, reject)) return {};
        const fs::path prev = ResolveOwnerWwmiDir(owner);
        std::error_code ec;
        if (!prev.empty() && fs::is_regular_file(prev / L"d3dx.ini", ec)) return NormWinPath(prev);
    } catch (const std::exception&) {
    }
    return {};
}

std::wstring DetectWwmiCoreVersion(const fs::path& wwmi_dir)
{
    const std::string text = ReadUtf8File(wwmi_dir / L"Core\\WWMI\\WuWa-Model-Importer.ini");
    if (text.empty()) return {};
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"(data\s*=\s*\"WWMI\s+([0-9]+(?:\.[0-9]+)+))", std::regex::icase)))
        return Utf8ToWide(m[1].str());
    if (std::regex_search(text, m, std::regex(R"(global\s+\$wwmi_version\s*=\s*([0-9]+(?:\.[0-9]+)*))", std::regex::icase)))
        return Utf8ToWide(m[1].str());
    return {};
}

std::vector<std::wstring> ScanThirdPartyWin64Addons(const fs::path& win64_dir)
{
    std::vector<std::wstring> found;
    std::error_code ec;
    if (!fs::is_directory(win64_dir, ec)) return found;
    for (const fs::directory_entry& e : fs::directory_iterator(win64_dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::wstring ext = LowerWide(e.path().extension().wstring());
        if (ext != L".addon" && ext != L".addon64") continue;
        const std::wstring name = e.path().filename().wstring();
        if (_wcsicmp(name.c_str(), L"wwmi_dx12.addon64") == 0 ||
            _wcsicmp(name.c_str(), L"zz_wwmi_rt_pack.addon64") == 0)
            continue;
        found.push_back(name);
    }
    std::sort(found.begin(), found.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    return found;
}

std::string MergeExternalReShadeIni(const fs::path& ini_path)
{
    const std::string kDefault =
        "[INSTALL]\r\nLogging=0\r\n\r\n[ADDON]\r\nAddonPath=reshade-addons\r\n\r\n[OVERLAY]\r\nTutorialProgress=4\r\n";
    if (!RegularFileOrMissing(ini_path)) return kDefault;
    std::string existing = ReadFileChecked(ini_path);
    if (existing.size() >= 3 &&
        static_cast<unsigned char>(existing[0]) == 0xEF &&
        static_cast<unsigned char>(existing[1]) == 0xBB &&
        static_cast<unsigned char>(existing[2]) == 0xBF) {
        existing.erase(0, 3);
    }
    auto trim_ascii = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        return s;
    };
    auto lower_ascii = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return s;
    };
    auto parse_section = [&](const std::string& line, std::string& sec_out) -> bool {
        // Match ReShade's exact case-sensitive section parsing (ini_file.cpp).
        const std::string t = trim_ascii(line);
        if (t.empty() || t.front() != '[') return false;
        const size_t close = t.find(']');
        std::string inner = (close == std::string::npos) ? t : t.substr(0, close);
        while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t' || inner.front() == '[' || inner.front() == ']'))
            inner.erase(inner.begin());
        while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t' || inner.back() == '[' || inner.back() == ']'))
            inner.pop_back();
        sec_out = std::move(inner);
        return true;
    };
    auto split_reshade_tokens = [&](const std::string& val) -> std::vector<std::string> {
        // Decode ReShade's ",," escaped literal comma while splitting on single ',' and ';'.
        std::vector<std::string> comma_parts;
        std::string buf;
        for (size_t i = 0; i < val.size(); ++i) {
            if (val[i] == ',') {
                if (i + 1 < val.size() && val[i + 1] == ',') {
                    buf.push_back(',');
                    ++i;
                    continue;
                }
                comma_parts.push_back(std::move(buf));
                buf.clear();
                continue;
            }
            buf.push_back(val[i]);
        }
        comma_parts.push_back(std::move(buf));

        std::vector<std::string> out;
        for (const std::string& part : comma_parts) {
            size_t pos = 0;
            while (pos <= part.size()) {
                const size_t sep = part.find(';', pos);
                const std::string tok = trim_ascii(sep == std::string::npos ? part.substr(pos) : part.substr(pos, sep - pos));
                if (!tok.empty()) out.push_back(tok);
                if (sep == std::string::npos) break;
                pos = sep + 1;
            }
        }
        return out;
    };
    auto escape_commas = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            out.push_back(c);
            if (c == ',') out.push_back(',');
        }
        return out;
    };
    if (trim_ascii(existing).empty()) return kDefault;

    const std::string eol = existing.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    std::istringstream iss(existing);
    for (std::string line; std::getline(iss, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    bool in_addon = false;
    int addon_header_idx = -1;
    std::vector<int> addon_path_indices;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = trim_ascii(lines[i]);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' || trimmed.front() == '/')
            continue;
        std::string sec_name;
        if (parse_section(lines[i], sec_name)) {
            in_addon = (sec_name == "ADDON");
            if (in_addon && addon_header_idx < 0) addon_header_idx = (int)i;
            continue;
        }
        if (in_addon) {
            const size_t eq = lines[i].find('=');
            if (eq != std::string::npos && trim_ascii(lines[i].substr(0, eq)) == "AddonPath") {
                addon_path_indices.push_back((int)i);
            }
        }
    }
    if (!addon_path_indices.empty()) {
        std::vector<std::string> extras;
        std::vector<std::string> seen_norms;
        for (int idx : addon_path_indices) {
            const size_t eq = lines[(size_t)idx].find('=');
            const std::string val = trim_ascii(lines[(size_t)idx].substr(eq + 1));
            for (const std::string& raw_tok : split_reshade_tokens(val)) {
                std::string norm = lower_ascii(raw_tok);
                for (char& c : norm) if (c == '\\') c = '/';
                while (!norm.empty() && norm.back() == '/') norm.pop_back();
                if (norm != "reshade-addons" &&
                    std::find(seen_norms.begin(), seen_norms.end(), norm) == seen_norms.end()) {
                    seen_norms.push_back(std::move(norm));
                    extras.push_back(escape_commas(raw_tok));
                }
            }
        }
        const int first_idx = addon_path_indices.front();
        std::string merged = "reshade-addons";
        for (const std::string& extra : extras)
            merged += (!extra.empty() && extra.front() == ',' ? ", " : ",") + extra;
        lines[(size_t)first_idx] = "AddonPath=" + merged;
        for (size_t k = addon_path_indices.size(); k-- > 1;) {
            lines.erase(lines.begin() + addon_path_indices[k]);
        }
    } else if (addon_header_idx >= 0) {
        lines.insert(lines.begin() + addon_header_idx + 1, "AddonPath=reshade-addons");
    } else {
        if (!lines.empty() && !trim_ascii(lines.back()).empty()) lines.emplace_back();
        lines.emplace_back("[ADDON]");
        lines.emplace_back("AddonPath=reshade-addons");
    }
    std::ostringstream out;
    for (const std::string& l : lines) out << l << eol;
    return out.str();
}

std::vector<std::wstring> ScanReShadeIniMigrationWarnings(const std::string& ini_text, const fs::path& old_base)
{
    std::vector<std::wstring> warnings;
    std::vector<std::string> seen;
    std::string old_norm = LowerAscii(WideToUtf8(CanonicalPath(old_base).wstring()));
    for (char& c : old_norm) if (c == '\\') c = '/';
    while (!old_norm.empty() && old_norm.back() == '/') old_norm.pop_back();

    auto is_path_key = [](const std::string& k_low) {
        return k_low == "presetpath" || k_low == "effectsearchpaths" || k_low == "texturesearchpaths" ||
            k_low == "intermediatecachepath" || k_low == "screenshotpath" || k_low == "extraaddonpaths" ||
            k_low == "addonpath";
    };
    auto is_relative_token = [](std::string t) {
        t = TrimAscii(t);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
        if (t.empty()) return false;
        std::string norm = LowerAscii(t);
        for (char& c : norm) if (c == '\\') c = '/';
        while (!norm.empty() && norm.back() == '/') norm.pop_back();
        if (norm == "reshade-addons" || norm == ".") return false;
        if (t.front() == '/' || t.front() == '\\' || t.front() == '%') return false;
        if (t.size() >= 2 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z')) && t[1] == ':') return false;
        if (std::all_of(t.begin(), t.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return false;
        return true;
    };

    std::istringstream iss(ini_text);
    std::string cur_section;
    for (std::string line; std::getline(iss, line);) {
        const std::string t = TrimAscii(line);
        if (t.empty() || t.front() == ';' || t.front() == '#' || t.front() == '/') continue;
        std::string sec;
        if (ParseIniSectionName(line, sec)) {
            cur_section = std::move(sec);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimAscii(line.substr(0, eq));
        const std::string val = TrimAscii(line.substr(eq + 1));
        if (val.empty()) continue;
        std::string v_norm = LowerAscii(val);
        for (char& c : v_norm) if (c == '\\') c = '/';
        if (!old_norm.empty() && v_norm.find(old_norm) != std::string::npos) {
            const std::string tag = "abs:" + cur_section + ":" + key;
            if (std::find(seen.begin(), seen.end(), tag) == seen.end()) {
                seen.push_back(tag);
                warnings.push_back(g_zh_ui
                    ? (L"外部 ReShade.ini 中 [" + Utf8ToWide(cur_section) + L"] " + Utf8ToWide(key) +
                       L" 仍包含旧目录路径（" + old_base.wstring() + L"），未自动替换，请按需检查。")
                    : (L"External ReShade.ini [" + Utf8ToWide(cur_section) + L"] " + Utf8ToWide(key) +
                       L" still references old folder (" + old_base.wstring() + L"); left unmodified — review if needed."));
            }
            continue;
        }
        if (is_path_key(LowerAscii(key))) {
            std::vector<std::string> rels;
            std::string buf;
            std::vector<std::string> parts;
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == ',') {
                    if (i + 1 < val.size() && val[i + 1] == ',') { buf.push_back(','); ++i; continue; }
                    parts.push_back(std::move(buf)); buf.clear(); continue;
                }
                buf.push_back(val[i]);
            }
            parts.push_back(std::move(buf));
            for (const auto& part : parts) {
                size_t pos = 0;
                while (pos <= part.size()) {
                    const size_t sep = part.find(';', pos);
                    const std::string tok = TrimAscii(sep == std::string::npos ? part.substr(pos) : part.substr(pos, sep - pos));
                    if (is_relative_token(tok)) rels.push_back(tok);
                    if (sep == std::string::npos) break;
                    pos = sep + 1;
                }
            }
            if (!rels.empty()) {
                const std::string tag = "rel:" + cur_section + ":" + key;
                if (std::find(seen.begin(), seen.end(), tag) == seen.end()) {
                    seen.push_back(tag);
                    std::wstring joined;
                    for (size_t i = 0; i < rels.size() && i < 3; ++i) {
                        if (i) joined += L", ";
                        joined += Utf8ToWide(rels[i]);
                    }
                    warnings.push_back(g_zh_ui
                        ? (L"外部 ReShade.ini 中 [" + Utf8ToWide(cur_section) + L"] " + Utf8ToWide(key) +
                           L" 包含相对路径（" + joined + L"）；若对应文件仍在旧目录，请手动复制到新目录。")
                        : (L"External ReShade.ini [" + Utf8ToWide(cur_section) + L"] " + Utf8ToWide(key) +
                           L" uses relative path(s) (" + joined + L"); copy those files from the old folder if needed."));
                }
            }
        }
    }
    return warnings;
}

std::wstring DecodeIniBytesToWide(const std::string& bytes)
{
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const size_t count = (bytes.size() - 2) / sizeof(wchar_t);
        std::wstring out(count, L'\0');
        if (count) memcpy(out.data(), bytes.data() + 2, count * sizeof(wchar_t));
        return out;
    }
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        const size_t count = (bytes.size() - 2) / 2;
        std::wstring out(count, L'\0');
        for (size_t i = 0; i < count; ++i) {
            const unsigned char hi = static_cast<unsigned char>(bytes[2 + i * 2]);
            const unsigned char lo = static_cast<unsigned char>(bytes[3 + i * 2]);
            out[i] = static_cast<wchar_t>((hi << 8) | lo);
        }
        return out;
    }
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return Utf8ToWide(bytes.substr(3));
    }
    return Utf8ToWide(bytes);
}

std::string EncodeWideUtf16LeBomCrlf(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size() + 32);
    out.push_back(L'\ufeff');
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            out += L"\r\n";
        } else if (text[i] == L'\n') {
            out += L"\r\n";
        } else {
            out.push_back(text[i]);
        }
    }
    return std::string(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(wchar_t));
}

bool PathWithinWideBase(const std::wstring& raw_val, const fs::path& base)
{
    if (raw_val.empty() || base.empty()) return false;
    std::wstring v = TrimWide(raw_val);
    std::wstring b = CanonicalPath(base).wstring();
    for (wchar_t& c : v) if (c == L'/') c = L'\\';
    for (wchar_t& c : b) if (c == L'/') c = L'\\';
    while (v.size() > 3 && v.back() == L'\\') v.pop_back();
    while (b.size() > 3 && b.back() == L'\\') b.pop_back();
    const std::wstring vl = LowerWide(v);
    const std::wstring bl = LowerWide(b);
    return vl == bl || (vl.size() > bl.size() && vl.compare(0, bl.size(), bl) == 0 && vl[bl.size()] == L'\\');
}

std::string MergeWwmiDx12Ini(
    const fs::path& source_ini,
    const fs::path& wwmi_dir,
    const fs::path& runtime_dir,
    const fs::path& old_base)
{
    const std::wstring kDefault =
        L"; Managed by WWMI-DX12-RT Setup\r\n[wwmi-dx12]\r\nwwmi_dir=" +
        wwmi_dir.wstring() + L"\r\nskin_mode=base\r\nruntime_dir=" + runtime_dir.wstring() +
        L"\r\nmax_logs=10\r\ntex_struct_fallback=1\r\ndebug=0\r\nlog_level=1\r\n";
    if (source_ini.empty() || !RegularFileOrMissing(source_ini)) return EncodeWideUtf16LeBomCrlf(kDefault);
    std::wstring text = DecodeIniBytesToWide(ReadFileChecked(source_ini));
    if (!text.empty() && text.front() == L'\ufeff') text.erase(text.begin());
    if (TrimWide(text).empty()) return EncodeWideUtf16LeBomCrlf(kDefault);

    std::vector<std::wstring> lines;
    std::wistringstream iss(text);
    for (std::wstring line; std::getline(iss, line);) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(std::move(line));
    }

    auto trim_ws = [](std::wstring s) {
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r')) s.erase(s.begin());
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r')) s.pop_back();
        return s;
    };

    bool in_wwmi = false;
    int wwmi_header_idx = -1;
    int wwmi_section_end = -1;
    bool seen_wwmi = false, seen_runtime = false, seen_skin = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring t = trim_ws(lines[i]);
        if (t.empty() || t.front() == L';' || t.front() == L'#' || t.front() == L'/') continue;
        if (t.front() == L'[') {
            if (in_wwmi && wwmi_section_end < 0) wwmi_section_end = (int)i;
            const size_t close = t.find(L']');
            std::wstring sec = close == std::wstring::npos ? t.substr(1) : t.substr(1, close - 1);
            sec = LowerWide(trim_ws(sec));
            in_wwmi = (sec == L"wwmi-dx12");
            if (in_wwmi && wwmi_header_idx < 0) wwmi_header_idx = (int)i;
            continue;
        }
        if (in_wwmi) {
            const size_t eq = lines[i].find(L'=');
            if (eq == std::wstring::npos) continue;
            const std::wstring k = LowerWide(trim_ws(lines[i].substr(0, eq)));
            const std::wstring v = trim_ws(lines[i].substr(eq + 1));
            if (k == L"wwmi_dir") {
                lines[i] = L"wwmi_dir=" + wwmi_dir.wstring();
                seen_wwmi = true;
            } else if (k == L"runtime_dir") {
                if (v.empty() || (!old_base.empty() && PathWithinWideBase(v, old_base)))
                    lines[i] = L"runtime_dir=" + runtime_dir.wstring();
                seen_runtime = true;
            } else if (k == L"skin_mode") {
                seen_skin = true;
            }
        }
    }

    std::vector<std::wstring> missing;
    if (!seen_wwmi) missing.push_back(L"wwmi_dir=" + wwmi_dir.wstring());
    if (!seen_skin) missing.push_back(L"skin_mode=base");
    if (!seen_runtime) missing.push_back(L"runtime_dir=" + runtime_dir.wstring());

    if (wwmi_header_idx >= 0) {
        const size_t pos = wwmi_section_end >= 0 ? (size_t)wwmi_section_end : lines.size();
        lines.insert(lines.begin() + pos, missing.begin(), missing.end());
    } else {
        if (!lines.empty() && !trim_ws(lines.back()).empty()) lines.emplace_back();
        lines.emplace_back(L"[wwmi-dx12]");
        lines.emplace_back(L"wwmi_dir=" + wwmi_dir.wstring());
        lines.emplace_back(L"skin_mode=base");
        lines.emplace_back(L"runtime_dir=" + runtime_dir.wstring());
        lines.emplace_back(L"max_logs=10");
        lines.emplace_back(L"tex_struct_fallback=1");
        lines.emplace_back(L"debug=0");
        lines.emplace_back(L"log_level=1");
    }

    std::wostringstream out;
    for (const auto& l : lines) out << l << L"\n";
    return EncodeWideUtf16LeBomCrlf(out.str());
}

bool WwmiDirHasContent(const fs::path& wwmi_dir)
{
    std::error_code ec;
    if (!fs::is_regular_file(wwmi_dir / L"d3dx.ini", ec)) return false;
    for (const wchar_t* sub : { L"Core", L"Mods" }) {
        const fs::path d = wwmi_dir / sub;
        if (!fs::is_directory(d, ec)) continue;
        auto it = fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
        if (!ec && it != fs::directory_iterator()) return true;
    }
    return false;
}

fs::path ResolveOldRuntimeDir(const fs::path& old_base)
{
    std::error_code ec;
    const fs::path ini = old_base / L"wwmi_dx12.ini";
    if (fs::is_regular_file(ini, ec)) {
        wchar_t val[4096]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"runtime_dir", L"", val, (DWORD)std::size(val), ini.c_str());
        const std::wstring trimmed = TrimWide(val);
        if (!trimmed.empty() && fs::is_directory(trimmed, ec))
            return CanonicalPath(trimmed);
    }
    return CanonicalPath(old_base / L"runtime");
}

std::vector<std::wstring> MigrateCacheFiles(const fs::path& old_runtime, const fs::path& new_runtime)
{
    std::vector<std::wstring> copied;
    std::error_code ec;
    const fs::path old_cache = old_runtime / L"cache";
    const fs::path new_cache = new_runtime / L"cache";
    if (!fs::is_directory(old_cache, ec) || SamePath(CanonicalPath(old_cache), CanonicalPath(new_cache)))
        return copied;
    fs::create_directories(new_cache, ec);
    for (const auto& e : fs::directory_iterator(old_cache, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::wstring ext = LowerWide(e.path().extension().wstring());
        if (ext != L".cache" && ext != L".texlook" && ext != L".lodcache" && ext != L".bin") continue;
        try {
            if (!RegularFileOrMissing(e.path())) continue;
        } catch (const std::exception&) {
            continue;
        }
        const fs::path dst = new_cache / e.path().filename();
        const auto src_time = fs::last_write_time(e.path(), ec);
        if (ec) continue;
        if (fs::exists(dst, ec)) {
            const auto dst_time = fs::last_write_time(dst, ec);
            if (!ec && dst_time >= src_time) continue;
        }
        AtomicWrite(dst, ReadFileChecked(e.path()));
        fs::last_write_time(dst, src_time, ec);
        copied.push_back(e.path().filename().wstring());
    }
    std::sort(copied.begin(), copied.end());
    return copied;
}

std::vector<std::wstring> MigrateThirdPartyAddons(
    const fs::path& old_base,
    const fs::path& new_base,
    std::vector<std::wstring>& warnings)
{
    std::vector<std::wstring> copied;
    std::error_code ec;
    const fs::path old_dir = old_base / L"reshade-addons";
    const fs::path new_dir = new_base / L"reshade-addons";
    if (!fs::is_directory(old_dir, ec) || SamePath(CanonicalPath(old_dir), CanonicalPath(new_dir)))
        return copied;
    fs::create_directories(new_dir, ec);
    std::vector<std::wstring> existing_lower;
    for (const auto& e : fs::directory_iterator(new_dir, fs::directory_options::skip_permission_denied, ec)) {
        if (e.is_regular_file(ec)) existing_lower.push_back(LowerWide(e.path().filename().wstring()));
    }
    for (const auto& e : fs::directory_iterator(old_dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (LowerWide(e.path().extension().wstring()) != L".addon64") continue;
        const std::wstring name = e.path().filename().wstring();
        const std::wstring low = LowerWide(name);
        if (low == L"wwmi_dx12.addon64" || low == L"zz_wwmi_rt_pack.addon64") continue;
        try {
            if (!RegularFileOrMissing(e.path())) continue;
        } catch (const std::exception&) {
            continue;
        }
        if (std::find(existing_lower.begin(), existing_lower.end(), low) != existing_lower.end()) {
            warnings.push_back(g_zh_ui
                ? (L"第三方插件文件名冲突（" + name + L"）：已保留新目录中的现有文件，未从旧目录覆盖。")
                : (L"Third-party add-on filename conflict (" + name + L"): kept existing file in new folder instead of overwriting."));
            continue;
        }
        AtomicWrite(new_dir / name, ReadFileChecked(e.path()));
        existing_lower.push_back(low);
        copied.push_back(name);
    }
    std::sort(copied.begin(), copied.end());
    return copied;
}

fs::path CreateRecoveryBackup(
    const fs::path& win64,
    const Hashes& owned,
    const fs::path& base,
    const std::string& token,
    std::array<fs::path, kBootstrap.size()>& backup_paths)
{
    const fs::path recovery_dir = base / L"runtime\\recovery" / Utf8ToWide(token);
    fs::create_directories(recovery_dir);
    std::ostringstream json;
    json << "{\n  \"pending_id\": \"" << token << "\",\n  \"win64_dir\": \"" << JsonEscape(win64.wstring())
         << "\",\n  \"entries\": [\n";
    bool first = true;
    for (size_t i = 0; i < kBootstrap.size(); ++i) {
        if (owned[i].empty()) continue;
        const fs::path src = win64 / kBootstrap[i];
        const fs::path dst = recovery_dir / kBootstrap[i];
        const std::string bytes = ReadFileChecked(src);
        WriteNewFile(dst, bytes);
        backup_paths[i] = dst;
        if (!first) json << ",\n";
        first = false;
        json << "    { \"relative\": \"" << WideToUtf8(kBootstrap[i])
             << "\", \"original_path\": \"" << JsonEscape(src.wstring())
             << "\", \"backup_path\": \"" << JsonEscape(dst.wstring())
             << "\", \"sha256\": \"" << Sha256Hex(bytes) << "\" }";
    }
    json << "\n  ]\n}\n";
    AtomicWrite(recovery_dir / L"manifest.json", json.str());
    return recovery_dir;
}

std::wstring MarkPreviousOwnerMigrated(const fs::path& old_base, const fs::path& new_base)
{
    try {
        const auto old_ledger = ReadLedger(old_base);
        if (old_ledger.state == LedgerInfo::State::Missing) return {};
        std::ostringstream out;
        out << "{\n  \"schema_version\": 1,\n  \"state\": \"uninstalled\",\n"
            << "  \"platform_kind\": \"windows\",\n"
            << "  \"base_root\": \"" << JsonEscape(old_base.wstring()) << "\",\n"
            << "  \"win64_dir\": \"" << JsonEscape(old_ledger.win64_dir.wstring()) << "\",\n";
        if (!old_ledger.wwmi_dir.empty())
            out << "  \"wwmi_dir\": \"" << JsonEscape(old_ledger.wwmi_dir.wstring()) << "\",\n";
        if (!old_ledger.installed_version.empty())
            out << "  \"installed_version\": \"" << JsonEscape(Utf8ToWide(old_ledger.installed_version)) << "\",\n";
        if (!old_ledger.installed_commit.empty())
            out << "  \"installed_commit\": \"" << JsonEscape(Utf8ToWide(old_ledger.installed_commit)) << "\",\n";
        out << "  \"migrated_to\": \"" << JsonEscape(new_base.wstring()) << "\"\n}\n";
        AtomicWrite(JournalPath(old_base), out.str());
        return {};
    } catch (const std::exception& e) {
        return g_zh_ui
            ? (L"新版本已提交生效，但无法更新旧目录账本（" + old_base.wstring() + L"）：" + Utf8ToWide(e.what()))
            : (L"New version committed, but could not update previous ledger (" + old_base.wstring() + L"): " + Utf8ToWide(e.what()));
    }
}

int CountRabbitFxInstances(const fs::path& wwmi_dir)
{
    std::error_code ec;
    const fs::path mods = wwmi_dir / L"Mods";
    if (!fs::is_directory(mods, ec)) return 0;
    int count = 0;
    auto walk = [&](auto& self, const fs::path& cur, int depth) -> void {
        if (depth > 6) return;
        for (const fs::directory_entry& e : fs::directory_iterator(cur, fs::directory_options::skip_permission_denied, ec)) {
            const std::wstring name = e.path().filename().wstring();
            if (_wcsnicmp(name.c_str(), L"DISABLED", 8) == 0) continue;
            if (e.is_directory(ec)) self(self, e.path(), depth + 1);
            else if (e.is_regular_file(ec) && _wcsicmp(name.c_str(), L"RabbitFX.ini") == 0) ++count;
        }
    };
    walk(walk, mods, 1);
    return count;
}

fs::path StageFilesToBackup(const std::vector<fs::path>& files, const fs::path& base_root, const char* label)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const auto staging = base_root / L"runtime\\uninstall-staging";
    fs::create_directories(staging);
    const auto batch = staging / (std::wstring(L"batch-") + stamp + L"-" + Utf8ToWide(label) + L"-" + Utf8ToWide(NewToken()));
    if (!fs::create_directory(batch)) throw std::runtime_error("暂存批次已存在");

    std::vector<fs::path> destinations;
    std::ostringstream json;
    json << "{\n  \"label\": \"" << label << "\",\n  \"entries\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto dst = batch / (std::to_wstring(i) + L"_" + files[i].filename().wstring());
        destinations.push_back(dst);
        if (i) json << ",\n";
        json << "    { \"original_path\": \"" << JsonEscape(files[i].wstring())
             << "\", \"staged_path\": \"" << JsonEscape(dst.wstring()) << "\" }";
    }
    json << "\n  ]\n}\n";
    const auto manifest = batch / L"manifest.json";
    AtomicWrite(manifest, json.str());
    for (size_t i = 0; i < files.size(); ++i) {
        if (!MoveFileExW(files[i].c_str(), destinations[i].c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            throw std::runtime_error("暂存失败：" + WideToUtf8(files[i].wstring()) + "（Win32 " +
                std::to_string(error) + "）；已保留恢复清单 " + WideToUtf8(manifest.wstring()));
        }
        if (RegularFileOrMissing(files[i]) || !RegularFileOrMissing(destinations[i]))
            throw std::runtime_error("暂存移动未完成；已保留恢复清单 " + WideToUtf8(manifest.wstring()));
    }
    return batch;
}

using Copies = std::vector<std::pair<fs::path, fs::path>>;

Copies PlanPayloadCopies(const fs::path& base)
{
    const auto stage5 = base / L"payload\\artifacts\\stage5";
    const bool development = fs::is_directory(stage5);
    const auto addons = base / L"reshade-addons";
    Copies copies;
    auto require = [&](const fs::path& source, const fs::path& destination) {
        if (!RegularFileOrMissing(source) || fs::file_size(source) == 0)
            throw std::runtime_error("缺少或为空的运行时组件：" + WideToUtf8(source.wstring()) + "；请重新构建或完整解压。");
        if (development) copies.emplace_back(source, destination);
    };
    const fs::path host_name = L"ReShade64-game-forward.dll";
    const auto host = base / L"host" / host_name;
    require(development ? stage5 / host_name : host, host);
    for (const auto* name : {L"wwmi_dx12.addon64", L"zz_wwmi_rt_pack.addon64"})
        require(development ? stage5 / name : addons / name, addons / name);
    for (const auto* name : {L"dxcompiler.dll", L"dxil.dll"}) {
        const auto rel = fs::path(L"wwmi_dx12\\dxc") / name;
        require((development ? stage5 : addons) / rel, addons / rel);
    }
    const std::pair<const wchar_t*, std::vector<const wchar_t*>> shaders[] = {
        {L"wwmi_dx12", {L"Bc7Half.cso", L"BufCopy.cso", L"IbVbHash.cso"}},
        {L"rt-pack-data", {L"CopyFinalPositions.cso", L"NativeDiscoveryCopy.cso", L"PatchInstances.cso",
            L"PatchSbt.cso", L"ProbeRtInstanceRows.cso", L"ReadRtRaygenId.cso", L"ShapePositions.cso",
            L"ShapeTangents.cso", L"SkinPositions.cso", L"SkinTangents.cso"}},
    };
    for (const auto& [dir, names] : shaders) {
        const auto source_dir = (development ? stage5 : addons) / dir;
        const auto attrs = GetFileAttributesW(source_dir.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("缺少有效的着色器目录：" + WideToUtf8(source_dir.wstring()));
        for (const auto* name : names)
            require(source_dir / name, addons / dir / name);
        if (development) {
            for (const auto& entry : fs::directory_iterator(source_dir)) {
                if (entry.is_regular_file() &&
                    std::find(names.begin(), names.end(), entry.path().filename()) == names.end())
                    require(entry.path(), addons / dir / entry.path().filename());
            }
        }
    }
    return copies;
}

void CheckBasePath(const fs::path& base, const fs::path& win64, const fs::path& wwmi)
{
    const auto path = base.wstring();
    if (std::any_of(path.begin(), path.end(), [](wchar_t c) { return c == L',' || c < 32 || (c >= 127 && c <= 159); }))
        throw std::runtime_error("BasePath 不得包含英文逗号或控制字符（ReShade INI 会拆分或截断路径）");
    const auto binaries = win64.parent_path();
    const auto client = binaries.parent_path();
    const auto game = SamePath(win64.filename(), L"Win64") && SamePath(binaries.filename(), L"Binaries") &&
        SamePath(client.filename(), L"Client") ? client.parent_path() : win64;
    if (InDirectory(base, game))
        throw std::runtime_error("BasePath 必须位于游戏目录之外：" + WideToUtf8(game.wstring()));
    if (InDirectory(base, wwmi))
        throw std::runtime_error("BasePath 必须位于关联 WWMI 目录之外：" + WideToUtf8(wwmi.wstring()));
    for (const auto* dir : {L"runtime", L"runtime\\logs", L"runtime\\cache\\shaders", L"runtime\\seed",
             L"runtime\\recovery", L"runtime\\uninstall-staging", L"host", L"reshade-addons\\wwmi_dx12\\dxc",
             L"reshade-addons\\rt-pack-data", L"seed"}) {
        const auto location = CanonicalPath(base / dir);
        if (!InDirectory(location, base) || InDirectory(location, wwmi))
            throw std::runtime_error("拒绝通过外部运行时目录写入其他目录：" + WideToUtf8((base / dir).wstring()));
    }
}

fs::path PendingPath(const fs::path& win64, const std::string& token, const wchar_t* name)
{
    return win64 / (L".wwmi-" + Utf8ToWide(token) + L"-" + name + L".tmp");
}

std::vector<fs::path> PendingFiles(const LedgerInfo& ledger)
{
    std::vector<fs::path> files;
    if (ledger.pending()) {
        for (const auto* name : kBootstrap) {
            const auto temp = PendingPath(ledger.win64_dir, ledger.pending_id, name);
            if (RegularFileOrMissing(temp)) files.push_back(temp);
        }
    }
    return files;
}

fs::path RestoredSourceRoot(const LedgerInfo& ledger, const fs::path& base_root)
{
    if (ledger.restored_from.empty() || ledger.restored_pending_id.empty()) return {};
    const std::wstring w = Utf8ToWide(ledger.restored_from);
    if (!IsValidAbsoluteWinPath(w)) return {};
    const fs::path src = CanonicalPath(fs::path(w));
    return SamePath(src, base_root) ? fs::path{} : src;
}

void EnsureRestoreSourceFinalized(const fs::path& base_root, const LedgerInfo& ledger)
{
    const fs::path src = RestoredSourceRoot(ledger, base_root);
    if (src.empty()) return;
    const DWORD attrs = GetFileAttributesW(src.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return;
        WinError("无法检查回滚来源目录", src, err);
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
    const auto src_led = ReadLedger(src);
    if (src_led.state == LedgerInfo::State::RollingBack &&
        src_led.pending_id == ledger.restored_pending_id &&
        SamePath(CanonicalPath(src_led.rollback_target), base_root)) {
        throw std::runtime_error(g_zh_ui
            ? ("检测到来源目录 " + WideToUtf8(src.wstring()) + " 的回滚收尾尚未完成，请先在该目录重试回滚完成后再操作当前目录。")
            : ("Rollback finalization in " + WideToUtf8(src.wstring()) + " is still pending; retry Rollback there first before modifying this folder."));
    }
}

bool PerformInstall(
    const fs::path& raw_game_dir,
    const fs::path& raw_wwmi_dir,
    const fs::path& raw_base,
    std::wstring& msg_out,
    const fs::path& migrate_from = {})
{
    try {
        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再安装。",
                "Wuthering Waves is currently running. Close the game and retry."));
        const auto provided_win64 = ResolveWin64FromAnyGameDir(raw_game_dir);
        if (provided_win64.empty())
            throw std::runtime_error(TrA("无效的游戏目录（未找到 Client-Win64-Shipping.exe）。",
                "Invalid game directory (Client-Win64-Shipping.exe not found)."));
        const auto win64 = CanonicalPath(provided_win64);
        const auto base = CanonicalPath(raw_base);
        const auto pkg_prov = ReadPackageProvenance(base);

        auto select_effective_wwmi = [&](const std::optional<VerifiedWin64Owner>& owner) -> fs::path {
            std::error_code ec;
            fs::path req = raw_wwmi_dir.empty() ? fs::path{} : CanonicalPath(raw_wwmi_dir);
            if (owner) {
                const fs::path old_wwmi = ResolveOwnerWwmiDir(*owner);
                if (!old_wwmi.empty() && WwmiDirHasContent(old_wwmi) && !InDirectory(base, old_wwmi)) {
                    if (req.empty() || !fs::is_regular_file(req / L"d3dx.ini", ec) ||
                        (SamePath(req, CanonicalPath(base / L"WWMI")) && !WwmiDirHasContent(req))) {
                        return old_wwmi;
                    }
                }
            }
            return req;
        };

        auto preflight = [&](Hashes& owned_out, MountState& state_out, std::optional<VerifiedWin64Owner>& owner_out) {
            auto ledger = ReadLedger(base);
            std::wstring msg;
            state_out = ClassifyMount(win64, ledger, base, migrate_from, pkg_prov.version, msg, owned_out, owner_out);
            if (state_out == MountState::ActiveElsewhere || state_out == MountState::RollingBack)
                throw std::runtime_error(WideToUtf8(msg));
            return ledger;
        };

        Hashes pre_owned;
        MountState pre_state = MountState::NotInstalled;
        std::optional<VerifiedWin64Owner> pre_owner;
        const auto pre_ledger = preflight(pre_owned, pre_state, pre_owner);
        const fs::path pre_wwmi = select_effective_wwmi(pre_owner);
        std::error_code wwmi_ec;
        if (pre_wwmi.empty() || !fs::is_regular_file(pre_wwmi / L"d3dx.ini", wwmi_ec))
            throw std::runtime_error(TrA("无效的 WWMI 目录（未找到 d3dx.ini）。请选择已有 WWMI 或点击【新建骨架】。",
                "Invalid WWMI directory (d3dx.ini not found). Select an existing WWMI folder or click [Init Skeleton]."));
        CheckBasePath(base, win64, pre_wwmi);
        if (!SamePath(win64, provided_win64)) CheckBasePath(base, provided_win64, pre_wwmi);
        const auto copies = PlanPayloadCopies(base);

        // Lock scope: canonical Win64 mutex + base_root mutex; re-verify Win64 stub & ledgers inside the locks.
        const fs::path pre_restore_src = RestoredSourceRoot(pre_ledger, base);
        const Win64MutexLock lock(win64);
        const BaseRootMutexLocks self_base_lock({ base, pre_restore_src });
        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再安装。",
                "Wuthering Waves is currently running. Close the game and retry."));
        Hashes owned;
        MountState mount_state = MountState::NotInstalled;
        std::optional<VerifiedWin64Owner> verified_owner;
        const auto old_ledger = preflight(owned, mount_state, verified_owner);
        const fs::path cur_restore_src = RestoredSourceRoot(old_ledger, base);
        const fs::path owner_restore_src = verified_owner ? RestoredSourceRoot(verified_owner->ledger, verified_owner->base_root) : fs::path{};
        const BaseRootMutexLocks prev_base_lock({
            verified_owner ? verified_owner->base_root : fs::path{},
            (!cur_restore_src.empty() && !SamePath(cur_restore_src, pre_restore_src)) ? cur_restore_src : fs::path{},
            (!owner_restore_src.empty() && !SamePath(owner_restore_src, base) && !SamePath(owner_restore_src, pre_restore_src)) ? owner_restore_src : fs::path{},
        });
        EnsureRestoreSourceFinalized(base, old_ledger);
        if (verified_owner) EnsureRestoreSourceFinalized(verified_owner->base_root, verified_owner->ledger);
        const fs::path wwmi = select_effective_wwmi(verified_owner);
        if (wwmi.empty() || !fs::is_regular_file(wwmi / L"d3dx.ini", wwmi_ec))
            throw std::runtime_error(TrA("无效的 WWMI 目录（未找到 d3dx.ini）。请选择已有 WWMI 或点击【新建骨架】。",
                "Invalid WWMI directory (d3dx.ini not found). Select an existing WWMI folder or click [Init Skeleton]."));
        CheckBasePath(base, win64, wwmi);
        if (!SamePath(win64, provided_win64)) CheckBasePath(base, provided_win64, wwmi);

        const auto abandoned = PendingFiles(old_ledger);
        if (!abandoned.empty()) StageFilesToBackup(abandoned, base, "interrupted-install");

        InstallActionKind action_kind = InstallActionKind::Install;
        if (verified_owner) action_kind = ClassifyTakeoverKind(*verified_owner, base, old_ledger, pkg_prov.version);
        else if (mount_state == MountState::Installed) action_kind = InstallActionKind::Refresh;
        else if (mount_state == MountState::Drifted || mount_state == MountState::Installing) action_kind = InstallActionKind::Repair;

        fs::path previous_owner_root;
        if (verified_owner) previous_owner_root = verified_owner->base_root;
        else if (old_ledger.state == LedgerInfo::State::Installing && !old_ledger.previous_owner.empty())
            previous_owner_root = old_ledger.previous_owner;
        std::string preserved_upgraded_from = !previous_owner_root.empty()
            ? WideToUtf8(previous_owner_root.wstring())
            : old_ledger.upgraded_from;

        // ---- Step 1: Prepare B completely BEFORE mutating Win64 ----
        std::vector<std::wstring> migration_warnings;
        std::vector<std::wstring> migrated_cache;
        std::vector<std::wstring> migrated_addons;

        for (const auto& [source, target] : copies) {
            fs::create_directories(target.parent_path());
            RegularFileOrMissing(target);
            if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) WinError("同步运行时组件失败", target);
        }
        const auto runtime = base / L"runtime";
        for (const auto* dir : { L"logs", L"cache\\shaders", L"seed", L"recovery" }) fs::create_directories(runtime / dir);
        fs::create_directories(base / L"seed");

        const fs::path reshade_src = RegularFileOrMissing(base / L"ReShade.ini")
            ? (base / L"ReShade.ini")
            : (verified_owner && RegularFileOrMissing(verified_owner->base_root / L"ReShade.ini")
                ? (verified_owner->base_root / L"ReShade.ini") : (base / L"ReShade.ini"));
        const std::string merged_reshade = MergeExternalReShadeIni(reshade_src);
        AtomicWrite(base / L"ReShade.ini", merged_reshade);
        if (verified_owner) {
            const auto rs_warns = ScanReShadeIniMigrationWarnings(merged_reshade, verified_owner->base_root);
            migration_warnings.insert(migration_warnings.end(), rs_warns.begin(), rs_warns.end());
        }

        fs::path wwmi_ini_src;
        fs::path old_base_for_ini;
        if (RegularFileOrMissing(base / L"wwmi_dx12.ini")) {
            wwmi_ini_src = base / L"wwmi_dx12.ini";
            old_base_for_ini = base;
        } else if (verified_owner && RegularFileOrMissing(verified_owner->base_root / L"wwmi_dx12.ini")) {
            wwmi_ini_src = verified_owner->base_root / L"wwmi_dx12.ini";
            old_base_for_ini = verified_owner->base_root;
        }
        AtomicWrite(base / L"wwmi_dx12.ini", MergeWwmiDx12Ini(wwmi_ini_src, wwmi, runtime, old_base_for_ini));

        if (verified_owner) {
            migrated_cache = MigrateCacheFiles(ResolveOldRuntimeDir(verified_owner->base_root), runtime);
            migrated_addons = MigrateThirdPartyAddons(verified_owner->base_root, base, migration_warnings);
        }

        // ---- Step 2: Transactional 2-file Win64 swap with recovery manifest & auto-rollback ----
        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再安装。",
                "Wuthering Waves is currently running. Close the game and retry."));

        if (verified_owner) {
            VerifiedWin64Owner re_owner;
            std::string reject;
            if (!DetectVerifiedWin64Owner(win64, base, migrate_from, re_owner, reject) ||
                !SamePath(re_owner.base_root, verified_owner->base_root)) {
                throw std::runtime_error(!reject.empty() ? reject : TrA(
                    "升级期间 Win64 文件状态发生变化，已中止写入以防损坏。",
                    "Win64 bootstrap files changed before swap; aborted to avoid corruption."));
            }
            owned = re_owner.owned_hashes;
        } else {
            owned = OwnedBootstrap(win64, old_ledger, base);
        }

        const std::array<std::string, kBootstrap.size()> content = {
            ReadFileChecked(copies.empty() ? base / L"host\\ReShade64-game-forward.dll" : copies.front().first),
            "[INSTALL]\r\nBasePath=" + WideToUtf8(base.wstring()) + "\r\n",
        };
        Hashes expected;
        for (size_t i = 0; i < kBootstrap.size(); ++i) expected[i] = Sha256Hex(content[i]);
        const auto token = NewToken();

        const bool resuming_installing = !verified_owner && old_ledger.state == LedgerInfo::State::Installing;
        fs::path recovery_dir;
        std::array<fs::path, kBootstrap.size()> backup_paths;
        Hashes preserved_prev_hashes{};
        if (resuming_installing) {
            // Reuse original verified recovery backups & previous_sha256; never resample mixed Win64 files.
            if (!old_ledger.recovery_dir.empty()) {
                std::error_code rec_ec;
                if (fs::is_directory(old_ledger.recovery_dir, rec_ec)) recovery_dir = old_ledger.recovery_dir;
            }
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                const fs::path bkp = VerifiedRecoveryBackupPath(old_ledger.recovery_backup[i], old_ledger.previous_sha256[i], base);
                if (!bkp.empty()) {
                    backup_paths[i] = bkp;
                    preserved_prev_hashes[i] = old_ledger.previous_sha256[i];
                    if (recovery_dir.empty()) recovery_dir = bkp.parent_path();
                } else if (!owned[i].empty() && !old_ledger.previous_sha256[i].empty() && old_ledger.previous_owner.empty()) {
                    preserved_prev_hashes[i] = old_ledger.previous_sha256[i];
                }
            }
            // If retrying with a DIFFERENT payload (B2) while Win64 still holds B1's partial file,
            // reset that B1 file back to the verified pre-transaction state BEFORE rewriting the ledger.
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                if (!owned[i].empty() && owned[i] != expected[i] && owned[i] != preserved_prev_hashes[i]) {
                    if (!backup_paths[i].empty()) {
                        const fs::path rst_tmp = win64 / (L".wwmi-rst-" + Utf8ToWide(NewToken()) + L"-" + kBootstrap[i] + L".tmp");
                        WriteNewFile(rst_tmp, ReadFileChecked(backup_paths[i]));
                        if (!MoveFileExW(rst_tmp.c_str(), (win64 / kBootstrap[i]).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                            WinError("无法还原中断的旧载荷文件", win64 / kBootstrap[i]);
                        owned[i] = preserved_prev_hashes[i];
                    } else {
                        StageFilesToBackup({ win64 / kBootstrap[i] }, base, "reset-interrupted-payload");
                        owned[i].clear();
                    }
                }
            }
        } else if (!owned[0].empty() || !owned[1].empty()) {
            recovery_dir = CreateRecoveryBackup(win64, owned, base, token, backup_paths);
            preserved_prev_hashes = owned;
        }
        const bool had_recovery = !backup_paths[0].empty() || !backup_paths[1].empty();

        auto write_ledger = [&](bool installing) {
            std::ostringstream out;
            out << "{\n  \"schema_version\": 1,\n  \"state\": \"" << (installing ? "installing" : "installed") << "\",\n"
                << "  \"platform_kind\": \"windows\",\n"
                << "  \"base_root\": \"" << JsonEscape(base.wstring()) << "\",\n"
                << "  \"win64_dir\": \"" << JsonEscape(win64.wstring()) << "\",\n"
                << "  \"wwmi_dir\": \"" << JsonEscape(wwmi.wstring()) << "\",\n"
                << "  \"installed_version\": \"" << JsonEscape(Utf8ToWide(pkg_prov.version)) << "\",\n";
            if (!pkg_prov.commit.empty())
                out << "  \"installed_commit\": \"" << JsonEscape(Utf8ToWide(pkg_prov.commit)) << "\",\n";
            else
                out << "  \"installed_commit\": null,\n";
            if (!preserved_upgraded_from.empty())
                out << "  \"upgraded_from\": \"" << JsonEscape(Utf8ToWide(preserved_upgraded_from)) << "\",\n";
            if (installing) {
                out << "  \"pending_id\": \"" << token << "\",\n";
                if (!previous_owner_root.empty())
                    out << "  \"previous_owner\": \"" << JsonEscape(previous_owner_root.wstring()) << "\",\n";
                if (!recovery_dir.empty())
                    out << "  \"recovery_dir\": \"" << JsonEscape(recovery_dir.wstring()) << "\",\n";
            }
            out << "  \"win64_files\": [\n";
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                if (i) out << ",\n";
                out << "    { \"relative\": \"" << WideToUtf8(kBootstrap[i]) << "\", \"path\": \""
                    << JsonEscape((win64 / kBootstrap[i]).wstring()) << "\", \"sha256\": \"" << expected[i]
                    << "\", \"size\": " << content[i].size();
                if (installing && !preserved_prev_hashes[i].empty()) out << ", \"previous_sha256\": \"" << preserved_prev_hashes[i] << "\"";
                if (installing && !backup_paths[i].empty()) out << ", \"recovery_backup\": \"" << JsonEscape(backup_paths[i].wstring()) << "\"";
                out << " }";
            }
            out << "\n  ],\n  \"extra_managed_files\": []\n}\n";
            AtomicWrite(JournalPath(base), out.str());
        };

        std::string old_ledger_raw;
        if (!verified_owner && old_ledger.state == LedgerInfo::State::Installed && RegularFileOrMissing(JournalPath(base)))
            old_ledger_raw = ReadFileChecked(JournalPath(base));

        write_ledger(true);

        std::vector<size_t> swapped;
        try {
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                const auto temp = PendingPath(win64, token, kBootstrap[i]);
                const auto dest = win64 / kBootstrap[i];
                WriteNewFile(temp, content[i]);
                const DWORD flags = MOVEFILE_WRITE_THROUGH | (owned[i].empty() ? 0 : MOVEFILE_REPLACE_EXISTING);
                if (!MoveFileExW(temp.c_str(), dest.c_str(), flags)) WinError("无法提交引导文件", dest);
                swapped.push_back(i);
            }
        } catch (...) {
            if (had_recovery && (!swapped.empty() || resuming_installing)) {
                bool restored_ok = true;
                for (size_t idx = kBootstrap.size(); idx-- > 0;) {
                    if (backup_paths[idx].empty()) continue;
                    try {
                        const fs::path rb_tmp = win64 / (L".wwmi-rb-" + Utf8ToWide(NewToken()) + L"-" + kBootstrap[idx] + L".tmp");
                        WriteNewFile(rb_tmp, ReadFileChecked(backup_paths[idx]));
                        if (!MoveFileExW(rb_tmp.c_str(), (win64 / kBootstrap[idx]).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                            restored_ok = false;
                    } catch (...) {
                        restored_ok = false;
                    }
                }
                if (!recovery_dir.empty()) {
                    const fs::path aborted_dir = recovery_dir / L"aborted-tmp";
                    std::error_code mec;
                    for (size_t i = 0; i < kBootstrap.size(); ++i) {
                        const fs::path leftover = PendingPath(win64, token, kBootstrap[i]);
                        if (fs::exists(leftover, mec)) {
                            fs::create_directories(aborted_dir, mec);
                            if (!MoveFileExW(leftover.c_str(), (aborted_dir / leftover.filename()).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
                                restored_ok = false;
                        }
                    }
                }
                if (restored_ok) {
                    bool all_match = true;
                    try {
                        for (size_t i = 0; i < kBootstrap.size(); ++i) {
                            if (preserved_prev_hashes[i].empty()) continue;
                            if (!RegularFileOrMissing(win64 / kBootstrap[i]) ||
                                Sha256Hex(ReadFileChecked(win64 / kBootstrap[i])) != preserved_prev_hashes[i]) {
                                all_match = false;
                                break;
                            }
                        }
                    } catch (...) {
                        all_match = false;
                    }
                    if (all_match) {
                        try {
                            if (!previous_owner_root.empty()) {
                                std::ostringstream rec_j;
                                rec_j << "{\n  \"schema_version\": 1,\n  \"state\": \"uninstalled\",\n"
                                      << "  \"win64_dir\": \"" << JsonEscape(win64.wstring()) << "\",\n"
                                      << "  \"recovered_to\": \"" << JsonEscape(previous_owner_root.wstring()) << "\"\n}\n";
                                AtomicWrite(JournalPath(base), rec_j.str());
                            } else if (!old_ledger_raw.empty()) {
                                AtomicWrite(JournalPath(base), old_ledger_raw);
                            }
                        } catch (...) {
                        }
                    }
                }
            }
            throw;
        }

        // ---- Step 3: Commit B ownership and mark A migrated_to (never deleting A) ----
        write_ledger(false);
        if (!previous_owner_root.empty()) {
            const std::wstring w = MarkPreviousOwnerMigrated(previous_owner_root, base);
            if (!w.empty()) migration_warnings.push_back(w);
        }

        const std::wstring new_ver_w = Utf8ToWide(pkg_prov.version);
        if (action_kind == InstallActionKind::Upgrade && verified_owner) {
            const std::wstring old_ver_w = FormatInstalledVersionW(verified_owner->ledger);
            msg_out = g_zh_ui
                ? (L"并排升级成功（" + old_ver_w + L" -> " + new_ver_w + L"）！旧目录已完整保留，可随时回滚。请使用 DirectX 12 模式启动游戏。")
                : (L"Side-by-side upgrade succeeded (" + old_ver_w + L" -> " + new_ver_w + L")! Previous folder preserved for rollback. Launch in DirectX 12 mode.");
        } else if (action_kind == InstallActionKind::Rollback && verified_owner) {
            const std::wstring old_ver_w = FormatInstalledVersionW(verified_owner->ledger);
            msg_out = g_zh_ui
                ? (L"回滚成功（" + old_ver_w + L" -> " + new_ver_w + L"）！原目录已完整保留。请使用 DirectX 12 模式启动游戏。")
                : (L"Rollback succeeded (" + old_ver_w + L" -> " + new_ver_w + L")! Previous folder preserved. Launch in DirectX 12 mode.");
        } else if (action_kind == InstallActionKind::Switch && verified_owner) {
            msg_out = TrW(
                L"并排切换成功！原目录已完整保留。请使用 DirectX 12 模式从库洛官方启动器或 Steam 开始游戏。",
                L"Side-by-side switch succeeded! Previous folder preserved. Launch Wuthering Waves in DirectX 12 mode.");
        } else if (action_kind == InstallActionKind::Refresh) {
            msg_out = TrW(
                L"安装启用成功（原地刷新，已保留外部配置）！请使用 DirectX 12 模式从库洛官方启动器或 Steam 开始游戏。",
                L"Installed and enabled (In-Place Refresh; external settings preserved)! Launch Wuthering Waves in DirectX 12 mode.");
        } else {
            msg_out = TrW(
                L"安装启用成功！请使用 DirectX 12 模式从库洛官方启动器或 Steam 开始游戏。",
                L"Installed and enabled! Launch Wuthering Waves in DirectX 12 mode from the Kuro Launcher or Steam.");
        }

        auto join_list = [](const std::vector<std::wstring>& items) {
            std::wstring s;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) s += L", ";
                s += items[i];
            }
            return s;
        };
        if (!migrated_cache.empty())
            msg_out += (g_zh_ui ? L"\r\n[迁移] 已迁移缓存：" : L"\r\n[Migrated] Cache: ") + join_list(migrated_cache);
        if (!migrated_addons.empty())
            msg_out += (g_zh_ui ? L"\r\n[迁移] 已迁移第三方插件：" : L"\r\n[Migrated] Third-party add-ons: ") + join_list(migrated_addons);
        for (const auto& w : migration_warnings)
            msg_out += (g_zh_ui ? L"\r\n[注意] " : L"\r\n[WARN] ") + w;

        std::error_code core_ec;
        if (!fs::is_regular_file(wwmi / L"Core\\WWMI\\WuWa-Model-Importer.ini", core_ec)) {
            msg_out += TrW(
                L"\r\n\r\n[提示] 尚未检测到官方 Core\\WWMI 核心文件，请将官方 WWMI-Package 的 Core 目录放入该 WWMI 文件夹后再使用角色皮肤（需要 RabbitFX 的皮肤请在 Mods 下仅保留一份）。",
                L"\r\n\r\n[Note] Official Core\\WWMI files were not detected yet. Copy WWMI-Package's Core\\ folder into this WWMI directory before using character mods (keep at most one copy of RabbitFX under Mods\\).");
        }
        const auto extra_addons = ScanThirdPartyWin64Addons(win64);
        if (!extra_addons.empty()) {
            const std::wstring list_str = join_list(extra_addons);
            msg_out += g_zh_ui
                ? (L"\r\n\r\n[提示] 检测到游戏 Win64 目录存在其他 ReShade 插件（" + list_str +
                   L"）。如需一起加载（例如 RenoDX），可将其复制到本工具 reshade-addons\\，或在外部 ReShade.ini 的 [ADDON] 下添加 LoadGameDirAddons=1（若游戏内 Mod 异常请先临时停用第三方插件排查）。")
                : (L"\r\n\r\n[Note] Detected other ReShade add-on(s) in Win64: " + list_str +
                   L". To load them alongside WWMI-DX12-RT (e.g. RenoDX), either copy them into <ExternalFolder>\\reshade-addons\\ or add LoadGameDirAddons=1 under [ADDON] in <ExternalFolder>\\ReShade.ini. If mods stop working in-game, temporarily disable third-party add-ons to check for conflicts.");
        }
        return true;
    } catch (const std::exception& e) {
        msg_out = Utf8ToWide(e.what());
        return false;
    }
}

bool PerformRollback(const fs::path& raw_base, const fs::path& raw_target, std::wstring& msg_out)
{
    try {
        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再执行回滚。",
                "Wuthering Waves is currently running. Close the game and retry Rollback."));
        const auto base = CanonicalPath(raw_base);
        const auto initial_b = ReadLedger(base);
        if (initial_b.state != LedgerInfo::State::Installed && initial_b.state != LedgerInfo::State::RollingBack)
            throw std::runtime_error(TrA("当前目录未处于已安装或待恢复回滚状态，无法执行回滚。",
                "Current folder is not in an active installed or rolling_back state; cannot roll back."));

        fs::path raw_prev;
        if (initial_b.state == LedgerInfo::State::RollingBack) {
            if (!raw_target.empty()) {
                if (!IsValidAbsoluteWinPath(raw_target.wstring()))
                    throw std::runtime_error(TrA("回滚目标路径必须是受信任的本地绝对路径（拒绝 UNC/远程盘）。",
                        "Rollback target must be a valid local absolute path (UNC/remote paths rejected)."));
                if (!SamePath(CanonicalPath(raw_target), CanonicalPath(initial_b.rollback_target)))
                    throw std::runtime_error(TrA("当前存在未完成的回滚目标，请先重试完成原回滚目标。",
                        "An interrupted rollback to another target is pending; retry rollback to that target first."));
            }
            raw_prev = initial_b.rollback_target;
        } else {
            raw_prev = !raw_target.empty() ? raw_target : fs::path(Utf8ToWide(initial_b.upgraded_from));
        }
        if (raw_prev.empty())
            throw std::runtime_error(TrA("当前账本未记录升级来源目录（upgraded_from）；可通过 --to <旧目录> 指定。",
                "No previous folder (upgraded_from) recorded in ledger; specify --to <old_dir> to roll back."));
        if (!IsValidAbsoluteWinPath(raw_prev.wstring()))
            throw std::runtime_error(TrA("回滚目标路径必须是受信任的本地绝对路径（拒绝 UNC/远程盘）。",
                "Rollback target must be a valid local absolute path (UNC/remote paths rejected)."));
        const fs::path target_a = CanonicalPath(raw_prev);
        if (SamePath(target_a, base))
            throw std::runtime_error(TrA("回滚目标目录不能与当前目录相同。",
                "Rollback target cannot be the same as the current folder."));
        std::error_code ec;
        if (!fs::is_directory(target_a, ec))
            throw std::runtime_error(g_zh_ui
                ? ("旧版目录不存在，无法回滚：" + WideToUtf8(target_a.wstring()))
                : ("Previous folder does not exist: " + WideToUtf8(target_a.wstring())));
        const fs::path host_dll_a = target_a / L"host\\ReShade64-game-forward.dll";
        if (!RegularFileOrMissing(host_dll_a))
            throw std::runtime_error(g_zh_ui
                ? ("旧版目录缺少 host\\ReShade64-game-forward.dll，无法回滚：" + WideToUtf8(target_a.wstring()))
                : ("Previous folder is missing host\\ReShade64-game-forward.dll: " + WideToUtf8(target_a.wstring())));
        const fs::path win64 = CanonicalPath(initial_b.win64_dir);
        if (!SamePath(initial_b.win64_dir, win64))
            throw std::runtime_error(TrA("原 Win64 目录现在指向其他位置，拒绝回滚。",
                "Recorded Win64 directory now resolves elsewhere; refusing rollback."));

        auto validate_target_a = [&](const LedgerInfo& led_a, bool resuming, const LedgerInfo& led_b_cur) -> bool {
            if (led_a.state == LedgerInfo::State::Installed) {
                if (resuming &&
                    SamePath(CanonicalPath(led_a.win64_dir), win64) &&
                    !led_a.restored_from.empty() &&
                    SamePath(CanonicalPath(fs::path(Utf8ToWide(led_a.restored_from))), base) &&
                    led_a.restored_pending_id == led_b_cur.pending_id) {
                    bool all_match_a = true;
                    for (size_t i = 0; i < kBootstrap.size(); ++i) {
                        if (!RegularFileOrMissing(win64 / kBootstrap[i]) ||
                            Sha256Hex(ReadFileChecked(win64 / kBootstrap[i])) != led_a.sha256[i]) {
                            all_match_a = false;
                            break;
                        }
                    }
                    if (all_match_a) return true;
                }
                // Allow overwriting stale A ledger left in `installed` if marking A migrated failed during upgrade,
                // ONLY when A is bound to THIS same Win64, B records upgraded_from=A, and B owns both Win64 files.
                if (!resuming &&
                    SamePath(CanonicalPath(led_a.win64_dir), win64) &&
                    !led_b_cur.upgraded_from.empty() &&
                    IsValidAbsoluteWinPath(Utf8ToWide(led_b_cur.upgraded_from)) &&
                    SamePath(CanonicalPath(fs::path(Utf8ToWide(led_b_cur.upgraded_from))), target_a)) {
                    try {
                        const auto check_b = OwnedBootstrap(win64, led_b_cur, base);
                        if (!check_b[0].empty() && !check_b[1].empty()) return false;
                    } catch (const std::exception&) {
                    }
                }
                throw std::runtime_error(TrA("旧目录当前已处于活动安装状态，拒绝覆盖其绑定。",
                    "Previous folder is actively installed; refusing to overwrite its binding."));
            }
            if (led_a.pending())
                throw std::runtime_error(TrA("旧目录账本处于未完成事务状态，拒绝回滚。",
                    "Previous folder ledger has a pending transaction; refusing rollback."));
            if (led_a.state == LedgerInfo::State::Missing && !RegularFileOrMissing(target_a / L"wwmi_dx12.ini"))
                throw std::runtime_error(TrA("旧目录缺少安装账本和 wwmi_dx12.ini，无法确认为可回滚的 WWMI-DX12-RT 目录。",
                    "Previous folder lacks both install ledger and wwmi_dx12.ini; refusing rollback."));
            if (!led_a.win64_dir.empty() && !SamePath(CanonicalPath(led_a.win64_dir), win64))
                throw std::runtime_error(TrA("旧目录账本绑定的 Win64 与当前不一致，拒绝回滚。",
                    "Previous folder ledger is bound to a different Win64 directory; refusing rollback."));
            return false;
        };

        validate_target_a(ReadLedger(target_a), initial_b.state == LedgerInfo::State::RollingBack, initial_b);

        const fs::path pre_restore_src = RestoredSourceRoot(initial_b, base);
        const Win64MutexLock wlock(win64);
        const BaseRootMutexLocks blocks({ base, target_a, pre_restore_src });
        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再执行回滚。",
                "Wuthering Waves is currently running. Close the game and retry Rollback."));
        const auto ledger_b = ReadLedger(base);
        if ((ledger_b.state != LedgerInfo::State::Installed && ledger_b.state != LedgerInfo::State::RollingBack) ||
            !SamePath(CanonicalPath(ledger_b.win64_dir), win64))
            throw std::runtime_error(TrA("加锁后账本状态已变化，请重试回滚。",
                "Ledger state changed while acquiring lock; retry Rollback."));
        const bool resuming_rollback = (ledger_b.state == LedgerInfo::State::RollingBack);
        if (resuming_rollback && !SamePath(CanonicalPath(ledger_b.rollback_target), target_a))
            throw std::runtime_error(TrA("加锁后回滚目标发生变化，请重试回滚。",
                "Rollback target changed while acquiring lock; retry Rollback."));
        if (!RegularFileOrMissing(host_dll_a))
            throw std::runtime_error(g_zh_ui
                ? ("旧版目录缺少 host\\ReShade64-game-forward.dll，无法回滚：" + WideToUtf8(target_a.wstring()))
                : ("Previous folder is missing host\\ReShade64-game-forward.dll: " + WideToUtf8(target_a.wstring())));
        const fs::path cur_restore_src = RestoredSourceRoot(ledger_b, base);
        const BaseRootMutexLocks extra_rb_lock({
            (!cur_restore_src.empty() && !SamePath(cur_restore_src, base) && !SamePath(cur_restore_src, target_a) && !SamePath(cur_restore_src, pre_restore_src))
                ? cur_restore_src : fs::path{}
        });
        EnsureRestoreSourceFinalized(base, ledger_b);

        auto ledger_a = ReadLedger(target_a);
        const bool already_committed_a = validate_target_a(ledger_a, resuming_rollback, ledger_b);

        const auto abandoned = PendingFiles(ledger_b);
        if (!abandoned.empty()) StageFilesToBackup(abandoned, base, "interrupted-rollback");

        auto write_b_uninstalled = [&]() {
            std::ostringstream out_b;
            out_b << "{\n  \"schema_version\": 1,\n  \"state\": \"uninstalled\",\n"
                  << "  \"win64_dir\": \"" << JsonEscape(win64.wstring()) << "\",\n"
                  << "  \"rolled_back_to\": \"" << JsonEscape(target_a.wstring()) << "\"\n}\n";
            AtomicWrite(JournalPath(base), out_b.str());
        };

        if (already_committed_a) {
            write_b_uninstalled();
            const std::wstring target_ver = FormatInstalledVersionW(ledger_a);
            msg_out = g_zh_ui
                ? (L"已安全回滚至旧目录：" + target_a.wstring() + L"（版本：" + target_ver + L"）。")
                : (L"Safely rolled back to previous folder: " + target_a.wstring() + L" (version: " + target_ver + L").");
            return true;
        }

        Hashes owned_b = OwnedBootstrap(win64, ledger_b, base);
        if (owned_b[0].empty() || owned_b[1].empty())
            throw std::runtime_error(TrA("当前 Win64 引导文件不完整，请先修复或卸载当前安装。",
                "Current Win64 bootstrap files are incomplete; repair or uninstall first."));

        // If target_a still carries a stale `installed` ledger (from an earlier read-only upgrade warning),
        // mark it inactive BEFORE writing B's `rolling_back` ledger or touching Win64 so mid-swap crashes stay resumable.
        if (!resuming_rollback && ledger_a.state == LedgerInfo::State::Installed) {
            std::ostringstream stale_a;
            stale_a << "{\n  \"schema_version\": 1,\n  \"state\": \"uninstalled\",\n"
                    << "  \"platform_kind\": \"windows\",\n"
                    << "  \"base_root\": \"" << JsonEscape(target_a.wstring()) << "\",\n"
                    << "  \"win64_dir\": \"" << JsonEscape(win64.wstring()) << "\",\n"
                    << "  \"wwmi_dir\": \"" << JsonEscape(ledger_a.wwmi_dir.wstring()) << "\",\n";
            if (!ledger_a.installed_version.empty())
                stale_a << "  \"installed_version\": \"" << JsonEscape(Utf8ToWide(ledger_a.installed_version)) << "\",\n";
            if (!ledger_a.installed_commit.empty())
                stale_a << "  \"installed_commit\": \"" << JsonEscape(Utf8ToWide(ledger_a.installed_commit)) << "\",\n";
            stale_a << "  \"migrated_to\": \"" << JsonEscape(base.wstring()) << "\"\n}\n";
            AtomicWrite(JournalPath(target_a), stale_a.str());
            ledger_a.state = LedgerInfo::State::Uninstalled;
        }

        const std::array<std::string, kBootstrap.size()> content_a = {
            ReadFileChecked(host_dll_a),
            "[INSTALL]\r\nBasePath=" + WideToUtf8(target_a.wstring()) + "\r\n",
        };
        Hashes expected_a;
        for (size_t i = 0; i < kBootstrap.size(); ++i) expected_a[i] = Sha256Hex(content_a[i]);

        const auto token = NewToken();
        fs::path recovery_dir;
        std::array<fs::path, kBootstrap.size()> backup_paths;
        Hashes preserved_prev_hashes{};
        if (resuming_rollback) {
            if (!ledger_b.recovery_dir.empty() && fs::is_directory(ledger_b.recovery_dir, ec))
                recovery_dir = ledger_b.recovery_dir;
            size_t ok_cnt = 0;
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                const fs::path bkp = VerifiedRecoveryBackupPath(ledger_b.recovery_backup[i], ledger_b.previous_sha256[i], base);
                if (!bkp.empty()) {
                    backup_paths[i] = bkp;
                    preserved_prev_hashes[i] = ledger_b.previous_sha256[i];
                    if (recovery_dir.empty()) recovery_dir = bkp.parent_path();
                    ++ok_cnt;
                }
            }
            const bool all_a_now = (owned_b[0] == expected_a[0] && owned_b[1] == expected_a[1]);
            if (ok_cnt != kBootstrap.size() && !all_a_now)
                throw std::runtime_error(TrA("未完成回滚的原始恢复备份缺失或损坏，拒绝覆盖 Win64。",
                    "Original recovery backups for interrupted rollback are missing or corrupted; refusing to modify Win64."));
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                if (owned_b[i] != expected_a[i] && owned_b[i] != preserved_prev_hashes[i]) {
                    const fs::path rst_tmp = win64 / (L".wwmi-rst-" + Utf8ToWide(NewToken()) + L"-" + kBootstrap[i] + L".tmp");
                    WriteNewFile(rst_tmp, ReadFileChecked(backup_paths[i]));
                    if (!MoveFileExW(rst_tmp.c_str(), (win64 / kBootstrap[i]).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        WinError("无法还原中断回滚的旧文件", win64 / kBootstrap[i]);
                    owned_b[i] = preserved_prev_hashes[i];
                }
            }
        } else {
            recovery_dir = CreateRecoveryBackup(win64, owned_b, base, token, backup_paths);
            preserved_prev_hashes = owned_b;
        }

        auto write_b_ledger = [&](bool rolling_back) {
            std::ostringstream out;
            out << "{\n  \"schema_version\": 1,\n  \"state\": \"" << (rolling_back ? "rolling_back" : "installed") << "\",\n"
                << "  \"platform_kind\": \"windows\",\n"
                << "  \"base_root\": \"" << JsonEscape(base.wstring()) << "\",\n"
                << "  \"win64_dir\": \"" << JsonEscape(win64.wstring()) << "\",\n"
                << "  \"wwmi_dir\": \"" << JsonEscape(ledger_b.wwmi_dir.wstring()) << "\",\n";
            if (!ledger_b.installed_version.empty())
                out << "  \"installed_version\": \"" << JsonEscape(Utf8ToWide(ledger_b.installed_version)) << "\",\n";
            if (!ledger_b.installed_commit.empty())
                out << "  \"installed_commit\": \"" << JsonEscape(Utf8ToWide(ledger_b.installed_commit)) << "\",\n";
            if (!ledger_b.upgraded_from.empty())
                out << "  \"upgraded_from\": \"" << JsonEscape(Utf8ToWide(ledger_b.upgraded_from)) << "\",\n";
            if (rolling_back) {
                out << "  \"pending_id\": \"" << token << "\",\n"
                    << "  \"rollback_target\": \"" << JsonEscape(target_a.wstring()) << "\",\n";
                if (!recovery_dir.empty())
                    out << "  \"recovery_dir\": \"" << JsonEscape(recovery_dir.wstring()) << "\",\n";
            }
            out << "  \"win64_files\": [\n";
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                if (i) out << ",\n";
                const std::string& sha = rolling_back ? expected_a[i] : preserved_prev_hashes[i];
                const size_t sz = rolling_back ? content_a[i].size()
                    : (!backup_paths[i].empty() ? (size_t)fs::file_size(backup_paths[i], ec) : 0);
                out << "    { \"relative\": \"" << WideToUtf8(kBootstrap[i]) << "\", \"path\": \""
                    << JsonEscape((win64 / kBootstrap[i]).wstring()) << "\", \"sha256\": \"" << sha
                    << "\", \"size\": " << sz;
                if (rolling_back) {
                    out << ", \"previous_sha256\": \"" << preserved_prev_hashes[i]
                        << "\", \"recovery_backup\": \"" << JsonEscape(backup_paths[i].wstring()) << "\"";
                }
                out << " }";
            }
            out << "\n  ],\n  \"extra_managed_files\": []\n}\n";
            AtomicWrite(JournalPath(base), out.str());
        };

        const bool all_already_a = resuming_rollback && owned_b[0] == expected_a[0] && owned_b[1] == expected_a[1];
        const std::string commit_pending_id = all_already_a ? ledger_b.pending_id : token;
        if (!all_already_a) {
            write_b_ledger(true);
            try {
                for (size_t i = 0; i < kBootstrap.size(); ++i) {
                    if (owned_b[i] == expected_a[i]) continue;
                    const auto temp = PendingPath(win64, token, kBootstrap[i]);
                    const auto dest = win64 / kBootstrap[i];
                    WriteNewFile(temp, content_a[i]);
                    if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        WinError("无法提交回滚引导文件", dest);
                }
            } catch (...) {
                for (size_t idx = kBootstrap.size(); idx-- > 0;) {
                    if (backup_paths[idx].empty()) continue;
                    try {
                        const fs::path dst = win64 / kBootstrap[idx];
                        if (!preserved_prev_hashes[idx].empty() && RegularFileOrMissing(dst) &&
                            Sha256Hex(ReadFileChecked(dst)) == preserved_prev_hashes[idx])
                            continue;
                        const fs::path rb_tmp = win64 / (L".wwmi-rb-" + Utf8ToWide(NewToken()) + L"-" + kBootstrap[idx] + L".tmp");
                        WriteNewFile(rb_tmp, ReadFileChecked(backup_paths[idx]));
                        MoveFileExW(rb_tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                    } catch (...) {
                    }
                }
                if (!recovery_dir.empty()) {
                    const fs::path aborted_dir = recovery_dir / L"aborted-tmp";
                    std::error_code mec;
                    for (size_t i = 0; i < kBootstrap.size(); ++i) {
                        const fs::path leftover = PendingPath(win64, token, kBootstrap[i]);
                        if (fs::exists(leftover, mec)) {
                            fs::create_directories(aborted_dir, mec);
                            MoveFileExW(leftover.c_str(), (aborted_dir / leftover.filename()).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
                        }
                    }
                }
                if (!preserved_prev_hashes[0].empty() && !preserved_prev_hashes[1].empty()) {
                    bool all_match = true;
                    try {
                        for (size_t i = 0; i < kBootstrap.size(); ++i) {
                            if (!RegularFileOrMissing(win64 / kBootstrap[i]) ||
                                Sha256Hex(ReadFileChecked(win64 / kBootstrap[i])) != preserved_prev_hashes[i]) {
                                all_match = false;
                                break;
                            }
                        }
                    } catch (...) {
                        all_match = false;
                    }
                    if (all_match) {
                        try { write_b_ledger(false); } catch (...) {}
                    }
                }
                throw;
            }
        }

        // Restore A's ledger to installed (preserving v1.0.2/v1.0.3 compatibility) and mark B uninstalled.
        fs::path wwmi_a = ledger_a.wwmi_dir;
        {
            const fs::path ini_a = target_a / L"wwmi_dx12.ini";
            if (fs::is_regular_file(ini_a, ec)) {
                wchar_t val[4096]{};
                GetPrivateProfileStringW(L"wwmi-dx12", L"wwmi_dir", L"", val, (DWORD)std::size(val), ini_a.c_str());
                const std::wstring trimmed = TrimWide(val);
                if (!trimmed.empty() && IsValidAbsoluteWinPath(trimmed) && fs::is_directory(trimmed, ec))
                    wwmi_a = CanonicalPath(trimmed);
            }
            if (wwmi_a.empty()) wwmi_a = ledger_b.wwmi_dir;
        }
        std::ostringstream out_a;
        out_a << "{\n  \"schema_version\": 1,\n  \"state\": \"installed\",\n"
              << "  \"platform_kind\": \"windows\",\n"
              << "  \"base_root\": \"" << JsonEscape(target_a.wstring()) << "\",\n"
              << "  \"win64_dir\": \"" << JsonEscape(win64.wstring()) << "\",\n"
              << "  \"wwmi_dir\": \"" << JsonEscape(wwmi_a.wstring()) << "\",\n";
        if (!ledger_a.installed_version.empty())
            out_a << "  \"installed_version\": \"" << JsonEscape(Utf8ToWide(ledger_a.installed_version)) << "\",\n";
        if (!ledger_a.installed_commit.empty())
            out_a << "  \"installed_commit\": \"" << JsonEscape(Utf8ToWide(ledger_a.installed_commit)) << "\",\n";
        out_a << "  \"restored_from\": \"" << JsonEscape(base.wstring()) << "\",\n"
              << "  \"restored_pending_id\": \"" << commit_pending_id << "\",\n"
              << "  \"win64_files\": [\n";
        for (size_t i = 0; i < kBootstrap.size(); ++i) {
            if (i) out_a << ",\n";
            out_a << "    { \"relative\": \"" << WideToUtf8(kBootstrap[i]) << "\", \"path\": \""
                  << JsonEscape((win64 / kBootstrap[i]).wstring()) << "\", \"sha256\": \"" << expected_a[i]
                  << "\", \"size\": " << content_a[i].size() << " }";
        }
        out_a << "\n  ],\n  \"extra_managed_files\": []\n}\n";
        fs::create_directories(target_a / L"runtime", ec);
        AtomicWrite(JournalPath(target_a), out_a.str());

        write_b_uninstalled();

        const std::wstring target_ver = FormatInstalledVersionW(ledger_a);
        msg_out = g_zh_ui
            ? (L"已安全回滚至旧目录：" + target_a.wstring() + L"（版本：" + target_ver + L"）。")
            : (L"Safely rolled back to previous folder: " + target_a.wstring() + L" (version: " + target_ver + L").");
        return true;
    } catch (const std::exception& e) {
        msg_out = Utf8ToWide(e.what());
        return false;
    }
}

bool PerformUninstall(const fs::path& raw_base, std::wstring& msg_out)
{
    try {
        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再卸载。",
                "Wuthering Waves is currently running. Close the game and retry Uninstall."));
        const auto base = CanonicalPath(raw_base);
        const auto noop = [&]() {
            msg_out = TrW(L"当前没有活动中的安装记录，无需卸载。", L"No active installation ledger found; nothing to uninstall.");
            return true;
        };
        const auto initial = ReadLedger(base);
        if (!initial.active()) return noop();
        if (initial.state == LedgerInfo::State::RollingBack)
            throw std::runtime_error(TrA("检测到未完成的回滚事务，请先重试【回滚旧版】完成后再按需卸载。",
                "An interrupted rollback transaction is pending; retry Rollback first before uninstalling."));
        const fs::path lock_win64 = CanonicalPath(initial.win64_dir);
        const fs::path pre_restore_src = RestoredSourceRoot(initial, base);
        const Win64MutexLock lock(lock_win64);
        const BaseRootMutexLocks base_lock({ base, pre_restore_src });
        const auto ledger = ReadLedger(base);
        if (!ledger.active()) return noop();
        if (ledger.state == LedgerInfo::State::RollingBack)
            throw std::runtime_error(TrA("检测到未完成的回滚事务，请先重试【回滚旧版】完成后再按需卸载。",
                "An interrupted rollback transaction is pending; retry Rollback first before uninstalling."));
        const fs::path cur_restore_src = RestoredSourceRoot(ledger, base);
        const BaseRootMutexLocks extra_rb_lock({
            (!cur_restore_src.empty() && !SamePath(cur_restore_src, pre_restore_src)) ? cur_restore_src : fs::path{}
        });
        EnsureRestoreSourceFinalized(base, ledger);
        if (!SamePath(CanonicalPath(ledger.win64_dir), lock_win64))
            throw std::runtime_error(TrA("安装账本绑定的 Win64 目录在加锁期间发生变化，请重试卸载。",
                "Recorded Win64 directory changed while acquiring lock; retry Uninstall."));
        if (!SamePath(ledger.win64_dir, CanonicalPath(ledger.win64_dir)))
            throw std::runtime_error(TrA("原 Win64 目录现在指向其他位置，拒绝卸载。",
                "Recorded Win64 directory now resolves elsewhere; refusing to uninstall."));
        const auto owned = OwnedBootstrap(ledger.win64_dir, ledger, base);
        const auto pending = PendingFiles(ledger);

        // If aborting an interrupted side-by-side upgrade with verified recovery backups, restore previous owner cleanly.
        if (ledger.state == LedgerInfo::State::Installing && !ledger.previous_owner.empty()) {
            std::array<fs::path, kBootstrap.size()> verified_bkps;
            bool can_restore = true;
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                verified_bkps[i] = VerifiedRecoveryBackupPath(ledger.recovery_backup[i], ledger.previous_sha256[i], base);
                if (verified_bkps[i].empty()) {
                    can_restore = false;
                    break;
                }
            }
            if (can_restore) {
                const auto batch = pending.empty() ? fs::path{} : StageFilesToBackup(pending, base, "abort-upgrade");
                for (size_t i = 0; i < kBootstrap.size(); ++i) {
                    const fs::path rb_tmp = ledger.win64_dir / (L".wwmi-restore-" + Utf8ToWide(NewToken()) + L"-" + kBootstrap[i] + L".tmp");
                    WriteNewFile(rb_tmp, ReadFileChecked(verified_bkps[i]));
                    if (!MoveFileExW(rb_tmp.c_str(), (ledger.win64_dir / kBootstrap[i]).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        WinError("无法还原原引导文件", ledger.win64_dir / kBootstrap[i]);
                }
                std::ostringstream journal;
                journal << "{\n  \"schema_version\": 1,\n  \"state\": \"uninstalled\",\n  \"win64_dir\": \""
                    << JsonEscape(ledger.win64_dir.wstring()) << "\",\n  \"restored_previous_owner\": \""
                    << JsonEscape(ledger.previous_owner.wstring()) << "\",\n  \"staged_batch\": \""
                    << JsonEscape(batch.wstring()) << "\"\n}\n";
                AtomicWrite(JournalPath(base), journal.str());
                msg_out = g_zh_ui
                    ? (L"已中止未完成的升级并还原原版本引导文件（" + ledger.previous_owner.wstring() + L"）。")
                    : (L"Aborted interrupted upgrade and restored previous version bootstrap files (" + ledger.previous_owner.wstring() + L").");
                return true;
            }
        }

        std::vector<fs::path> files;
        for (size_t i = 0; i < kBootstrap.size(); ++i)
            if (!owned[i].empty()) files.push_back(ledger.win64_dir / kBootstrap[i]);
        files.insert(files.end(), pending.begin(), pending.end());
        const auto batch = files.empty() ? fs::path{} : StageFilesToBackup(files, base, "uninstall");
        std::ostringstream journal;
        journal << "{\n  \"schema_version\": 1,\n  \"state\": \"uninstalled\",\n  \"win64_dir\": \""
            << JsonEscape(ledger.win64_dir.wstring()) << "\",\n  \"staged_batch\": \"" << JsonEscape(batch.wstring()) << "\"\n}\n";
        AtomicWrite(JournalPath(base), journal.str());
        msg_out = batch.empty()
            ? std::wstring(TrW(L"已卸载：引导文件均已不存在。", L"Uninstalled: bootstrap files were already absent."))
            : (std::wstring(TrW(L"已卸载，引导文件暂存于：", L"Uninstalled. Staged bootstrap backup: ")) + batch.wstring());
        return true;
    } catch (const std::exception& e) {
        msg_out = Utf8ToWide(e.what());
        return false;
    }
}

std::wstring BuildDoctorText(const fs::path& raw_game, const fs::path& raw_wwmi, const fs::path& base_root, const fs::path& migrate_from = {})
{
    std::wostringstream ss;
    const auto pkg_prov = ReadPackageProvenance(base_root);
    ss << TrW(L"当前包版本：", L"Package version: ") << Utf8ToWide(pkg_prov.version) << L"\r\n";
    try {
        const auto led = ReadLedger(base_root);
        if (led.active()) {
            ss << TrW(L"已安装版本：", L"Installed version: ") << FormatInstalledVersionW(led) << L"\r\n";
        } else if (led.state == LedgerInfo::State::Uninstalled && !led.migrated_to.empty()) {
            ss << TrW(L"迁移记录：曾迁移至 ", L"Migration note: previously migrated to ")
               << Utf8ToWide(led.migrated_to) << L"\r\n";
        }
    } catch (const std::exception&) {
    }
    ss << TrW(L"外部挂载程序根：", L"External runtime root: ") << NormWinPath(base_root) << L"\r\n";

    const fs::path win64 = ResolveWin64FromAnyGameDir(raw_game);
    if (win64.empty()) {
        ss << TrW(L"[错误] 游戏目录：未找到 Client-Win64-Shipping.exe\r\n",
                  L"[ERR]  Game directory: Client-Win64-Shipping.exe not found\r\n");
    } else {
        ss << TrW(L"[正常] 游戏 Win64：", L"[OK]   Game Win64: ") << win64.wstring() << L"\r\n";
        std::wstring mmsg;
        InspectMountState(win64, base_root, mmsg, migrate_from);
        ss << TrW(L"[状态] 挂载检查：", L"[INFO] Mount status: ") << mmsg << L"\r\n";
    }

    std::error_code ec;
    if (raw_wwmi.empty() || !fs::is_regular_file(raw_wwmi / L"d3dx.ini", ec)) {
        ss << TrW(L"[待设] WWMI 目录：尚未关联有效的 d3dx.ini（可选中已有 XXMI\\WWMI 或点击新建骨架）\r\n",
                  L"[WARN] WWMI directory: no d3dx.ini linked yet (select an existing XXMI\\WWMI folder or click Init Skeleton)\r\n");
    } else {
        const fs::path wwmi = NormWinPath(raw_wwmi);
        ss << TrW(L"[正常] WWMI 目录：", L"[OK]   WWMI directory: ") << wwmi.wstring() << L"\r\n";
        if (fs::is_regular_file(wwmi / L"Core\\WWMI\\WuWa-Model-Importer.ini", ec)) {
            const std::wstring ver = DetectWwmiCoreVersion(wwmi);
            if (g_zh_ui)
                ss << L"[正常] Core\\WWMI 核心包齐全" << (ver.empty() ? L"" : (L"（检测到 WWMI " + ver + L"）")) << L"\r\n";
            else
                ss << L"[OK]   Core\\WWMI scripts found" << (ver.empty() ? L"" : (L" (WWMI " + ver + L")")) << L"\r\n";
        } else {
            ss << TrW(L"[提示] 缺少 Core\\WWMI\\WuWa-Model-Importer.ini（请放入官方 WWMI-Package 核心文件）\r\n",
                      L"[WARN] Missing Core\\WWMI\\WuWa-Model-Importer.ini (place official WWMI-Package Core\\ files here)\r\n");
        }
        if (!win64.empty()) {
            const auto extra_addons = ScanThirdPartyWin64Addons(win64);
            if (!extra_addons.empty()) {
                std::wstring list_str;
                for (size_t i = 0; i < extra_addons.size(); ++i) {
                    if (i) list_str += L", ";
                    list_str += extra_addons[i];
                }
                if (g_zh_ui)
                    ss << L"[提示] Win64 存在其他 ReShade 插件：" << list_str
                       << L"（需放入外部 reshade-addons\\ 或在外部 ReShade.ini [ADDON] 中设 LoadGameDirAddons=1 才会加载）\r\n";
                else
                    ss << L"[WARN] Other ReShade add-on(s) in Win64: " << list_str
                       << L" (copy into <ExternalFolder>\\reshade-addons\\ or set LoadGameDirAddons=1 under [ADDON] in external ReShade.ini to load them)\r\n";
            }
        }

        const int rfx = CountRabbitFxInstances(wwmi);
        if (rfx == 1)
            ss << TrW(L"[正常] RabbitFX.ini 实例唯一 (1 份)\r\n", L"[OK]   RabbitFX.ini unique (1 copy)\r\n");
        else if (rfx == 0)
            ss << TrW(L"[提示] Mods 下未检测到 RabbitFX.ini（部分材质/发光 Mod 需要安装 RabbitFX）\r\n",
                      L"[WARN] RabbitFX.ini not found under Mods\\ (required by some material/glow mods)\r\n");
        else if (g_zh_ui)
            ss << L"[错误] 检测到 " << rfx << L" 份重复的 RabbitFX.ini！请只保留唯一一份以免着色器冲突\r\n";
        else
            ss << L"[ERR]  Detected " << rfx << L" duplicate copies of RabbitFX.ini! Keep only ONE copy to avoid shader conflicts.\r\n";
    }
    return ss.str();
}

// ------------------------- Opt-In Manual Update Check -------------------------
constexpr const wchar_t* kGithubAllowedPrefix = L"https://github.com/Bud-Open/wwmi-dx12-rt/";
constexpr const wchar_t* kGithubFallbackUrl = L"https://github.com/Bud-Open/wwmi-dx12-rt/releases/latest";

bool IsAllowedGithubReleaseUrl(const std::wstring& url)
{
    if (url.rfind(kGithubAllowedPrefix, 0) != 0) return false;
    for (wchar_t c : url) {
        if (c <= 0x20 || c == L'"' || c == L'\'') return false;
    }
    return true;
}

struct UpdateCheckResult {
    bool ok = false;
    bool update_available = false;
    std::wstring current_version;
    std::wstring latest_version;
    std::wstring release_url = kGithubFallbackUrl;
    std::wstring asset_name;
    std::wstring asset_digest;
    std::wstring published_at;
    std::wstring error;
};

UpdateCheckResult CheckLatestReleaseWinHttp(const std::string& current_version)
{
    UpdateCheckResult res;
    res.current_version = Utf8ToWide(current_version.empty() ? "dev" : current_version);

    HINTERNET hSession = WinHttpOpen(
        L"WWMI-DX12-RT-Setup",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) {
        res.error = TrW(L"无法初始化 WinHTTP 网络会话。", L"Failed to initialize WinHTTP session.");
        return res;
    }
    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        res.error = TrW(L"无法连接 api.github.com。", L"Failed to connect to api.github.com.");
        return res;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        L"/repos/Bud-Open/wwmi-dx12-rt/releases/latest",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.error = TrW(L"无法创建 HTTPS 请求。", L"Failed to create HTTPS request.");
        return res;
    }

    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (sent) sent = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (sent) {
        WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX);
    }

    std::string body;
    constexpr size_t kMaxBody = 1024 * 1024;
    if (sent && status_code == 200) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
            if (body.size() >= kMaxBody) break;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(avail, kMaxBody - body.size()));
            const size_t offset = body.size();
            body.resize(offset + chunk);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, body.data() + offset, chunk, &read) || read == 0) {
                body.resize(offset);
                break;
            }
            body.resize(offset + read);
        }
    }

    const DWORD net_err = sent ? ERROR_SUCCESS : GetLastError();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!sent) {
        res.error = g_zh_ui
            ? (L"无法访问 GitHub Releases（WinHTTP " + std::to_wstring(net_err) + L"）。请检查网络连接。")
            : (L"Could not reach GitHub Releases (WinHTTP " + std::to_wstring(net_err) + L"). Check your network connection.");
        return res;
    }
    if (status_code != 200) {
        res.error = g_zh_ui
            ? (L"GitHub API 返回状态码 HTTP " + std::to_wstring(status_code) + L"。")
            : (L"GitHub API returned HTTP " + std::to_wstring(status_code) + L".");
        return res;
    }

    try {
        const auto root = ledger_json::Reader(body).parse();
        if (root.type != ledger_json::Value::Type::Object) throw std::runtime_error("not a JSON object");
        std::string latest;
        if (auto it = root.fields.find("tag_name"); it != root.fields.end() && it->second.type == ledger_json::Value::Type::String)
            latest = TrimAscii(it->second.text);
        res.latest_version = Utf8ToWide(latest);
        if (auto it = root.fields.find("html_url"); it != root.fields.end() && it->second.type == ledger_json::Value::Type::String) {
            const std::wstring candidate = Utf8ToWide(it->second.text);
            if (IsAllowedGithubReleaseUrl(candidate)) res.release_url = candidate;
        }
        if (auto it = root.fields.find("published_at"); it != root.fields.end() && it->second.type == ledger_json::Value::Type::String)
            res.published_at = Utf8ToWide(it->second.text);
        if (auto it = root.fields.find("assets"); it != root.fields.end() && it->second.type == ledger_json::Value::Type::Array) {
            const ledger_json::Value* chosen = nullptr;
            for (const char* suffix : { "-win64.zip", ".zip" }) {
                const size_t slen = strlen(suffix);
                for (const auto& item : it->second.elements) {
                    if (item.type != ledger_json::Value::Type::Object) continue;
                    auto nit = item.fields.find("name");
                    if (nit == item.fields.end() || nit->second.type != ledger_json::Value::Type::String) continue;
                    const std::string low = LowerAscii(nit->second.text);
                    if (low.size() >= slen && low.compare(low.size() - slen, slen, suffix) == 0) {
                        chosen = &item;
                        break;
                    }
                }
                if (chosen) break;
            }
            if (chosen) {
                if (auto nit = chosen->fields.find("name"); nit != chosen->fields.end() && nit->second.type == ledger_json::Value::Type::String)
                    res.asset_name = Utf8ToWide(nit->second.text);
                if (auto dit = chosen->fields.find("digest"); dit != chosen->fields.end() && dit->second.type == ledger_json::Value::Type::String)
                    res.asset_digest = Utf8ToWide(dit->second.text);
            }
        }
        const auto cmp = CompareSemVer(current_version, latest);
        res.update_available = (cmp.has_value() && *cmp < 0);
        res.ok = true;
    } catch (const std::exception& e) {
        res.error = g_zh_ui
            ? (L"解析 GitHub 更新信息失败：" + Utf8ToWide(e.what()))
            : (L"Failed to parse GitHub release metadata: " + Utf8ToWide(e.what()));
    }
    return res;
}

std::wstring FormatUpdateDialogMessage(const UpdateCheckResult& r)
{
    std::wostringstream ss;
    ss << TrW(L"当前版本：", L"Current version: ") << r.current_version << L"\r\n"
       << TrW(L"最新发布：", L"Latest release: ") << (r.latest_version.empty() ? TrW(L"未知", L"Unknown") : r.latest_version.c_str()) << L"\r\n";
    if (!r.published_at.empty())
        ss << TrW(L"发布时间：", L"Published at: ") << r.published_at << L"\r\n";
    if (!r.asset_name.empty())
        ss << TrW(L"推荐压缩包：", L"Windows archive: ") << r.asset_name << L"\r\n";
    if (!r.asset_digest.empty())
        ss << TrW(L"校验摘要：", L"Digest: ") << r.asset_digest << L"\r\n";
    ss << L"\r\n";
    if (r.update_available) {
        ss << TrW(L"发现新版本！建议下载后解压至新目录，运行新版安装器一键并排升级（支持随时回滚）。",
                  L"A newer version is available! Download and extract into a new folder, then run Setup for side-by-side upgrade (with rollback).");
    } else if (!ParseSemVer(WideToUtf8(r.current_version)).has_value()) {
        ss << TrW(L"当前为开发/自定义版本，请对照上方最新正式版自行判断是否更新。",
                  L"Current build is a dev/custom version; compare against the latest release above.");
    } else {
        ss << TrW(L"当前已是最新版本。", L"You are already on the latest release.");
    }
    return ss.str();
}

// ------------------------- Headless CLI Mode -------------------------
void CliPrint(const std::wstring& w)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode)) {
        const std::wstring line = w + L"\n";
        DWORD written = 0;
        WriteConsoleW(hOut, line.data(), (DWORD)line.size(), &written, nullptr);
        return;
    }
    const std::string s = WideToUtf8(w) + "\n";
    DWORD written = 0;
    WriteFile(hOut, s.data(), (DWORD)s.size(), &written, nullptr);
}

int RunCli(int argc, wchar_t** argv, const fs::path& base_root)
{
    std::wstring action = L"doctor";
    fs::path game_arg, wwmi_arg, migrate_from_arg, to_arg;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--cli" && i + 1 < argc) action = argv[++i];
        else if (a == L"--game" && i + 1 < argc) game_arg = argv[++i];
        else if (a == L"--wwmi" && i + 1 < argc) wwmi_arg = argv[++i];
        else if (a == L"--migrate-from" && i + 1 < argc) migrate_from_arg = argv[++i];
        else if (a == L"--to" && i + 1 < argc) to_arg = argv[++i];
        else if (a == L"--lang" && i + 1 < argc) g_zh_ui = DetectChineseUi(argv[++i]);
    }
    if (action == L"check-update") {
        const auto pkg = ReadPackageProvenance(base_root);
        const auto res = CheckLatestReleaseWinHttp(pkg.version);
        if (!res.ok) {
            CliPrint(L"CLI_ERR: " + res.error);
            return 1;
        }
        CliPrint(FormatUpdateDialogMessage(res) + L"\r\n" + TrW(L"发布页面：", L"Release page: ") + res.release_url);
        return 0;
    }
    if (action == L"rollback") {
        std::wstring msg;
        const bool ok = PerformRollback(base_root, to_arg, msg);
        CliPrint((ok ? L"CLI_OK: " : L"CLI_ERR: ") + msg);
        return ok ? 0 : 1;
    }
    if (action != L"uninstall" && action != L"init-wwmi" && game_arg.empty())
        game_arg = AutoDiscoverGameWin64(base_root);
    if (action != L"uninstall" && wwmi_arg.empty())
        wwmi_arg = AutoDiscoverWwmiDir(base_root, game_arg);

    if (action == L"init-wwmi") {
        const fs::path target = wwmi_arg.empty() ? (base_root / L"WWMI") : wwmi_arg;
        std::wstring err;
        if (!InitWwmiSkeleton(target, err)) {
            CliPrint(L"CLI_ERR: " + err);
            return 1;
        }
        CliPrint(L"CLI_OK init-wwmi=" + NormWinPath(target));
        return 0;
    }
    if (action == L"install") {
        std::wstring msg;
        const bool ok = PerformInstall(game_arg, wwmi_arg, base_root, msg, migrate_from_arg);
        CliPrint((ok ? L"CLI_OK: " : L"CLI_ERR: ") + msg);
        return ok ? 0 : 1;
    }
    if (action == L"uninstall") {
        std::wstring msg;
        const bool ok = PerformUninstall(base_root, msg);
        CliPrint((ok ? L"CLI_OK: " : L"CLI_ERR: ") + msg);
        return ok ? 0 : 1;
    }
    CliPrint(BuildDoctorText(game_arg, wwmi_arg, base_root, migrate_from_arg));
    return 0;
}

// ------------------------- Obsidian & Gold Native Win32 GUI -------------------------
enum CtrlId : int {
    IDC_EDIT_GAME = 101,
    IDC_BTN_BROWSE_GAME = 102,
    IDC_EDIT_WWMI = 103,
    IDC_BTN_BROWSE_WWMI = 104,
    IDC_BTN_INSTALL = 107,
    IDC_BTN_UNINSTALL = 108,
    IDC_BTN_MORE = 114,
};

enum MenuId : int {
    IDM_ROLLBACK = 201,
    IDM_CHECK_UPDATE = 202,
    IDM_REFRESH = 203,
    IDM_OPEN_MODS = 204,
    IDM_OPEN_LOGS = 205,
    IDM_INIT_WWMI = 206,
};

constexpr UINT WM_APP_UPDATE_RESULT = WM_APP + 1;

namespace ui_theme {
constexpr COLORREF kWindowBg     = RGB(17, 17, 20);   // #111114
constexpr COLORREF kTitleBg      = RGB(23, 22, 25);   // #171619
constexpr COLORREF kCardBg       = RGB(26, 25, 29);   // #1A191D
constexpr COLORREF kCardBorder   = RGB(45, 42, 36);   // #2D2A24
constexpr COLORREF kInputBg      = RGB(16, 16, 19);   // #101013
constexpr COLORREF kInputBorder  = RGB(53, 49, 41);   // #353129
constexpr COLORREF kTextPrimary  = RGB(243, 238, 227);// #F3EEE3
constexpr COLORREF kTextInput    = RGB(236, 229, 214);// #ECE5D6
constexpr COLORREF kTextMuted    = RGB(161, 155, 143);// #A19B8F
constexpr COLORREF kTextDisabled = RGB(104, 99, 90);  // #68635A

constexpr COLORREF kGold         = RGB(212, 175, 90); // #D4AF5A
constexpr COLORREF kGoldHover    = RGB(226, 192, 111);// #E2C06F
constexpr COLORREF kGoldPress    = RGB(184, 147, 66); // #B89342
constexpr COLORREF kGoldText     = RGB(20, 18, 12);   // #14120C
constexpr COLORREF kGoldSoftBg   = RGB(42, 36, 23);   // #2A2417
constexpr COLORREF kGoldSoftBdr  = RGB(74, 62, 36);   // #4A3E24
constexpr COLORREF kGoldSoftFg   = RGB(228, 192, 114);// #E4C072

constexpr COLORREF kSecBg        = RGB(35, 33, 38);   // #232126
constexpr COLORREF kSecHover     = RGB(46, 43, 50);   // #2E2B32
constexpr COLORREF kSecPress     = RGB(27, 25, 30);   // #1B191E
constexpr COLORREF kSecBorder    = RGB(62, 57, 48);   // #3E3930
constexpr COLORREF kSecHoverBdr  = RGB(106, 93, 66);  // #6A5D42
constexpr COLORREF kSecDisBg     = RGB(26, 25, 29);   // #1A191D
constexpr COLORREF kSecDisBdr    = RGB(42, 39, 35);   // #2A2723

constexpr COLORREF kOkFg         = RGB(94, 234, 212); // #5EEAD4
constexpr COLORREF kOkBg         = RGB(19, 42, 36);   // #132A24
constexpr COLORREF kOkDot        = RGB(45, 212, 191); // #2DD4BF
constexpr COLORREF kWarnFg       = RGB(245, 196, 81); // #F5C451
constexpr COLORREF kErrFg        = RGB(248, 113, 113);// #F87171
constexpr COLORREF kErrBg        = RGB(49, 24, 28);   // #31181C
constexpr COLORREF kFreshBg      = RGB(34, 33, 38);   // #222126
constexpr COLORREF kFreshFg      = RGB(199, 192, 179);// #C7C0B3
constexpr COLORREF kFreshDot     = RGB(142, 136, 124);// #8E887C
} // namespace ui_theme

inline void BlendPixel(uint32_t& dst, COLORREF cr, float alpha)
{
    if (alpha <= 0.0f) return;
    const uint32_t sr = GetRValue(cr), sg = GetGValue(cr), sb = GetBValue(cr);
    if (alpha >= 1.0f) {
        dst = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
        return;
    }
    const float dr = static_cast<float>((dst >> 16) & 0xFFu);
    const float dg = static_cast<float>((dst >> 8) & 0xFFu);
    const float db = static_cast<float>(dst & 0xFFu);
    const float inv = 1.0f - alpha;
    const uint32_t or_ = static_cast<uint32_t>(sr * alpha + dr * inv + 0.5f);
    const uint32_t og_ = static_cast<uint32_t>(sg * alpha + dg * inv + 0.5f);
    const uint32_t ob_ = static_cast<uint32_t>(sb * alpha + db * inv + 0.5f);
    dst = 0xFF000000u | (or_ << 16) | (og_ << 8) | ob_;
}

struct DibCanvas {
    HDC      dc = nullptr;
    HBITMAP  bmp = nullptr;
    HBITMAP  old_bmp = nullptr;
    uint32_t* px = nullptr;
    int      w = 0;
    int      h = 0;

    DibCanvas(HDC ref_dc, int width, int height) : w(width), h(height)
    {
        if (w <= 0 || h <= 0) return;
        dc = CreateCompatibleDC(ref_dc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) {
            old_bmp = static_cast<HBITMAP>(SelectObject(dc, bmp));
            px = static_cast<uint32_t*>(bits);
        }
    }
    ~DibCanvas()
    {
        if (dc && old_bmp) SelectObject(dc, old_bmp);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
    DibCanvas(const DibCanvas&) = delete;
    DibCanvas& operator=(const DibCanvas&) = delete;

    bool valid() const { return dc != nullptr && px != nullptr; }

    void Fill(COLORREF cr)
    {
        if (!px) return;
        const uint32_t val = 0xFF000000u | ((uint32_t)GetRValue(cr) << 16) | ((uint32_t)GetGValue(cr) << 8) | (uint32_t)GetBValue(cr);
        std::fill(px, px + static_cast<size_t>(w) * static_cast<size_t>(h), val);
    }

    void RoundRectAA(int x, int y, int rw, int rh, float radius, COLORREF fill, COLORREF border, float border_px = 1.0f)
    {
        if (!px || rw <= 0 || rh <= 0) return;
        const float r = std::min(radius, std::min(rw, rh) * 0.5f);
        const float cx = x + rw * 0.5f;
        const float cy = y + rh * 0.5f;
        const float hx = rw * 0.5f - r;
        const float hy = rh * 0.5f - r;
        const int x0 = std::max(0, x - 1);
        const int y0 = std::max(0, y - 1);
        const int x1 = std::min(w, x + rw + 1);
        const int y1 = std::min(h, y + rh + 1);
        for (int py = y0; py < y1; ++py) {
            const float dy = std::fabs((py + 0.5f) - cy) - hy;
            uint32_t* row = px + static_cast<size_t>(py) * static_cast<size_t>(w);
            for (int px_i = x0; px_i < x1; ++px_i) {
                const float dx = std::fabs((px_i + 0.5f) - cx) - hx;
                const float ax = std::max(dx, 0.0f);
                const float ay = std::max(dy, 0.0f);
                const float d = std::sqrt(ax * ax + ay * ay) + std::min(std::max(dx, dy), 0.0f) - r;
                const float cov_outer = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (cov_outer <= 0.0f) continue;
                if (border_px > 0.0f) {
                    const float cov_inner = std::clamp(0.5f - (d + border_px), 0.0f, 1.0f);
                    BlendPixel(row[px_i], border, cov_outer);
                    if (cov_inner > 0.0f) BlendPixel(row[px_i], fill, cov_inner);
                } else {
                    BlendPixel(row[px_i], fill, cov_outer);
                }
            }
        }
    }

    void CircleAA(float cx, float cy, float r, COLORREF fill)
    {
        if (!px || r <= 0.0f) return;
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 1.0f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 1.0f)));
        const int x1 = std::min(w, static_cast<int>(std::ceil(cx + r + 1.0f)));
        const int y1 = std::min(h, static_cast<int>(std::ceil(cy + r + 1.0f)));
        for (int py = y0; py < y1; ++py) {
            const float dy = (py + 0.5f) - cy;
            uint32_t* row = px + static_cast<size_t>(py) * static_cast<size_t>(w);
            for (int px_i = x0; px_i < x1; ++px_i) {
                const float dx = (px_i + 0.5f) - cx;
                const float cov = std::clamp(0.5f - (std::sqrt(dx * dx + dy * dy) - r), 0.0f, 1.0f);
                if (cov > 0.0f) BlendPixel(row[px_i], fill, cov);
            }
        }
    }

    void HLine(int x0, int x1, int y, COLORREF cr)
    {
        if (!px || y < 0 || y >= h) return;
        const int a = std::max(0, x0), b = std::min(w, x1);
        uint32_t* row = px + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int i = a; i < b; ++i) BlendPixel(row[i], cr, 1.0f);
    }

    void Text(HFONT font, COLORREF fg, const std::wstring& s, RECT rc, UINT flags)
    {
        if (!dc || s.empty()) return;
        HGDIOBJ old_f = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &rc, flags | DT_NOPREFIX);
        SelectObject(dc, old_f);
    }

    int MeasureTextWidth(HFONT font, const std::wstring& s)
    {
        if (!dc || s.empty()) return 0;
        HGDIOBJ old_f = SelectObject(dc, font);
        SIZE sz{};
        GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
        SelectObject(dc, old_f);
        return sz.cx;
    }
};

void ApplyObsidianWindowChrome(HWND hwnd)
{
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using FnDwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<FnDwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn) {
        const BOOL dark = TRUE;
        fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        fn(hwnd, 19 /* legacy dark mode */, &dark, sizeof(dark));
        const DWORD round_pref = 2; /* DWMWCP_ROUND */
        fn(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round_pref, sizeof(round_pref));
        const COLORREF caption = ui_theme::kTitleBg;
        fn(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
        const COLORREF text_col = ui_theme::kTextPrimary;
        fn(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text_col, sizeof(text_col));
        const COLORREF border = ui_theme::kCardBorder;
        fn(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    }
    FreeLibrary(dwm);
}

struct UiHealthRow {
    enum class Kind { Ok, Info, Warn, Err };
    Kind         kind = Kind::Ok;
    std::wstring icon;
    std::wstring text;
};

struct GuiState {
    fs::path base_root;
    UINT     dpi = 96;
    HFONT    font_title = nullptr;
    HFONT    font_bold = nullptr;
    HFONT    font_regular = nullptr;
    HFONT    font_small = nullptr;
    HFONT    font_mono = nullptr;
    HBRUSH   brush_input = nullptr;
    HWND     edit_game = nullptr;
    HWND     edit_wwmi = nullptr;
    HWND     btn_browse_game = nullptr;
    HWND     btn_browse_wwmi = nullptr;
    HWND     btn_install = nullptr;
    HWND     btn_uninstall = nullptr;
    HWND     btn_more = nullptr;
    HWND     hover_btn = nullptr;

    std::wstring pkg_version;
    std::wstring status_pill_text;
    int          status_tone = 0; // 0=fresh, 1=upgrade, 2=installed, 3=err
    std::wstring health_right_tag;
    std::vector<UiHealthRow> health_rows;
    bool         can_rollback = false;
    bool         can_uninstall = false;
    bool         update_checking = false;
    int          modal_depth = 0;
    std::unique_ptr<UpdateCheckResult> deferred_update_res;
    std::shared_ptr<std::atomic<HWND>> live_hwnd;

    int sc(int v) const { return MulDiv(v, static_cast<int>(dpi), 96); }
};

std::wstring GetWndString(HWND h)
{
    const int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::wstring s((size_t)len + 1, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    s.resize((size_t)len);
    return TrimWide(s);
}

struct ModalScope {
    HWND      owner = nullptr;
    GuiState* st = nullptr;
    bool      did_disable = false;

    ModalScope(HWND own, GuiState* s, bool manage_owner_enable) : owner(own), st(s)
    {
        if (!st) return;
        const bool first = (++st->modal_depth == 1);
        if (manage_owner_enable && owner && first && IsWindowEnabled(owner)) {
            EnableWindow(owner, FALSE);
            did_disable = true;
        }
    }
    ~ModalScope()
    {
        if (!st) return;
        const bool last = (--st->modal_depth <= 0);
        if (st->modal_depth < 0) st->modal_depth = 0;
        if (did_disable && owner) {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        if (owner && last && st->deferred_update_res) {
            auto* pending = st->deferred_update_res.release();
            if (!PostMessageW(owner, WM_APP_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(pending))) {
                delete pending;
            }
        }
    }
    ModalScope(const ModalScope&) = delete;
    ModalScope& operator=(const ModalScope&) = delete;
};

std::wstring BrowseFolderDialog(HWND owner, GuiState* st, const wchar_t* title)
{
    ModalScope scope(owner, st, false);
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t path[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? std::wstring(path) : std::wstring{};
}

LRESULT CALLBACK FlatBtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"WwmiOrigProc"));
    auto* st = reinterpret_cast<GuiState*>(GetPropW(hwnd, L"WwmiGuiState"));
    if (msg == WM_MOUSEMOVE && st) {
        if (st->hover_btn != hwnd) {
            st->hover_btn = hwnd;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE && st) {
        if (st->hover_btn == hwnd) {
            st->hover_btn = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"WwmiOrigProc");
        RemovePropW(hwnd, L"WwmiGuiState");
    }
    return orig ? CallWindowProcW(orig, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void AttachFlatButton(HWND btn, GuiState* st)
{
    SetPropW(btn, L"WwmiGuiState", st);
    WNDPROC orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(btn, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FlatBtnProc)));
    SetPropW(btn, L"WwmiOrigProc", reinterpret_cast<HANDLE>(orig));
}

void DrawFlatButton(const DRAWITEMSTRUCT* dis, GuiState* st)
{
    const int bw = dis->rcItem.right - dis->rcItem.left;
    const int bh = dis->rcItem.bottom - dis->rcItem.top;
    if (bw <= 0 || bh <= 0) return;

    const bool is_primary = (dis->CtlID == IDC_BTN_INSTALL || dis->CtlID == IDOK);
    const bool in_card = (dis->CtlID == IDC_BTN_BROWSE_GAME || dis->CtlID == IDC_BTN_BROWSE_WWMI);
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool hovered = (!disabled && st && st->hover_btn == dis->hwndItem);

    COLORREF bg_under = in_card ? ui_theme::kCardBg : ui_theme::kWindowBg;
    COLORREF fill = ui_theme::kSecBg;
    COLORREF border = ui_theme::kSecBorder;
    COLORREF text_fg = ui_theme::kTextPrimary;
    float border_px = 1.0f;

    if (is_primary) {
        border_px = 0.0f;
        if (disabled) {
            fill = RGB(56, 50, 36);
            text_fg = ui_theme::kTextDisabled;
        } else if (pressed) {
            fill = ui_theme::kGoldPress;
            text_fg = ui_theme::kGoldText;
        } else if (hovered) {
            fill = ui_theme::kGoldHover;
            text_fg = ui_theme::kGoldText;
        } else {
            fill = ui_theme::kGold;
            text_fg = ui_theme::kGoldText;
        }
    } else {
        if (disabled) {
            fill = ui_theme::kSecDisBg;
            border = ui_theme::kSecDisBdr;
            text_fg = ui_theme::kTextDisabled;
        } else if (pressed) {
            fill = ui_theme::kSecPress;
            border = ui_theme::kSecBorder;
        } else if (hovered) {
            fill = ui_theme::kSecHover;
            border = ui_theme::kSecHoverBdr;
        }
    }

    DibCanvas canvas(dis->hDC, bw, bh);
    if (!canvas.valid()) return;
    canvas.Fill(bg_under);
    const float rad = static_cast<float>(st ? st->sc(in_card ? 6 : 8) : 7);
    canvas.RoundRectAA(0, 0, bw, bh, rad, fill, border, border_px);

    const std::wstring label = GetWndString(dis->hwndItem);
    const bool is_more = (dis->CtlID == IDC_BTN_MORE);
    RECT trc{ 0, 0, is_more ? (bw - (st ? st->sc(14) : 14)) : bw, bh };
    HFONT f = (is_primary && st && st->font_bold) ? st->font_bold : (st ? st->font_regular : nullptr);
    canvas.Text(f, text_fg, label, trc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (is_more) {
        // Draw crisp font-independent downward triangle chevron so no Unicode glyph can ever render as tofu.
        const int s4 = st ? st->sc(4) : 4;
        const int cx = bw - (st ? st->sc(18) : 18);
        const int cy = bh / 2 - (st ? st->sc(1) : 1);
        for (int dy = 0; dy <= s4; ++dy) {
            const int half = s4 - dy;
            for (int dx = -half; dx <= half; ++dx) {
                const int px_i = cx + dx, py_i = cy + dy;
                if (px_i >= 0 && px_i < bw && py_i >= 0 && py_i < bh) {
                    BlendPixel(canvas.px[static_cast<size_t>(py_i) * static_cast<size_t>(bw) + static_cast<size_t>(px_i)], text_fg, 0.9f);
                }
            }
        }
    }
    BitBlt(dis->hDC, dis->rcItem.left, dis->rcItem.top, bw, bh, canvas.dc, 0, 0, SRCCOPY);
}

void RefreshGuiReport(HWND hwnd, GuiState* st)
{
    const std::wstring game_txt = GetWndString(st->edit_game);
    const std::wstring wwmi_txt = GetWndString(st->edit_wwmi);
    const auto pkg = ReadPackageProvenance(st->base_root);
    st->pkg_version = Utf8ToWide(pkg.version);
    st->health_rows.clear();
    st->can_rollback = false;
    st->can_uninstall = false;

    LedgerInfo led{};
    bool led_ok = false;
    try {
        led = ReadLedger(st->base_root);
        led_ok = true;
    } catch (const std::exception&) {
    }

    std::error_code ec;
    if (led_ok) {
        st->can_uninstall = (led.state == LedgerInfo::State::Installed || led.state == LedgerInfo::State::Installing);
        if (led.state == LedgerInfo::State::Installed && !led.upgraded_from.empty()) {
            const fs::path prev(Utf8ToWide(led.upgraded_from));
            st->can_rollback = IsValidAbsoluteWinPath(prev.wstring()) &&
                fs::is_regular_file(prev / L"host\\ReShade64-game-forward.dll", ec);
        } else if (led.state == LedgerInfo::State::RollingBack && !led.rollback_target.empty()) {
            st->can_rollback = IsValidAbsoluteWinPath(led.rollback_target.wstring()) &&
                fs::is_regular_file(led.rollback_target / L"host\\ReShade64-game-forward.dll", ec);
        }
    }

    const fs::path win64 = ResolveWin64FromAnyGameDir(game_txt);
    std::wstring primary_label = TrW(L"一键安装启用", L"Install & Enable");
    st->status_tone = 0;
    st->status_pill_text = TrW(L"未安装", L"Not Installed");
    st->health_right_tag = (g_zh_ui ? L"当前包 " : L"Package ") + st->pkg_version;

    if (win64.empty()) {
        st->status_tone = 3;
        st->status_pill_text = TrW(L"未定位游戏", L"Game Not Found");
        st->health_rows.push_back({
            UiHealthRow::Kind::Err, L"\u2715",
            TrW(L"未找到 Client-Win64-Shipping.exe，请点击上方「浏览」选择《鸣潮》目录。",
                L"Client-Win64-Shipping.exe not found. Click Browse above to select Wuthering Waves.")
        });
    } else {
        const auto info = DescribeInstallAction(win64, st->base_root);
        if (info.mount_state == MountState::UpgradeReady) {
            st->status_tone = 1;
            const std::wstring prev_name = info.owner_base_root.filename().wstring();
            const std::wstring clean_src = info.source_version.empty() ? TrW(L"旧版", L"Legacy") : info.source_version;
            if (info.action == InstallActionKind::Upgrade) {
                primary_label = g_zh_ui ? (L"一键升级到 " + info.target_version) : (L"Upgrade to " + info.target_version);
                st->status_pill_text = TrW(L"可升级", L"Upgrade Ready");
                st->health_right_tag = clean_src + L" \u2192 " + info.target_version;
                st->health_rows.push_back({
                    UiHealthRow::Kind::Info, L"\u2197",
                    g_zh_ui
                        ? (L"检测到旧版（" + prev_name + L"）；一键升级将自动继承配置，并完整保留旧目录。")
                        : (L"Previous install (" + prev_name + L") detected; upgrade migrates settings and preserves old folder.")
                });
            } else if (info.action == InstallActionKind::Rollback) {
                primary_label = g_zh_ui ? (L"切回 " + info.target_version) : (L"Switch to " + info.target_version);
                st->status_pill_text = TrW(L"可切换版本", L"Switch Ready");
                st->health_right_tag = clean_src + L" \u2192 " + info.target_version;
                st->health_rows.push_back({ UiHealthRow::Kind::Info, L"\u2190", info.message });
            } else {
                primary_label = TrW(L"切换至此目录", L"Switch to This Folder");
                st->status_pill_text = TrW(L"可切换目录", L"Switch Ready");
                st->health_rows.push_back({ UiHealthRow::Kind::Info, L"\u2197", info.message });
            }
        } else if (info.mount_state == MountState::Installed) {
            st->status_tone = 2;
            primary_label = TrW(L"重新应用配置", L"Reapply Setup");
            st->status_pill_text = TrW(L"已安装就绪", L"Installed");
            const std::wstring inst_v = FormatInstalledVersionW(led);
            st->health_right_tag = g_zh_ui
                ? (L"已装 " + inst_v + (st->can_rollback ? L" · 可回滚旧版" : L""))
                : (L"Installed " + inst_v + (st->can_rollback ? L" · Rollback ready" : L""));
            st->health_rows.push_back({
                UiHealthRow::Kind::Ok, L"\u2713",
                TrW(L"已就绪：游戏目录已配置好引导文件，直接以 DX12 模式启动游戏即可（无需后台运行本工具）。",
                    L"Ready: game bootstrap configured. Launch Wuthering Waves in DX12 mode (no background process needed).")
            });
            if (st->can_rollback) {
                const fs::path prev(Utf8ToWide(led.upgraded_from));
                st->health_rows.push_back({
                    UiHealthRow::Kind::Info, L"\u2190",
                    g_zh_ui
                        ? (L"升级前的旧版目录已保留（" + prev.filename().wstring() + L"），可在右下「更多操作」中一键回滚。")
                        : (L"Previous folder preserved (" + prev.filename().wstring() + L"); roll back anytime via More menu.")
                });
            }
        } else if (info.mount_state == MountState::RollingBack) {
            st->status_tone = 3;
            primary_label = TrW(L"继续完成回滚", L"Resume Rollback");
            st->status_pill_text = TrW(L"回滚待恢复", L"Rollback Pending");
            st->health_rows.push_back({ UiHealthRow::Kind::Warn, L"!", info.message });
        } else if (info.mount_state == MountState::Drifted || info.mount_state == MountState::Installing) {
            st->status_tone = 1;
            primary_label = TrW(L"修复安装", L"Repair Setup");
            st->status_pill_text = TrW(L"需修复安装", L"Repair Needed");
            st->health_rows.push_back({ UiHealthRow::Kind::Warn, L"!", info.message });
        } else if (info.mount_state == MountState::Conflict || info.mount_state == MountState::ActiveElsewhere) {
            st->status_tone = 3;
            st->status_pill_text = TrW(L"目录冲突", L"Conflict");
            st->health_rows.push_back({ UiHealthRow::Kind::Err, L"\u2715", info.message });
        } else {
            st->health_rows.push_back({
                UiHealthRow::Kind::Ok, L"\u2713",
                TrW(L"已找到游戏目录；尚未安装，点击下方「一键安装启用」即可生效。",
                    L"Game directory located; not installed yet. Click Install & Enable below to set up.")
            });
        }
    }

    // WWMI & Core & RabbitFX concise combined row
    const fs::path wwmi_p = wwmi_txt.empty() ? fs::path{} : fs::path(wwmi_txt);
    if (wwmi_p.empty() || !fs::is_regular_file(wwmi_p / L"d3dx.ini", ec)) {
        st->health_rows.push_back({
            UiHealthRow::Kind::Warn, L"!",
            TrW(L"Mod 目录尚未关联 d3dx.ini（点安装时将自动询问初始化骨架，或浏览已有 XXMI\\WWMI）。",
                L"No d3dx.ini in Mod folder yet (Install can auto-initialize a skeleton, or browse XXMI\\WWMI).")
        });
    } else {
        const bool has_core = fs::is_regular_file(wwmi_p / L"Core\\WWMI\\WuWa-Model-Importer.ini", ec);
        const std::wstring ver = has_core ? DetectWwmiCoreVersion(wwmi_p) : L"";
        const int rfx = CountRabbitFxInstances(wwmi_p);
        if (has_core && rfx == 1) {
            st->health_rows.push_back({
                UiHealthRow::Kind::Ok, L"\u2713",
                g_zh_ui
                    ? (L"WWMI 核心包齐全" + (ver.empty() ? L"" : (L"（WWMI " + ver + L"）")) + L" · RabbitFX 实例唯一 (1 份)。")
                    : (L"Core\\WWMI ready" + (ver.empty() ? L"" : (L" (WWMI " + ver + L")")) + L" · RabbitFX unique (1 copy).")
            });
        } else if (rfx > 1) {
            st->health_rows.push_back({
                UiHealthRow::Kind::Err, L"\u2715",
                g_zh_ui
                    ? (L"检测到 " + std::to_wstring(rfx) + L" 份重复的 RabbitFX.ini！请只保留 1 份以免着色器冲突。")
                    : (L"Detected " + std::to_wstring(rfx) + L" duplicate RabbitFX.ini copies! Keep only ONE.")
            });
        } else {
            std::wstring detail;
            if (!has_core) detail += TrW(L"缺少官方 Core\\WWMI 核心包", L"Missing official Core\\WWMI");
            if (rfx == 0) {
                if (!detail.empty()) detail += L" · ";
                detail += TrW(L"Mods 下未检测到 RabbitFX.ini（发光皮肤需放入 1 份）", L"RabbitFX.ini not found under Mods\\");
            }
            st->health_rows.push_back({ UiHealthRow::Kind::Warn, L"!", detail });
        }
    }

    if (!win64.empty()) {
        const auto extra_addons = ScanThirdPartyWin64Addons(win64);
        if (!extra_addons.empty()) {
            std::wstring list_str;
            for (size_t i = 0; i < extra_addons.size(); ++i) {
                if (i) list_str += L", ";
                list_str += extra_addons[i];
            }
            st->health_rows.push_back({
                UiHealthRow::Kind::Warn, L"!",
                g_zh_ui
                    ? (L"Win64 存在其他 ReShade 插件（" + list_str + L"），如需共存请放入外部 reshade-addons\\。")
                    : (L"Other ReShade add-on(s) in Win64 (" + list_str + L"); copy into reshade-addons\\ to coexist.")
            });
        }
    }

    if (st->btn_install) {
        SetWindowTextW(st->btn_install, primary_label.c_str());
        InvalidateRect(st->btn_install, nullptr, FALSE);
    }
    if (st->btn_uninstall) {
        EnableWindow(st->btn_uninstall, st->can_uninstall ? TRUE : FALSE);
        InvalidateRect(st->btn_uninstall, nullptr, FALSE);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void PaintMainWindow(HWND hwnd, HDC hdc, GuiState* st)
{
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int cw = cr.right - cr.left;
    const int ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0) return;

    DibCanvas c(hdc, cw, ch);
    if (!c.valid()) return;
    c.Fill(ui_theme::kWindowBg);

    const auto s = [&](int v) { return st->sc(v); };

    // 1. Hero Header
    const int hdr_y = s(18);
    // Subtle gold emblem square
    c.RoundRectAA(s(20), hdr_y + s(7), s(10), s(10), static_cast<float>(s(3)), ui_theme::kGold, ui_theme::kGold, 0.0f);
    const std::wstring brand = L"WWMI DX12+RT";
    const int brand_w = c.MeasureTextWidth(st->font_title, brand);
    c.Text(st->font_title, ui_theme::kTextPrimary, brand, RECT{ s(38), hdr_y, s(38) + brand_w + s(8), hdr_y + s(26) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Version pill
    int px = s(48) + brand_w;
    const int ver_tw = c.MeasureTextWidth(st->font_small, st->pkg_version);
    const int ver_pw = ver_tw + s(18);
    c.RoundRectAA(px, hdr_y + s(3), ver_pw, s(20), static_cast<float>(s(10)), ui_theme::kGoldSoftBg, ui_theme::kGoldSoftBdr, 1.0f);
    c.Text(st->font_small, ui_theme::kGoldSoftFg, st->pkg_version, RECT{ px, hdr_y + s(3), px + ver_pw, hdr_y + s(23) }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Live status pill
    px += ver_pw + s(10);
    COLORREF sp_bg = ui_theme::kFreshBg, sp_fg = ui_theme::kFreshFg, sp_dot = ui_theme::kFreshDot;
    if (st->status_tone == 1) { sp_bg = ui_theme::kGoldSoftBg; sp_fg = ui_theme::kGoldSoftFg; sp_dot = ui_theme::kGold; }
    else if (st->status_tone == 2) { sp_bg = ui_theme::kOkBg; sp_fg = ui_theme::kOkFg; sp_dot = ui_theme::kOkDot; }
    else if (st->status_tone == 3) { sp_bg = ui_theme::kErrBg; sp_fg = ui_theme::kErrFg; sp_dot = ui_theme::kErrFg; }
    const int st_tw = c.MeasureTextWidth(st->font_small, st->status_pill_text);
    const int st_pw = st_tw + s(28);
    c.RoundRectAA(px, hdr_y + s(3), st_pw, s(20), static_cast<float>(s(10)), sp_bg, sp_bg, 0.0f);
    c.CircleAA(static_cast<float>(px + s(11)), static_cast<float>(hdr_y + s(13)), static_cast<float>(s(3)), sp_dot);
    c.Text(st->font_small, sp_fg, st->status_pill_text, RECT{ px + s(18), hdr_y + s(3), px + st_pw - s(6), hdr_y + s(23) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Right subtle subtitle
    c.Text(st->font_small, ui_theme::kTextMuted,
        TrW(L"DX12 + 光追 Mod 引导", L"DX12 + RT Mod Loader"),
        RECT{ cw - s(180), hdr_y + s(3), cw - s(20), hdr_y + s(23) }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 2. Path Configuration Card
    c.RoundRectAA(s(20), s(58), cw - s(40), s(98), static_cast<float>(s(10)), ui_theme::kCardBg, ui_theme::kCardBorder, 1.0f);
    c.Text(st->font_regular, ui_theme::kTextMuted, TrW(L"游戏目录", L"Game Dir"),
        RECT{ s(34), s(72), s(106), s(102) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    c.RoundRectAA(s(108), s(72), cw - s(212), s(30), static_cast<float>(s(6)), ui_theme::kInputBg, ui_theme::kInputBorder, 1.0f);

    c.Text(st->font_regular, ui_theme::kTextMuted, TrW(L"Mod 目录", L"Mod Dir"),
        RECT{ s(34), s(112), s(106), s(142) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    c.RoundRectAA(s(108), s(112), cw - s(212), s(30), static_cast<float>(s(6)), ui_theme::kInputBg, ui_theme::kInputBorder, 1.0f);

    // 3. Health Checklist Card
    const int hc_y = s(168), hc_h = s(144);
    c.RoundRectAA(s(20), hc_y, cw - s(40), hc_h, static_cast<float>(s(10)), ui_theme::kCardBg, ui_theme::kCardBorder, 1.0f);
    c.Text(st->font_small, ui_theme::kTextMuted,
        TrW(L"环境检测（游戏目录仅写入 2 个引导文件，不改动游戏本体）", L"Environment Check (only 2 bootstrap files in Win64)"),
        RECT{ s(34), hc_y + s(8), cw - s(210), hc_y + s(28) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    c.Text(st->font_small, ui_theme::kGoldSoftFg, st->health_right_tag,
        RECT{ cw - s(240), hc_y + s(8), cw - s(34), hc_y + s(28) }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    c.HLine(s(32), cw - s(32), hc_y + s(34), ui_theme::kCardBorder);

    int ry = hc_y + s(44);
    const int row_max_y = hc_y + hc_h - s(10);
    for (const auto& row : st->health_rows) {
        if (ry >= row_max_y) break;
        COLORREF ic_col = ui_theme::kOkFg;
        if (row.kind == UiHealthRow::Kind::Info) ic_col = ui_theme::kGoldSoftFg;
        else if (row.kind == UiHealthRow::Kind::Warn) ic_col = ui_theme::kWarnFg;
        else if (row.kind == UiHealthRow::Kind::Err) ic_col = ui_theme::kErrFg;

        c.Text(st->font_bold, ic_col, row.icon, RECT{ s(34), ry, s(52), ry + s(20) }, DT_CENTER | DT_TOP | DT_SINGLELINE);
        RECT calc{ s(56), ry, cw - s(36), row_max_y };
        HGDIOBJ old_f = SelectObject(c.dc, st->font_regular);
        const int needed_h = DrawTextW(c.dc, row.text.c_str(), static_cast<int>(row.text.size()), &calc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(c.dc, old_f);
        const int use_h = std::min(std::max(needed_h, s(20)), row_max_y - ry);
        c.Text(st->font_regular, ui_theme::kTextPrimary, row.text, RECT{ s(56), ry, cw - s(36), ry + use_h }, DT_LEFT | DT_WORDBREAK);
        ry += use_h + s(8);
    }

    BitBlt(hdc, 0, 0, cw, ch, c.dc, 0, 0, SRCCOPY);
}

struct ObsidianDialogState {
    GuiState*    parent_st = nullptr;
    std::wstring badge;
    int          badge_tone = 1; // 0=neutral, 1=gold, 2=ok, 3=err
    std::wstring body;
    std::wstring ok_label;
    std::wstring cancel_label;
    HWND         btn_ok = nullptr;
    HWND         btn_cancel = nullptr;
    bool         done = false;
    bool         accepted = false;
    int          card_h = 0;
};

LRESULT CALLBACK ObsidianDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* ds = reinterpret_cast<ObsidianDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ds = static_cast<ObsidianDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));
        ApplyObsidianWindowChrome(hwnd);
        if (!ds || !ds->parent_st) return 0;

        auto* st = ds->parent_st;
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const int cw = cr.right - cr.left;
        const int ch = cr.bottom - cr.top;
        const auto s = [&](int v) { return st->sc(v); };

        const int bw_ok = s(ds->ok_label.size() > 4 ? 128 : 96);
        const int bw_cancel = s(ds->cancel_label.size() > 4 ? 112 : 88);
        const int bh = s(34);
        const int by = ch - s(16) - bh;
        int rx = cw - s(20);

        if (!ds->cancel_label.empty()) {
            rx -= bw_cancel;
            ds->btn_cancel = CreateWindowExW(0, L"BUTTON", ds->cancel_label.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                rx, by, bw_cancel, bh, hwnd, reinterpret_cast<HMENU>((INT_PTR)IDCANCEL), cs->hInstance, nullptr);
            SendMessageW(ds->btn_cancel, WM_SETFONT, reinterpret_cast<WPARAM>(st->font_regular), TRUE);
            AttachFlatButton(ds->btn_cancel, st);
            rx -= s(10);
        }
        rx -= bw_ok;
        ds->btn_ok = CreateWindowExW(0, L"BUTTON", ds->ok_label.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            rx, by, bw_ok, bh, hwnd, reinterpret_cast<HMENU>((INT_PTR)IDOK), cs->hInstance, nullptr);
        SendMessageW(ds->btn_ok, WM_SETFONT, reinterpret_cast<WPARAM>(st->font_bold), TRUE);
        AttachFlatButton(ds->btn_ok, st);
        SetFocus(ds->btn_ok);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (ds && ds->parent_st) {
            auto* st = ds->parent_st;
            RECT cr{};
            GetClientRect(hwnd, &cr);
            const int cw = cr.right - cr.left;
            const int ch = cr.bottom - cr.top;
            DibCanvas c(hdc, cw, ch);
            if (c.valid()) {
                const auto s = [&](int v) { return st->sc(v); };
                c.Fill(ui_theme::kWindowBg);

                int top_y = s(16);
                if (!ds->badge.empty()) {
                    COLORREF bg = ui_theme::kFreshBg, fg = ui_theme::kFreshFg, dot = ui_theme::kFreshDot;
                    if (ds->badge_tone == 1) { bg = ui_theme::kGoldSoftBg; fg = ui_theme::kGoldSoftFg; dot = ui_theme::kGold; }
                    else if (ds->badge_tone == 2) { bg = ui_theme::kOkBg; fg = ui_theme::kOkFg; dot = ui_theme::kOkDot; }
                    else if (ds->badge_tone == 3) { bg = ui_theme::kErrBg; fg = ui_theme::kErrFg; dot = ui_theme::kErrFg; }
                    const int tw = c.MeasureTextWidth(st->font_small, ds->badge);
                    const int pw = std::min(cw - s(40), tw + s(28));
                    c.RoundRectAA(s(20), top_y, pw, s(22), static_cast<float>(s(11)), bg, bg, 0.0f);
                    c.CircleAA(static_cast<float>(s(31)), static_cast<float>(top_y + s(11)), static_cast<float>(s(3)), dot);
                    c.Text(st->font_small, fg, ds->badge, RECT{ s(38), top_y, s(20) + pw - s(8), top_y + s(22) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    top_y += s(32);
                }

                const int card_h = std::max(s(64), ch - top_y - s(64));
                c.RoundRectAA(s(20), top_y, cw - s(40), card_h, static_cast<float>(s(10)), ui_theme::kCardBg, ui_theme::kCardBorder, 1.0f);
                c.Text(st->font_regular, ui_theme::kTextPrimary, ds->body,
                    RECT{ s(34), top_y + s(12), cw - s(34), top_y + card_h - s(10) }, DT_LEFT | DT_TOP | DT_WORDBREAK);
                BitBlt(hdc, 0, 0, cw, ch, c.dc, 0, 0, SRCCOPY);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        if (ds && ds->parent_st) {
            DrawFlatButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp), ds->parent_st);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (ds) {
            const int id = LOWORD(wp);
            if (id == IDOK) {
                ds->accepted = true;
                ds->done = true;
                return 0;
            }
            if (id == IDCANCEL) {
                ds->accepted = false;
                ds->done = true;
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        if (ds) {
            ds->accepted = false;
            ds->done = true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShowObsidianDialog(
    HWND owner,
    GuiState* st,
    const std::wstring& title,
    const std::wstring& badge,
    int badge_tone,
    const std::wstring& body,
    const std::wstring& ok_label,
    const std::wstring& cancel_label = L"")
{
    if (!st) return MessageBoxW(owner, body.c_str(), title.c_str(),
        (cancel_label.empty() ? MB_OK : MB_YESNO) | MB_ICONINFORMATION) == IDYES;

    static bool reg_done = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!reg_done) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ObsidianDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"WwmiObsidianDlgWnd";
        RegisterClassExW(&wc);
        reg_done = true;
    }

    const auto s = [&](int v) { return st->sc(v); };
    const int cw = s(490);
    int text_h = s(72);
    if (HDC tmp_dc = GetDC(owner)) {
        HGDIOBJ old_f = SelectObject(tmp_dc, st->font_regular);
        RECT calc{ 0, 0, cw - s(68), 0 };
        DrawTextW(tmp_dc, body.c_str(), static_cast<int>(body.size()), &calc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(tmp_dc, old_f);
        ReleaseDC(owner, tmp_dc);
        text_h = std::clamp(static_cast<int>(calc.bottom - calc.top), s(44), s(280));
    }
    const int badge_h = badge.empty() ? 0 : s(32);
    const int ch = s(16) + badge_h + (text_h + s(26)) + s(64);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, cw, ch };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT orc{};
    if (owner && GetWindowRect(owner, &orc)) {
        x = orc.left + ((orc.right - orc.left) - ww) / 2;
        y = orc.top + ((orc.bottom - orc.top) - wh) / 2;
    }

    ObsidianDialogState ds{};
    ds.parent_st = st;
    ds.badge = badge;
    ds.badge_tone = badge_tone;
    ds.body = body;
    ds.ok_label = ok_label;
    ds.cancel_label = cancel_label;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WwmiObsidianDlgWnd", title.c_str(),
        style, x, y, ww, wh, owner, nullptr, hInst, &ds);
    if (!dlg) return false;

    bool got_quit = false;
    WPARAM quit_code = 0;
    {
        ModalScope scope(owner, st, true);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);

        MSG m{};
        while (!ds.done) {
            const BOOL gm = GetMessageW(&m, nullptr, 0, 0);
            if (gm <= 0) {
                if (gm == 0) { got_quit = true; quit_code = m.wParam; }
                break;
            }
            if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
                ds.accepted = false;
                ds.done = true;
                break;
            }
            if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {
                const HWND focused = GetFocus();
                ds.accepted = !(ds.btn_cancel && focused == ds.btn_cancel);
                ds.done = true;
                break;
            }
            if (!IsDialogMessageW(dlg, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        if (st->hover_btn == ds.btn_ok || st->hover_btn == ds.btn_cancel) st->hover_btn = nullptr;
        DestroyWindow(dlg);
    }
    if (got_quit) {
        PostQuitMessage(static_cast<int>(quit_code));
    }
    return ds.accepted;
}

void ShowMoreMenu(HWND hwnd, GuiState* st)
{
    ModalScope scope(hwnd, st, false);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (st->can_rollback ? MF_ENABLED : MF_GRAYED), IDM_ROLLBACK,
        TrW(L"回滚至升级前旧版", L"Roll Back to Previous Version"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (st->update_checking ? MF_GRAYED : MF_ENABLED), IDM_CHECK_UPDATE,
        TrW(L"检查更新 (GitHub Releases)", L"Check for Updates (GitHub)"));
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, TrW(L"重新检测环境", L"Refresh Health Check"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN_MODS, TrW(L"打开 Mods 目录", L"Open Mods Folder"));
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOGS, TrW(L"打开运行日志目录", L"Open Logs Folder"));
    AppendMenuW(menu, MF_STRING, IDM_INIT_WWMI, TrW(L"新建标准 WWMI 骨架", L"Initialize WWMI Skeleton"));

    RECT rc{};
    GetWindowRect(st->btn_more, &rc);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_LEFTBUTTON, rc.right, rc.top - st->sc(4), 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<GuiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<GuiState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->dpi = GetDpiForWindow(hwnd);
        if (st->dpi == 0) st->dpi = 96;
        ApplyObsidianWindowChrome(hwnd);

        const wchar_t* face = TrW(L"Microsoft YaHei UI", L"Segoe UI");
        st->font_title   = CreateFontW(-st->sc(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        st->font_bold    = CreateFontW(-st->sc(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        st->font_regular = CreateFontW(-st->sc(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        st->font_small   = CreateFontW(-st->sc(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        st->font_mono    = CreateFontW(-st->sc(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        st->brush_input  = CreateSolidBrush(ui_theme::kInputBg);
        st->live_hwnd    = std::make_shared<std::atomic<HWND>>(hwnd);

        RECT cr{};
        GetClientRect(hwnd, &cr);
        const int cw = cr.right - cr.left;
        const auto s = [&](int v) { return st->sc(v); };

        st->edit_game = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            s(118), s(78), cw - s(232), s(18), hwnd, reinterpret_cast<HMENU>((INT_PTR)IDC_EDIT_GAME), cs->hInstance, nullptr);
        SendMessageW(st->edit_game, WM_SETFONT, reinterpret_cast<WPARAM>(st->font_regular), TRUE);

        st->edit_wwmi = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            s(118), s(118), cw - s(232), s(18), hwnd, reinterpret_cast<HMENU>((INT_PTR)IDC_EDIT_WWMI), cs->hInstance, nullptr);
        SendMessageW(st->edit_wwmi, WM_SETFONT, reinterpret_cast<WPARAM>(st->font_regular), TRUE);

        auto mk_btn = [&](const wchar_t* txt, int x, int y, int w, int h, int id) {
            HWND b = CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                x, y, w, h, hwnd, reinterpret_cast<HMENU>((INT_PTR)id), cs->hInstance, nullptr);
            SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(st->font_regular), TRUE);
            AttachFlatButton(b, st);
            return b;
        };

        st->btn_browse_game = mk_btn(TrW(L"浏览", L"Browse"), cw - s(96), s(72), s(64), s(30), IDC_BTN_BROWSE_GAME);
        st->btn_browse_wwmi = mk_btn(TrW(L"浏览", L"Browse"), cw - s(96), s(112), s(64), s(30), IDC_BTN_BROWSE_WWMI);

        const int fy = s(328), fh = s(38);
        st->btn_install   = mk_btn(TrW(L"一键安装启用", L"Install & Enable"), s(20), fy, s(208), fh, IDC_BTN_INSTALL);
        st->btn_uninstall = mk_btn(TrW(L"卸载", L"Uninstall"), s(240), fy, s(88), fh, IDC_BTN_UNINSTALL);
        st->btn_more      = mk_btn(TrW(L"更多操作", L"More"), cw - s(132), fy, s(112), fh, IDC_BTN_MORE);

        const fs::path g = AutoDiscoverGameWin64(st->base_root);
        const fs::path w = AutoDiscoverWwmiDir(st->base_root, g);
        if (!g.empty()) SetWindowTextW(st->edit_game, g.c_str());
        if (!w.empty()) SetWindowTextW(st->edit_wwmi, w.c_str());
        RefreshGuiReport(hwnd, st);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (st) PaintMainWindow(hwnd, hdc, st);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        if (!st) break;
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetTextColor(hdc, ui_theme::kTextInput);
        SetBkColor(hdc, ui_theme::kInputBg);
        return reinterpret_cast<LRESULT>(st->brush_input);
    }
    case WM_DRAWITEM: {
        if (!st) break;
        DrawFlatButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp), st);
        return TRUE;
    }
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if ((id == IDC_EDIT_GAME || id == IDC_EDIT_WWMI) && code == EN_KILLFOCUS) {
            RefreshGuiReport(hwnd, st);
            return 0;
        }
        if (id == IDC_BTN_BROWSE_GAME) {
            const std::wstring p = BrowseFolderDialog(hwnd, st, TrW(
                L"请选择《鸣潮》安装目录（包含 Wuthering Waves Game 或 Client-Win64-Shipping.exe）",
                L"Select Wuthering Waves install folder (containing Wuthering Waves Game or Client-Win64-Shipping.exe)"));
            if (!p.empty()) {
                const fs::path resolved = ResolveWin64FromAnyGameDir(p);
                SetWindowTextW(st->edit_game, (resolved.empty() ? p : resolved.wstring()).c_str());
                RefreshGuiReport(hwnd, st);
            }
        } else if (id == IDC_BTN_BROWSE_WWMI) {
            const std::wstring p = BrowseFolderDialog(hwnd, st, TrW(
                L"请选择包含 d3dx.ini 与 Mods 的 WWMI 目录（如 XXMI Launcher\\WWMI）",
                L"Select WWMI folder containing d3dx.ini and Mods\\ (e.g. XXMI Launcher\\WWMI)"));
            if (!p.empty()) {
                SetWindowTextW(st->edit_wwmi, p.c_str());
                RefreshGuiReport(hwnd, st);
            }
        } else if (id == IDC_BTN_MORE) {
            ShowMoreMenu(hwnd, st);
        } else if (id == IDM_INIT_WWMI) {
            fs::path target = GetWndString(st->edit_wwmi);
            if (target.empty()) target = st->base_root / L"WWMI";
            std::wstring err;
            if (InitWwmiSkeleton(target, err)) {
                SetWindowTextW(st->edit_wwmi, NormWinPath(target).c_str());
                RefreshGuiReport(hwnd, st);
                const std::wstring ok_msg = g_zh_ui
                    ? (L"已创建标准 WWMI 目录骨架：\r\n" + NormWinPath(target) + L"\r\n\r\n请确保放入官方 Core\\WWMI 与 Mods\\RabbitFX。")
                    : (L"Initialized standard WWMI skeleton:\r\n" + NormWinPath(target) + L"\r\n\r\nPlace official Core\\WWMI and Mods\\RabbitFX inside before launching.");
                ShowObsidianDialog(hwnd, st, L"WWMI DX12+RT", TrW(L"骨架已创建", L"Skeleton Created"), 2, ok_msg, TrW(L"确定", L"OK"));
            } else {
                ShowObsidianDialog(hwnd, st, TrW(L"错误", L"Error"), TrW(L"初始化失败", L"Init Failed"), 3, err, TrW(L"确定", L"OK"));
            }
        } else if (id == IDC_BTN_INSTALL) {
            // If an interrupted rollback is pending, Primary CTA resumes rollback cleanly.
            try {
                const auto cur_led = ReadLedger(st->base_root);
                if (cur_led.state == LedgerInfo::State::RollingBack) {
                    std::wstring res_msg;
                    const bool ok = PerformRollback(st->base_root, {}, res_msg);
                    RefreshGuiReport(hwnd, st);
                    ShowObsidianDialog(hwnd, st, L"WWMI DX12+RT",
                        ok ? TrW(L"回滚完成", L"Rollback Complete") : TrW(L"回滚失败", L"Rollback Failed"),
                        ok ? 2 : 3, res_msg, TrW(L"确定", L"OK"));
                    return 0;
                }
            } catch (const std::exception&) {
            }
            std::wstring game_s = GetWndString(st->edit_game);
            std::wstring wwmi_s = GetWndString(st->edit_wwmi);
            std::error_code ec;
            const fs::path win64_chk = ResolveWin64FromAnyGameDir(game_s);
            const auto act = win64_chk.empty() ? InstallActionInfo{} : DescribeInstallAction(win64_chk, st->base_root);
            if (act.mount_state != MountState::UpgradeReady &&
                (wwmi_s.empty() || !fs::is_regular_file(fs::path(wwmi_s) / L"d3dx.ini", ec))) {
                const fs::path target = wwmi_s.empty() ? (st->base_root / L"WWMI") : fs::path(wwmi_s);
                const std::wstring ask = g_zh_ui
                    ? (L"未找到 d3dx.ini，是否直接在以下目录初始化标准 WWMI 骨架并继续安装？\r\n\r\n" + NormWinPath(target))
                    : (L"d3dx.ini not found. Initialize a standard WWMI skeleton here and continue installing?\r\n\r\n" + NormWinPath(target));
                if (ShowObsidianDialog(hwnd, st, L"WWMI DX12+RT", TrW(L"初始化 WWMI 目录", L"Initialize WWMI"), 1,
                        ask, TrW(L"初始化并安装", L"Init & Install"), TrW(L"取消", L"Cancel"))) {
                    std::wstring init_err;
                    if (!InitWwmiSkeleton(target, init_err)) {
                        ShowObsidianDialog(hwnd, st, TrW(L"错误", L"Error"), TrW(L"初始化失败", L"Init Failed"), 3, init_err, TrW(L"确定", L"OK"));
                        return 0;
                    }
                    wwmi_s = NormWinPath(target);
                    SetWindowTextW(st->edit_wwmi, wwmi_s.c_str());
                } else {
                    return 0;
                }
            }
            std::wstring res_msg;
            const bool ok = PerformInstall(game_s, wwmi_s, st->base_root, res_msg);
            RefreshGuiReport(hwnd, st);
            ShowObsidianDialog(hwnd, st, L"WWMI DX12+RT",
                ok ? TrW(L"安装就绪", L"Installed") : TrW(L"操作未完成", L"Install Failed"),
                ok ? 2 : 3, res_msg, TrW(L"确定", L"OK"));
        } else if (id == IDM_ROLLBACK) {
            if (ShowObsidianDialog(hwnd, st, TrW(L"回滚旧版", L"Rollback"), TrW(L"确认回滚", L"Confirm Rollback"), 1,
                    TrW(L"确定回滚至升级前保留的旧版目录吗？当前版本目录仍会保留，随时可再次升级。",
                        L"Roll back to the previous installation folder? Your current folder stays intact."),
                    TrW(L"确认回滚", L"Roll Back"), TrW(L"取消", L"Cancel"))) {
                std::wstring res_msg;
                const bool ok = PerformRollback(st->base_root, {}, res_msg);
                RefreshGuiReport(hwnd, st);
                ShowObsidianDialog(hwnd, st, L"WWMI DX12+RT",
                    ok ? TrW(L"回滚完成", L"Rollback Complete") : TrW(L"回滚失败", L"Rollback Failed"),
                    ok ? 2 : 3, res_msg, TrW(L"确定", L"OK"));
            }
        } else if (id == IDC_BTN_UNINSTALL) {
            bool aborting_upgrade = false;
            try {
                const auto cur_led = ReadLedger(st->base_root);
                aborting_upgrade = (cur_led.state == LedgerInfo::State::Installing && !cur_led.previous_owner.empty());
            } catch (const std::exception&) {
            }
            const std::wstring dlg_badge = aborting_upgrade
                ? TrW(L"中止未完成升级", L"Abort Interrupted Upgrade")
                : TrW(L"恢复官方原版", L"Restore Vanilla Game");
            const std::wstring dlg_body = aborting_upgrade
                ? TrW(L"检测到上次升级尚未完成。继续卸载将还原升级前保留的旧版引导文件（若恢复备份完整）或清理半成品。\r\n\r\n您的 Mod 皮肤文件不会被删除。",
                      L"An interrupted upgrade was detected. Continuing will restore the previous version's bootstrap files (if recovery backups are intact) or remove partial files.\r\n\r\nYour Mods are never deleted.")
                : TrW(L"将安全移走游戏目录中的 2 个引导文件（dxgi.dll 与 ReShade.ini），使游戏恢复为官方原版。\r\n\r\n您的 Mod 皮肤文件与外部配置不会被删除。",
                      L"This safely moves the 2 bootstrap files (dxgi.dll and ReShade.ini) out of Win64 to restore the vanilla game.\r\n\r\nYour Mods and external settings are never deleted.");
            if (ShowObsidianDialog(hwnd, st, TrW(L"确认卸载", L"Confirm Uninstall"), dlg_badge, 1,
                    dlg_body, TrW(L"确认卸载", L"Uninstall"), TrW(L"取消", L"Cancel"))) {
                std::wstring res_msg;
                const bool ok = PerformUninstall(st->base_root, res_msg);
                RefreshGuiReport(hwnd, st);
                const bool restored_prev = ok && (res_msg.find(L"还原原版本") != std::wstring::npos ||
                    res_msg.find(L"restored previous") != std::wstring::npos);
                const std::wstring ok_badge = restored_prev
                    ? TrW(L"已还原旧版", L"Restored Previous")
                    : TrW(L"已卸载（恢复原版）", L"Uninstalled");
                ShowObsidianDialog(hwnd, st, L"WWMI DX12+RT",
                    ok ? ok_badge : TrW(L"卸载失败", L"Uninstall Failed"),
                    ok ? 2 : 3, res_msg, TrW(L"确定", L"OK"));
            }
        } else if (id == IDM_REFRESH) {
            RefreshGuiReport(hwnd, st);
        } else if (id == IDM_OPEN_MODS) {
            const fs::path wwmi = GetWndString(st->edit_wwmi);
            const fs::path mods = wwmi / L"Mods";
            std::error_code ec;
            if (fs::is_directory(mods, ec)) ShellExecuteW(hwnd, L"open", mods.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else ShowObsidianDialog(hwnd, st, TrW(L"提示", L"Notice"), TrW(L"目录不存在", L"Not Found"), 1,
                TrW(L"Mods 目录不存在，请先设置有效的 WWMI 目录。",
                    L"Mods folder does not exist yet. Select a valid WWMI folder first."),
                TrW(L"确定", L"OK"));
        } else if (id == IDM_OPEN_LOGS) {
            const fs::path logs = st->base_root / L"runtime\\logs";
            std::error_code ec;
            fs::create_directories(logs, ec);
            ShellExecuteW(hwnd, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == IDM_CHECK_UPDATE && !st->update_checking) {
            st->update_checking = true;
            const std::string cur_ver = ReadPackageProvenance(st->base_root).version;
            auto live = st->live_hwnd;
            std::thread([cur_ver, live]() {
                auto* res = new UpdateCheckResult(CheckLatestReleaseWinHttp(cur_ver));
                const HWND target = live ? live->load() : nullptr;
                if (!target || !PostMessageW(target, WM_APP_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(res))) {
                    delete res;
                }
            }).detach();
        }
        return 0;
    }
    case WM_APP_UPDATE_RESULT: {
        std::unique_ptr<UpdateCheckResult> res(reinterpret_cast<UpdateCheckResult*>(lp));
        if (st) st->update_checking = false;
        if (!res) return 0;
        if (st && st->modal_depth > 0) {
            st->deferred_update_res = std::move(res);
            return 0;
        }
        if (!res->ok) {
            ShowObsidianDialog(hwnd, st, TrW(L"检查更新", L"Check for Updates"),
                TrW(L"查询失败", L"Check Failed"), 3, res->error, TrW(L"确定", L"OK"));
            return 0;
        }
        const std::wstring badge = res->update_available
            ? (g_zh_ui ? (L"发现新版本 · " + res->current_version + L" \u2192 " + res->latest_version)
                       : (L"Update Available · " + res->current_version + L" \u2192 " + res->latest_version))
            : (g_zh_ui ? (L"已是最新版本 · " + res->current_version)
                       : (L"Up to Date · " + res->current_version));
        std::wostringstream body;
        body << TrW(L"当前版本：", L"Current version: ") << res->current_version << L"\r\n"
             << TrW(L"最新发布：", L"Latest release: ") << res->latest_version << L"\r\n";
        if (!res->published_at.empty())
            body << TrW(L"发布时间：", L"Published: ") << res->published_at << L"\r\n";
        if (!res->asset_name.empty())
            body << TrW(L"推荐安装包：", L"Package: ") << res->asset_name << L"\r\n";
        if (!res->asset_digest.empty()) {
            const std::wstring short_sha = res->asset_digest.size() > 31
                ? (res->asset_digest.substr(0, 31) + L"...")
                : res->asset_digest;
            body << TrW(L"校验摘要：", L"Digest: ") << short_sha << L"\r\n";
        }
        body << L"\r\n";
        if (res->update_available) {
            body << TrW(L"下载后解压至新目录并运行新版安装器，即可一键并排升级（自动保留旧版以便随时回滚）。",
                        L"Download and extract into a new folder, then run Setup for side-by-side upgrade (with rollback).");
        } else {
            body << TrW(L"当前已是最新正式版本，无需更新。", L"You are already on the latest release.");
        }
        const bool open_url = ShowObsidianDialog(
            hwnd, st, TrW(L"检查更新", L"Check for Updates"),
            badge, res->update_available ? 1 : 2, body.str(),
            res->update_available ? TrW(L"打开下载页", L"Open Release") : TrW(L"查看发布页", L"Open Releases"),
            TrW(L"暂不打开", L"Close"));
        if (open_url && IsAllowedGithubReleaseUrl(res->release_url)) {
            ShellExecuteW(hwnd, L"open", res->release_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_DESTROY:
        if (st) {
            if (st->live_hwnd) st->live_hwnd->store(nullptr);
            if (st->font_title) DeleteObject(st->font_title);
            if (st->font_bold) DeleteObject(st->font_bold);
            if (st->font_regular) DeleteObject(st->font_regular);
            if (st->font_small) DeleteObject(st->font_small);
            if (st->font_mono) DeleteObject(st->font_mono);
            if (st->brush_input) DeleteObject(st->brush_input);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g_zh_ui = DetectChineseUi();
    const fs::path base_root = SelfBaseRoot();

    bool cli_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--cli") == 0) {
            cli_mode = true;
        } else if (wcscmp(argv[i], L"--lang") == 0 && i + 1 < argc) {
            g_zh_ui = DetectChineseUi(argv[++i]);
        }
    }
    if (cli_mode) {
        const HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!stdout_handle || stdout_handle == INVALID_HANDLE_VALUE)
            AttachConsole(ATTACH_PARENT_PROCESS);
        const int rc = RunCli(argc, argv, base_root);
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    GuiState st{};
    st.base_root = base_root;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WwmiDx12RtSetupWnd";
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{ 0, 0, MulDiv(720, static_cast<int>(dpi), 96), MulDiv(384, static_cast<int>(dpi), 96) };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        TrW(L"WWMI DX12+RT 安装配置器", L"WWMI DX12+RT Setup"),
        style, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInst, &st);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return (int)m.wParam;
}
