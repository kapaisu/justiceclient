#include "zip.h"
#include "util.h"
#include "miniz.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

bool ZipIsValid(const std::wstring& zipPath) {
    std::error_code ec;
    uint64_t sz = fs::file_size(zipPath, ec);
    if (ec || sz < 22) return false;
    size_t want = (size_t)(std::min<uint64_t>)(sz, 65557);
    std::ifstream f(zipPath, std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)(sz - want), std::ios::beg);
    std::string buf(want, '\0');
    f.read(&buf[0], (std::streamsize)want);
    size_t n = (size_t)f.gcount();
    const unsigned char sig[4] = { 0x50, 0x4B, 0x05, 0x06 };
    for (size_t i = 0; i + 4 <= n; i++)
        if (memcmp(buf.data() + i, sig, 4) == 0) return true;
    return false;
}

static bool EndsWithCI(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); i++)
        if (tolower((unsigned char)s[s.size() - suf.size() + i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

bool UnzipAll(const std::wstring& zipPath, const std::wstring& destDir) {
    std::string data = util::ReadFile(zipPath);
    if (data.empty()) return false;

    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return false;

    bool ok = true;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::wstring rel = util::Utf8ToWide(st.m_filename);
        for (auto& c : rel) if (c == L'/') c = L'\\';
        std::wstring outPath = destDir + L"\\" + rel;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) { util::EnsureDir(outPath); continue; }
        util::EnsureDir(fs::path(outPath).parent_path().wstring());
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (p) { util::WriteFile(outPath, std::string((char*)p, sz)); mz_free(p); }
        else ok = false;
    }
    mz_zip_reader_end(&zip);
    return ok;
}

std::string ZipReadEntry(const std::wstring& zipPath, const std::string& entryName) {
    std::string data = util::ReadFile(zipPath);
    if (data.empty()) return {};
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return {};
    std::string out;
    int idx = mz_zip_reader_locate_file(&zip, entryName.c_str(), nullptr, 0);
    if (idx >= 0) {
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
        if (p) { out.assign((char*)p, sz); mz_free(p); }
    }
    mz_zip_reader_end(&zip);
    return out;
}

int ZipExtractByPrefix(const std::wstring& zipPath, const std::string& prefix, const std::wstring& destDir) {
    std::string data = util::ReadFile(zipPath);
    if (data.empty()) return 0;
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return 0;
    int written = 0;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        std::string name = st.m_filename;
        if (name.rfind(prefix, 0) != 0) continue;
        std::string rel = name.substr(prefix.size());
        std::wstring wrel = util::Utf8ToWide(rel);
        for (auto& c : wrel) if (c == L'/') c = L'\\';
        std::wstring outPath = destDir + L"\\" + wrel;
        util::EnsureDir(fs::path(outPath).parent_path().wstring());
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (p) { util::WriteFile(outPath, std::string((char*)p, sz)); mz_free(p); written++; }
    }
    mz_zip_reader_end(&zip);
    return written;
}

bool ZipFolder(const std::wstring& srcDir, const std::wstring& zipPath, const std::string& innerPrefix) {
    std::error_code ec;
    if (!fs::exists(srcDir, ec)) return false;

    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return false;
    bool ok = true;
    for (auto it = fs::recursive_directory_iterator(srcDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        std::wstring rel = fs::relative(it->path(), srcDir, fec).wstring();
        std::string relU = util::WideToUtf8(rel);
        for (auto& c : relU) if (c == '\\') c = '/';
        std::string entry = innerPrefix.empty() ? relU : (innerPrefix + "/" + relU);
        std::string data = util::ReadFile(it->path().wstring());
        if (!mz_zip_writer_add_mem(&zip, entry.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION)) { ok = false; break; }
    }
    void* buf = nullptr; size_t size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    if (ok && buf) ok = util::WriteFile(zipPath, std::string((char*)buf, size));
    if (buf) mz_free(buf);
    mz_zip_writer_end(&zip);
    return ok;
}

int ExtractNativeLibs(const std::wstring& jarPath, const std::wstring& destDir) {
    std::string data = util::ReadFile(jarPath);
    if (data.empty()) return 0;

    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return 0;

    util::EnsureDir(destDir);
    int written = 0;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        std::string name = st.m_filename;
        if (name.rfind("META-INF/", 0) == 0 || name.find("module-info") != std::string::npos) continue;
        if (!(EndsWithCI(name, ".dll") || EndsWithCI(name, ".so") ||
              EndsWithCI(name, ".dylib") || EndsWithCI(name, ".jnilib"))) continue;
        size_t slash = name.find_last_of('/');
        std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        std::wstring outPath = destDir + L"\\" + util::Utf8ToWide(base);
        if (util::Exists(outPath) && util::FileSize(outPath) > 0) continue;
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (p) { util::WriteFile(outPath, std::string((char*)p, sz)); mz_free(p); written++; }
    }
    mz_zip_reader_end(&zip);
    return written;
}
