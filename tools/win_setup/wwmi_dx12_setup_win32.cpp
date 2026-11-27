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
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include "ledger_json.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
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

class MountLock {
    HANDLE handle_;
public:
    explicit MountLock(const fs::path& base)
    {
        const fs::path path = base / L"runtime\\.mount-lock";
        fs::create_directories(path.parent_path());
        RegularFileOrMissing(path);
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) WinError("无法打开安装锁", path);
        OVERLAPPED offset{};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset)) {
            const auto error = GetLastError();
            CloseHandle(handle_);
            WinError("无法锁定账本，可能有另一安装/卸载正在运行", path, error);
        }
    }
    ~MountLock() { CloseHandle(handle_); }
    MountLock(const MountLock&) = delete;
    MountLock& operator=(const MountLock&) = delete;
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

    // 2. Registry Uninstall entries (Kuro Official Launcher CN / Global / Bilibili)
    const std::pair<HKEY, const wchar_t*> uninstall_roots[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    for (const auto& [hive, path] : uninstall_roots) {
        HKEY hRoot = nullptr;
        if (RegOpenKeyExW(hive, path, 0, KEY_READ, &hRoot) != ERROR_SUCCESS) continue;
        wchar_t subname[512]{};
        for (DWORD idx = 0;; ++idx) {
            DWORD len = (DWORD)std::size(subname);
            if (RegEnumKeyExW(hRoot, idx, subname, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const std::wstring low = LowerWide(subname);
            if (low.find(L"wuthering") == std::wstring::npos && low.find(L"kuro") == std::wstring::npos &&
                low.find(L"3513350") == std::wstring::npos && low.find(L"\u9e23\u6f6e") == std::wstring::npos)
                continue;
            const std::wstring full_sub = std::wstring(path) + L"\\" + subname;
            const std::wstring loc = QueryRegString(hive, full_sub.c_str(), L"InstallLocation");
            if (!loc.empty()) cands.emplace_back(loc);
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

    // 4. Common drive paths
    for (wchar_t drv = L'C'; drv <= L'J'; ++drv) {
        const std::wstring root = std::wstring(1, drv) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        cands.emplace_back(root + L"Wuthering Waves");
        cands.emplace_back(root + L"Games\\Wuthering Waves");
        cands.emplace_back(root + L"Program Files\\Wuthering Waves");
        cands.emplace_back(root + L"Program Files (x86)\\Steam\\steamapps\\common\\Wuthering Waves");
        cands.emplace_back(root + L"SteamLibrary\\steamapps\\common\\Wuthering Waves");
    }

    for (const fs::path& c : cands) {
        const fs::path hit = ResolveWin64FromAnyGameDir(c);
        if (!hit.empty()) return hit;
    }
    return {};
}

fs::path AutoDiscoverWwmiDir(const fs::path& base_root)
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

struct LedgerInfo {
    enum class State { Missing, Installed, Installing, Uninstalled };
    State state = State::Missing;
    fs::path win64_dir;
    std::array<std::string, kBootstrap.size()> sha256;
    std::array<std::string, kBootstrap.size()> previous_sha256;
    std::string pending_id;

    bool active() const { return state == State::Installed || state == State::Installing; }
};

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
        else if (state == "uninstalled") info.state = LedgerInfo::State::Uninstalled;
        else throw std::runtime_error("invalid ledger state");
        info.win64_dir = LedgerAbsolutePath(json.at("win64_dir"));
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
                if (info.state == LedgerInfo::State::Installing) {
                    const auto found = item.fields.find("previous_sha256");
                    if (found != item.fields.end()) {
                        info.previous_sha256[i] = found->second.string();
                        if (!IsHex(info.previous_sha256[i], 64)) throw std::runtime_error("invalid previous digest");
                    }
                }
            }
            if (info.state == LedgerInfo::State::Installing) {
                info.pending_id = json.at("pending_id").string();
                if (!IsHex(info.pending_id, 32)) throw std::runtime_error("invalid transaction ID");
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

Hashes OwnedBootstrap(const fs::path& win64_dir, const LedgerInfo& ledger)
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
        if (owned[i] != ledger.sha256[i] && owned[i] != ledger.previous_sha256[i])
            throw std::runtime_error(g_zh_ui
                ? ("Win64/" + name + " 哈希与账本不一致，拒绝修改")
                : ("Win64/" + name + " hash does not match the install ledger; refusing to modify."));
    }
    return owned;
}

enum class MountState { NotInstalled, Installed, Drifted, Installing, ActiveElsewhere, Conflict };

MountState ClassifyMount(const fs::path& win64, const LedgerInfo& ledger, std::wstring& msg, Hashes& owned)
{
    if (ledger.active() && !SamePath(ledger.win64_dir, win64)) {
        msg = g_zh_ui
            ? (L"账本仍绑定 " + ledger.win64_dir.wstring() + L"；请先卸载旧游戏目录，再安装到新目录。")
            : (L"Ledger is still bound to " + ledger.win64_dir.wstring() + L". Uninstall the previous game directory first.");
        return MountState::ActiveElsewhere;
    }
    owned = OwnedBootstrap(win64, ledger);
    if (!ledger.active()) {
        msg = TrW(L"Win64 未挂载引导文件。", L"Not mounted in Win64.");
        return MountState::NotInstalled;
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

MountState InspectMountState(const fs::path& win64_dir, const fs::path& base_root, std::wstring& msg_out)
{
    try {
        Hashes owned;
        return ClassifyMount(CanonicalPath(win64_dir), ReadLedger(base_root), msg_out, owned);
    } catch (const std::exception& e) {
        msg_out = Utf8ToWide(e.what());
        return MountState::Conflict;
    }
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
    const std::string existing = ReadFileChecked(ini_path);
    auto trim_ascii = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        return s;
    };
    auto lower_ascii = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return s;
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
    int addon_path_idx = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = trim_ascii(lines[i]);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            in_addon = lower_ascii(trim_ascii(trimmed.substr(1, trimmed.size() - 2))) == "addon";
            if (in_addon && addon_header_idx < 0) addon_header_idx = (int)i;
            continue;
        }
        if (in_addon && !trimmed.empty() && trimmed.front() != ';' && trimmed.front() != '#') {
            const size_t eq = lines[i].find('=');
            if (eq != std::string::npos && lower_ascii(trim_ascii(lines[i].substr(0, eq))) == "addonpath") {
                addon_path_idx = (int)i;
                break;
            }
        }
    }
    if (addon_path_idx >= 0) {
        const size_t eq = lines[addon_path_idx].find('=');
        const std::string key = trim_ascii(lines[addon_path_idx].substr(0, eq));
        const std::string val = trim_ascii(lines[addon_path_idx].substr(eq + 1));
        bool has_default = false;
        size_t pos = 0;
        while (pos <= val.size()) {
            size_t sep = val.find_first_of(",;", pos);
            std::string tok = lower_ascii(trim_ascii(sep == std::string::npos ? val.substr(pos) : val.substr(pos, sep - pos)));
            for (char& c : tok) if (c == '\\') c = '/';
            while (!tok.empty() && tok.back() == '/') tok.pop_back();
            if (tok == "reshade-addons") { has_default = true; break; }
            if (sep == std::string::npos) break;
            pos = sep + 1;
        }
        if (!has_default) {
            lines[addon_path_idx] = key + "=" + (val.empty() ? "reshade-addons" : ("reshade-addons," + val));
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
             L"runtime\\uninstall-staging", L"host", L"reshade-addons\\wwmi_dx12\\dxc", L"reshade-addons\\rt-pack-data", L"seed"}) {
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
    if (ledger.state == LedgerInfo::State::Installing) {
        for (const auto* name : kBootstrap) {
            const auto temp = PendingPath(ledger.win64_dir, ledger.pending_id, name);
            if (RegularFileOrMissing(temp)) files.push_back(temp);
        }
    }
    return files;
}

bool PerformInstall(const fs::path& raw_game_dir, const fs::path& raw_wwmi_dir, const fs::path& raw_base, std::wstring& msg_out)
{
    try {
        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再安装。",
                "Wuthering Waves is currently running. Close the game and retry."));
        const auto provided_win64 = ResolveWin64FromAnyGameDir(raw_game_dir);
        if (provided_win64.empty())
            throw std::runtime_error(TrA("无效的游戏目录（未找到 Client-Win64-Shipping.exe）。",
                "Invalid game directory (Client-Win64-Shipping.exe not found)."));
        if (raw_wwmi_dir.empty() || !fs::is_regular_file(raw_wwmi_dir / L"d3dx.ini"))
            throw std::runtime_error(TrA("无效的 WWMI 目录（未找到 d3dx.ini）。请选择已有 WWMI 或点击【新建骨架】。",
                "Invalid WWMI directory (d3dx.ini not found). Select an existing WWMI folder or click [Init Skeleton]."));
        const auto win64 = CanonicalPath(provided_win64);
        const auto wwmi = CanonicalPath(raw_wwmi_dir);
        const auto base = CanonicalPath(raw_base);
        CheckBasePath(base, win64, wwmi);
        if (!SamePath(win64, provided_win64)) CheckBasePath(base, provided_win64, wwmi);
        Hashes owned;
        auto check_mount = [&]() {
            auto ledger = ReadLedger(base);
            std::wstring msg;
            if (ClassifyMount(win64, ledger, msg, owned) == MountState::ActiveElsewhere)
                throw std::runtime_error(WideToUtf8(msg));
            return ledger;
        };
        check_mount();
        const auto copies = PlanPayloadCopies(base);

        const MountLock lock(base);
        if (IsGameRunning())
            throw std::runtime_error("检测到《鸣潮》正在运行，请先退出游戏再安装。");
        const auto old_ledger = check_mount();
        const auto abandoned = PendingFiles(old_ledger);
        if (!abandoned.empty()) StageFilesToBackup(abandoned, base, "interrupted-install");
        const std::array<std::string, kBootstrap.size()> content = {
            ReadFileChecked(copies.empty() ? base / L"host\\ReShade64-game-forward.dll" : copies.front().first),
            "[INSTALL]\r\nBasePath=" + WideToUtf8(base.wstring()) + "\r\n",
        };
        Hashes expected;
        for (size_t i = 0; i < kBootstrap.size(); ++i) expected[i] = Sha256Hex(content[i]);
        const auto token = NewToken();
        auto write_ledger = [&](bool installing) {
            std::ostringstream out;
            out << "{\n  \"schema_version\": 1,\n  \"state\": \"" << (installing ? "installing" : "installed") << "\",\n"
                << "  \"platform_kind\": \"windows\",\n"
                << "  \"base_root\": \"" << JsonEscape(base.wstring()) << "\",\n"
                << "  \"win64_dir\": \"" << JsonEscape(win64.wstring()) << "\",\n"
                << "  \"wwmi_dir\": \"" << JsonEscape(wwmi.wstring()) << "\",\n";
            if (installing) out << "  \"pending_id\": \"" << token << "\",\n";
            out << "  \"win64_files\": [\n";
            for (size_t i = 0; i < kBootstrap.size(); ++i) {
                if (i) out << ",\n";
                out << "    { \"relative\": \"" << WideToUtf8(kBootstrap[i]) << "\", \"path\": \""
                    << JsonEscape((win64 / kBootstrap[i]).wstring()) << "\", \"sha256\": \"" << expected[i]
                    << "\", \"size\": " << content[i].size();
                if (installing && !owned[i].empty()) out << ", \"previous_sha256\": \"" << owned[i] << "\"";
                out << " }";
            }
            out << "\n  ],\n  \"extra_managed_files\": []\n}\n";
            AtomicWrite(JournalPath(base), out.str());
        };

        write_ledger(true);
        for (const auto& [source, target] : copies) {
            fs::create_directories(target.parent_path());
            // CopyFileW would follow a reparse point at the destination.
            RegularFileOrMissing(target);
            if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) WinError("同步运行时组件失败", target);
        }
        const auto runtime = base / L"runtime";
        for (const auto* dir : { L"logs", L"cache\\shaders", L"seed" }) fs::create_directories(runtime / dir);
        fs::create_directories(base / L"seed");
        AtomicWrite(base / L"ReShade.ini", MergeExternalReShadeIni(base / L"ReShade.ini"));
        const std::wstring wini = L"\ufeff; Managed by WWMI-DX12-RT Setup\r\n[wwmi-dx12]\r\nwwmi_dir=" +
            wwmi.wstring() + L"\r\nskin_mode=base\r\nruntime_dir=" + runtime.wstring() +
            L"\r\nmax_logs=10\r\ntex_struct_fallback=1\r\ndebug=0\r\nlog_level=1\r\n";
        AtomicWrite(base / L"wwmi_dx12.ini",
            std::string(reinterpret_cast<const char*>(wini.data()), wini.size() * sizeof(wchar_t)));

        if (IsGameRunning())
            throw std::runtime_error(TrA("检测到《鸣潮》正在运行，请先退出游戏再安装。",
                "Wuthering Waves is currently running. Close the game and retry."));
        const auto current = OwnedBootstrap(win64, ReadLedger(base));
        std::vector<fs::path> old_files;
        for (size_t i = 0; i < kBootstrap.size(); ++i)
            if (!current[i].empty()) old_files.push_back(win64 / kBootstrap[i]);
        if (!old_files.empty()) StageFilesToBackup(old_files, base, "reinstall");
        for (size_t i = 0; i < kBootstrap.size(); ++i) {
            const auto temp = PendingPath(win64, token, kBootstrap[i]);
            const auto dest = win64 / kBootstrap[i];
            WriteNewFile(temp, content[i]);
            // No REPLACE_EXISTING: a new foreign file may have appeared since preflight.
            if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH)) WinError("无法提交引导文件", dest);
        }
        write_ledger(false);
        msg_out = TrW(
            L"安装启用成功！请使用 DirectX 12 模式从库洛官方启动器或 Steam 开始游戏。",
            L"Installed and enabled! Launch Wuthering Waves in DirectX 12 mode from the Kuro Launcher or Steam.");
        std::error_code core_ec;
        if (!fs::is_regular_file(wwmi / L"Core\\WWMI\\WuWa-Model-Importer.ini", core_ec)) {
            msg_out += TrW(
                L"\r\n\r\n[提示] 尚未检测到官方 Core\\WWMI 核心文件，请将官方 WWMI-Package 的 Core 目录放入该 WWMI 文件夹后再使用角色皮肤（需要 RabbitFX 的皮肤请在 Mods 下仅保留一份）。",
                L"\r\n\r\n[Note] Official Core\\WWMI files were not detected yet. Copy WWMI-Package's Core\\ folder into this WWMI directory before using character mods (keep at most one copy of RabbitFX under Mods\\).");
        }
        const auto extra_addons = ScanThirdPartyWin64Addons(win64);
        if (!extra_addons.empty()) {
            std::wstring list_str;
            for (size_t i = 0; i < extra_addons.size(); ++i) {
                if (i) list_str += L", ";
                list_str += extra_addons[i];
            }
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
        if (!ReadLedger(base).active()) return noop();
        const MountLock lock(base);
        const auto ledger = ReadLedger(base);
        if (!ledger.active()) return noop();
        if (!SamePath(ledger.win64_dir, CanonicalPath(ledger.win64_dir)))
            throw std::runtime_error(TrA("原 Win64 目录现在指向其他位置，拒绝卸载。",
                "Recorded Win64 directory now resolves elsewhere; refusing to uninstall."));
        const auto owned = OwnedBootstrap(ledger.win64_dir, ledger);
        std::vector<fs::path> files;
        for (size_t i = 0; i < kBootstrap.size(); ++i)
            if (!owned[i].empty()) files.push_back(ledger.win64_dir / kBootstrap[i]);
        const auto pending = PendingFiles(ledger);
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

std::wstring BuildDoctorText(const fs::path& raw_game, const fs::path& raw_wwmi, const fs::path& base_root)
{
    std::wostringstream ss;
    ss << TrW(L"外部挂载程序根：", L"External runtime root: ") << NormWinPath(base_root) << L"\r\n";

    const fs::path win64 = ResolveWin64FromAnyGameDir(raw_game);
    if (win64.empty()) {
        ss << TrW(L"[错误] 游戏目录：未找到 Client-Win64-Shipping.exe\r\n",
                  L"[ERR]  Game directory: Client-Win64-Shipping.exe not found\r\n");
    } else {
        ss << TrW(L"[正常] 游戏 Win64：", L"[OK]   Game Win64: ") << win64.wstring() << L"\r\n";
        std::wstring mmsg;
        InspectMountState(win64, base_root, mmsg);
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

// ------------------------- Headless CLI Mode -------------------------
void CliPrint(const std::wstring& w)
{
    const std::string s = WideToUtf8(w) + "\n";
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hOut, s.data(), (DWORD)s.size(), &written, nullptr);
    }
}

int RunCli(int argc, wchar_t** argv, const fs::path& base_root)
{
    std::wstring action = L"doctor";
    fs::path game_arg, wwmi_arg;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--cli" && i + 1 < argc) action = argv[++i];
        else if (a == L"--game" && i + 1 < argc) game_arg = argv[++i];
        else if (a == L"--wwmi" && i + 1 < argc) wwmi_arg = argv[++i];
        else if (a == L"--lang" && i + 1 < argc) g_zh_ui = DetectChineseUi(argv[++i]);
    }
    if (action != L"uninstall" && action != L"init-wwmi" && game_arg.empty())
        game_arg = AutoDiscoverGameWin64(base_root);
    if (action != L"uninstall" && wwmi_arg.empty())
        wwmi_arg = AutoDiscoverWwmiDir(base_root);

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
        const bool ok = PerformInstall(game_arg, wwmi_arg, base_root, msg);
        CliPrint((ok ? L"CLI_OK: " : L"CLI_ERR: ") + msg);
        return ok ? 0 : 1;
    }
    if (action == L"uninstall") {
        std::wstring msg;
        const bool ok = PerformUninstall(base_root, msg);
        CliPrint((ok ? L"CLI_OK: " : L"CLI_ERR: ") + msg);
        return ok ? 0 : 1;
    }
    CliPrint(BuildDoctorText(game_arg, wwmi_arg, base_root));
    return 0;
}

// ------------------------- Native Win32 GUI -------------------------
enum CtrlId : int {
    IDC_EDIT_GAME = 101,
    IDC_BTN_BROWSE_GAME = 102,
    IDC_EDIT_WWMI = 103,
    IDC_BTN_BROWSE_WWMI = 104,
    IDC_BTN_INIT_WWMI = 105,
    IDC_EDIT_REPORT = 106,
    IDC_BTN_INSTALL = 107,
    IDC_BTN_UNINSTALL = 108,
    IDC_BTN_DOCTOR = 109,
    IDC_BTN_OPEN_MODS = 110,
    IDC_BTN_OPEN_LOGS = 111,
};

struct GuiState {
    fs::path base_root;
    HFONT    font = nullptr;
    HWND     edit_game = nullptr;
    HWND     edit_wwmi = nullptr;
    HWND     edit_report = nullptr;
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

std::wstring BrowseFolderDialog(HWND owner, const wchar_t* title)
{
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

void RefreshGuiReport(GuiState* st)
{
    const std::wstring rep = BuildDoctorText(GetWndString(st->edit_game), GetWndString(st->edit_wwmi), st->base_root);
    SetWindowTextW(st->edit_report, rep.c_str());
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<GuiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<GuiState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        const UINT dpi = GetDpiForWindow(hwnd);
        const auto sc = [&](int v) { return MulDiv(v, (int)dpi, 96); };
        st->font = CreateFontW(-sc(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            TrW(L"Microsoft YaHei UI", L"Segoe UI"));

        auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) {
            HWND c = CreateWindowExW((wcscmp(cls, L"EDIT") == 0) ? WS_EX_CLIENTEDGE : 0, cls, txt,
                WS_CHILD | WS_VISIBLE | style, sc(x), sc(y), sc(w), sc(h), hwnd,
                id ? reinterpret_cast<HMENU>((INT_PTR)id) : nullptr, cs->hInstance, nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
            return c;
        };

        mk(L"STATIC", TrW(L"鸣潮游戏目录（支持库洛官方启动器根目录 / Steam 目录 / Win64）：",
            L"Wuthering Waves folder (Kuro Launcher root / Steam folder / Win64):"), 0, 16, 14, 620, 22, 0);
        st->edit_game = mk(L"EDIT", L"", ES_AUTOHSCROLL, 16, 38, 536, 28, IDC_EDIT_GAME);
        mk(L"BUTTON", TrW(L"浏览...", L"Browse..."), BS_PUSHBUTTON, 560, 38, 96, 28, IDC_BTN_BROWSE_GAME);

        mk(L"STATIC", TrW(L"WWMI Mod 目录（可直接复用已有 XXMI Launcher\\WWMI，或点右侧新建）：",
            L"WWMI Mods folder (reuse existing XXMI Launcher\\WWMI, or click Init Skeleton):"), 0, 16, 76, 620, 22, 0);
        st->edit_wwmi = mk(L"EDIT", L"", ES_AUTOHSCROLL, 16, 100, 434, 28, IDC_EDIT_WWMI);
        mk(L"BUTTON", TrW(L"浏览...", L"Browse..."), BS_PUSHBUTTON, 458, 100, 94, 28, IDC_BTN_BROWSE_WWMI);
        mk(L"BUTTON", TrW(L"新建骨架", L"Init Skeleton"), BS_PUSHBUTTON, 560, 100, 96, 28, IDC_BTN_INIT_WWMI);

        mk(L"STATIC", TrW(L"当前环境与健康自检状态（运行期对游戏目录 0 字节额外写入）：",
            L"Environment & health check (zero runtime writes inside the game folder):"), 0, 16, 140, 620, 22, 0);
        st->edit_report = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 16, 164, 640, 165, IDC_EDIT_REPORT);

        mk(L"BUTTON", TrW(L"一键安装启用", L"Install & Enable"), BS_DEFPUSHBUTTON, 16, 344, 140, 36, IDC_BTN_INSTALL);
        mk(L"BUTTON", TrW(L"一键卸载还原", L"Uninstall / Restore"), BS_PUSHBUTTON, 166, 344, 130, 36, IDC_BTN_UNINSTALL);
        mk(L"BUTTON", TrW(L"刷新自检", L"Refresh Check"), BS_PUSHBUTTON, 306, 344, 105, 36, IDC_BTN_DOCTOR);
        mk(L"BUTTON", TrW(L"打开 Mods 目录", L"Open Mods"), BS_PUSHBUTTON, 421, 344, 118, 36, IDC_BTN_OPEN_MODS);
        mk(L"BUTTON", TrW(L"打开日志目录", L"Open Logs"), BS_PUSHBUTTON, 549, 344, 107, 36, IDC_BTN_OPEN_LOGS);

        const fs::path g = AutoDiscoverGameWin64(st->base_root);
        const fs::path w = AutoDiscoverWwmiDir(st->base_root);
        if (!g.empty()) SetWindowTextW(st->edit_game, g.c_str());
        if (!w.empty()) SetWindowTextW(st->edit_wwmi, w.c_str());
        RefreshGuiReport(st);
        return 0;
    }
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wp);
        if (id == IDC_BTN_BROWSE_GAME) {
            const std::wstring p = BrowseFolderDialog(hwnd, TrW(
                L"请选择《鸣潮》安装目录（包含 Wuthering Waves Game 或 Client-Win64-Shipping.exe）",
                L"Select Wuthering Waves install folder (containing Wuthering Waves Game or Client-Win64-Shipping.exe)"));
            if (!p.empty()) {
                const fs::path resolved = ResolveWin64FromAnyGameDir(p);
                SetWindowTextW(st->edit_game, (resolved.empty() ? p : resolved.wstring()).c_str());
                RefreshGuiReport(st);
            }
        } else if (id == IDC_BTN_BROWSE_WWMI) {
            const std::wstring p = BrowseFolderDialog(hwnd, TrW(
                L"请选择包含 d3dx.ini 与 Mods 的 WWMI 目录（如 XXMI Launcher\\WWMI）",
                L"Select WWMI folder containing d3dx.ini and Mods\\ (e.g. XXMI Launcher\\WWMI)"));
            if (!p.empty()) {
                SetWindowTextW(st->edit_wwmi, p.c_str());
                RefreshGuiReport(st);
            }
        } else if (id == IDC_BTN_INIT_WWMI) {
            fs::path target = GetWndString(st->edit_wwmi);
            if (target.empty()) target = st->base_root / L"WWMI";
            std::wstring err;
            if (InitWwmiSkeleton(target, err)) {
                SetWindowTextW(st->edit_wwmi, NormWinPath(target).c_str());
                RefreshGuiReport(st);
                const std::wstring ok_msg = g_zh_ui
                    ? (L"已创建标准 WWMI 目录骨架：\n" + NormWinPath(target) + L"\n\n请确保放入官方 Core\\WWMI 与 Mods\\RabbitFX。")
                    : (L"Initialized standard WWMI skeleton:\n" + NormWinPath(target) + L"\n\nPlace official Core\\WWMI and Mods\\RabbitFX inside before launching.");
                MessageBoxW(hwnd, ok_msg.c_str(), L"WWMI DX12+RT", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, err.c_str(), TrW(L"错误", L"Error"), MB_OK | MB_ICONERROR);
            }
        } else if (id == IDC_BTN_INSTALL) {
            std::wstring res_msg;
            const bool ok = PerformInstall(GetWndString(st->edit_game), GetWndString(st->edit_wwmi), st->base_root, res_msg);
            RefreshGuiReport(st);
            MessageBoxW(hwnd, res_msg.c_str(), L"WWMI DX12+RT", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        } else if (id == IDC_BTN_UNINSTALL) {
            std::wstring res_msg;
            const bool ok = PerformUninstall(st->base_root, res_msg);
            RefreshGuiReport(st);
            MessageBoxW(hwnd, res_msg.c_str(), L"WWMI DX12+RT", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        } else if (id == IDC_BTN_DOCTOR) {
            RefreshGuiReport(st);
        } else if (id == IDC_BTN_OPEN_MODS) {
            const fs::path wwmi = GetWndString(st->edit_wwmi);
            const fs::path mods = wwmi / L"Mods";
            std::error_code ec;
            if (fs::is_directory(mods, ec)) ShellExecuteW(hwnd, L"open", mods.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else MessageBoxW(hwnd, TrW(L"Mods 目录不存在，请先设置有效的 WWMI 目录。",
                L"Mods folder does not exist yet. Select a valid WWMI folder first."),
                TrW(L"提示", L"Notice"), MB_OK | MB_ICONWARNING);
        } else if (id == IDC_BTN_OPEN_LOGS) {
            const fs::path logs = st->base_root / L"runtime\\logs";
            std::error_code ec;
            fs::create_directories(logs, ec);
            ShellExecuteW(hwnd, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_DESTROY:
        if (st && st->font) DeleteObject(st->font);
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
    wc.lpszClassName = L"WwmiDx12RtSetupWnd";
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const int w = MulDiv(688, (int)dpi, 96);
    const int h = MulDiv(432, (int)dpi, 96);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        TrW(L"WWMI DX12+RT 安装配置器 (Windows 官方启动器 / Steam 通用版)",
            L"WWMI DX12+RT Setup (Windows Kuro Launcher & Steam)"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
        hInst, &st);

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
