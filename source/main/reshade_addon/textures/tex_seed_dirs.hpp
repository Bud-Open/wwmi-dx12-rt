#pragma once
// ===========================================================================
//  tex_seed_dirs.hpp -- Non-destructive modular seed directory enumeration
//  and lightweight version-stamp ledger for TexLook (*.texlook) and TexLod
//  (*.lodcache).
//
//  Design goals:
//    * 100% generic, zero per-character / per-hash hardcoding.
//    * Deterministic lexicographical overlay order across multiple seed dirs:
//        1) WWMI_DX12_SEED_DIRS (semicolon-separated directories, optional)
//        2) <cache_dir>\seed
//        3) <g_data_dir>\seed (when g_data_dir is set and distinct)
//    * Never renames or mutates files inside modular seed directories.
//    * Tracks (normalized path -> size + mtime) in a small UTF-8 ledger under
//      cache_dir so unchanged seeds are skipped in O(1) while new or updated
//      seeds overlay seamlessly onto existing user caches.
// ===========================================================================
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <share.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wwmi_seed {

struct SeedFile {
    std::wstring path;       // full path for opening
    std::wstring name;       // basename for logging
    std::wstring norm_key;   // lowercased normalized path key in ledger
    std::string  stamp;      // "<size>:<mtime_hi>:<mtime_lo>"
    uint64_t     size = 0;
};

using Ledger = std::unordered_map<std::wstring, std::string>;

inline std::wstring TrimPath(std::wstring s)
{
    auto is_space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"'; };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

inline std::wstring LowerKey(const std::wstring& s)
{
    std::wstring out = TrimPath(s);
    for (wchar_t& c : out) c = static_cast<wchar_t>(towlower(c));
    return out;
}

inline std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline bool IsDirectory(const std::wstring& p)
{
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline std::vector<std::wstring> CollectSeedDirs(const std::wstring& cache_dir, const std::wstring& data_dir)
{
    std::vector<std::wstring>       dirs;
    std::unordered_set<std::wstring> seen;
    auto add_dir = [&](const std::wstring& raw) {
        const std::wstring clean = TrimPath(raw);
        if (clean.empty()) return;
        const std::wstring key = LowerKey(clean);
        if (!seen.insert(key).second) return;
        if (IsDirectory(clean)) dirs.push_back(clean);
    };

    wchar_t env_buf[32768]{};
    const DWORD env_n = GetEnvironmentVariableW(L"WWMI_DX12_SEED_DIRS", env_buf, static_cast<DWORD>(std::size(env_buf)));
    if (env_n > 0 && env_n < std::size(env_buf)) {
        const std::wstring env_str(env_buf, env_n);
        size_t             pos = 0;
        while (pos <= env_str.size()) {
            const size_t sep = env_str.find(L';', pos);
            const std::wstring part = sep == std::wstring::npos ? env_str.substr(pos) : env_str.substr(pos, sep - pos);
            add_dir(part);
            if (sep == std::wstring::npos) break;
            pos = sep + 1;
        }
    }

    if (!cache_dir.empty()) add_dir(cache_dir + L"\\seed");
    if (!data_dir.empty()) add_dir(data_dir + L"\\seed");
    return dirs;
}

inline std::vector<SeedFile> EnumerateModularSeeds(
    const std::vector<std::wstring>& dirs, const wchar_t* ext, const wchar_t* exclude_exact = nullptr)
{
    std::vector<SeedFile>            out;
    std::unordered_set<std::wstring> seen_paths;
    const size_t                     ext_len = wcslen(ext);

    for (const std::wstring& dir : dirs) {
        std::vector<SeedFile> local;
        WIN32_FIND_DATAW      fd{};
        const HANDLE          h = FindFirstFileW((dir + L"\\*" + ext).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const size_t n = wcslen(fd.cFileName);
            if (n <= ext_len || _wcsicmp(fd.cFileName + n - ext_len, ext) != 0) continue;
            if (_wcsnicmp(fd.cFileName, L"DISABLED_", 9) == 0) continue;
            if (exclude_exact && _wcsicmp(fd.cFileName, exclude_exact) == 0) continue;

            SeedFile sf;
            sf.name = fd.cFileName;
            sf.path = dir + L"\\" + sf.name;
            sf.norm_key = LowerKey(sf.path);
            if (!seen_paths.insert(sf.norm_key).second) continue;

            sf.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            char stamp_buf[96];
            snprintf(stamp_buf, sizeof(stamp_buf), "%llu:%08lx:%08lx",
                static_cast<unsigned long long>(sf.size),
                static_cast<unsigned long>(fd.ftLastWriteTime.dwHighDateTime),
                static_cast<unsigned long>(fd.ftLastWriteTime.dwLowDateTime));
            sf.stamp = stamp_buf;
            local.push_back(std::move(sf));
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        // Deterministic lexicographical ordering within each seed directory
        // (e.g., 000_base.texlook -> 010_patch.texlook -> 020_user.texlook).
        std::sort(local.begin(), local.end(), [](const SeedFile& a, const SeedFile& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        for (auto& sf : local) out.push_back(std::move(sf));
    }
    return out;
}

inline Ledger LoadLedger(const std::wstring& state_path)
{
    Ledger out;
    if (state_path.empty()) return out;
    FILE* f = _wfsopen(state_path.c_str(), L"r", _SH_DENYNO);
    if (!f) return out;
    char        buf[4096];
    std::string line;
    while (fgets(buf, sizeof(buf), f)) {
        line += buf;
        if (!line.empty() && line.back() != '\n' && !feof(f)) continue;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        const size_t tab = line.find('\t');
        if (tab != std::string::npos && tab > 0 && tab + 1 < line.size()) {
            const std::string  stamp = line.substr(0, tab);
            const std::wstring key = LowerKey(Utf8ToWide(line.substr(tab + 1)));
            if (!key.empty()) out[key] = stamp;
        }
        line.clear();
    }
    fclose(f);
    return out;
}

inline bool SaveLedger(const std::wstring& state_path, const Ledger& ledger)
{
    if (state_path.empty()) return false;
    const std::wstring tmp = state_path + L".tmp";
    FILE*              f = _wfsopen(tmp.c_str(), L"w", _SH_DENYNO);
    if (!f) return false;
    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(ledger.size());
    for (const auto& kv : ledger) rows.emplace_back(WideToUtf8(kv.first), kv.second);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    bool ok = true;
    for (const auto& r : rows) {
        if (fprintf(f, "%s\t%s\n", r.second.c_str(), r.first.c_str()) < 0) {
            ok = false;
            break;
        }
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), state_path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace wwmi_seed
