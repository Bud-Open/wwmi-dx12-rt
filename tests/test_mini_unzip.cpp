// Native unit test harness for tools/win_setup/mini_unzip.hpp.
// Invoked by tests/test_mini_unzip.py with test ZIP files.

#include "mini_unzip.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz < 0) return false;
    out.resize(static_cast<size_t>(sz));
    f.seekg(0, std::ios::beg);
    return sz == 0 || static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), sz));
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: test_mini_unzip <zip_path> <required_root|-> [max_unpacked_bytes]\n";
        return 2;
    }
    const std::string zip_path = argv[1];
    const std::string req_root = (std::string(argv[2]) == "-") ? "" : std::string(argv[2]);
    mini_unzip::Limits limits{};
    if (argc >= 4) {
        limits.max_unpacked_bytes = std::stoull(argv[3]);
    }

    std::vector<uint8_t> buf;
    if (!ReadAll(zip_path, buf)) {
        std::cerr << "ERR: could not read " << zip_path << "\n";
        return 2;
    }

    mini_unzip::EntryStats stats{};
    std::string err;
    const bool ok = mini_unzip::ExtractZipBuffer(
        buf.data(), buf.size(), req_root, limits,
        [](const std::string& rel, const std::vector<uint8_t>& content, std::string&) {
            const uint32_t crc = mini_unzip::Crc32(content.data(), content.size());
            std::printf("FILE %s %zu %08x\n", rel.c_str(), content.size(), crc);
            return true;
        },
        stats, err);

    if (!ok) {
        std::cerr << "REJECT: " << err << "\n";
        return 1;
    }
    std::printf("OK %u %llu\n", stats.file_count, static_cast<unsigned long long>(stats.unpacked_bytes));
    return 0;
}
