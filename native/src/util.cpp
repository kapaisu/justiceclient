#include "util.h"
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace util {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring HomeDir() {
    wchar_t* buf = nullptr; size_t len = 0;
    if (_wdupenv_s(&buf, &len, L"USERPROFILE") == 0 && buf) {
        std::wstring s(buf); free(buf); return s;
    }
    return L"";
}

std::wstring GameDir() { return HomeDir() + L"\\.justice-launcher"; }

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? p : p.substr(0, slash);
}

std::wstring InstanceDir(const std::string& versionId) {
    return GameDir() + L"\\instances\\" + Utf8ToWide(versionId);
}
std::wstring ModsDir(const std::string& versionId) {
    return InstanceDir(versionId) + L"\\mods";
}

std::string ReadFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

bool WriteFile(const std::wstring& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

bool Exists(const std::wstring& path) {
    std::error_code ec; return fs::exists(path, ec);
}
bool IsDir(const std::wstring& path) {
    std::error_code ec; return fs::is_directory(path, ec);
}
void EnsureDir(const std::wstring& path) {
    std::error_code ec; fs::create_directories(path, ec);
}

std::string Base64Encode(const std::string& bytes) {
    if (bytes.empty()) return {};
    DWORD n = 0;
    CryptBinaryToStringA((const BYTE*)bytes.data(), (DWORD)bytes.size(),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string out(n, '\0');
    CryptBinaryToStringA((const BYTE*)bytes.data(), (DWORD)bytes.size(),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &n);
    out.resize(n);
    return out;
}

std::string Base64Decode(const std::string& b64) {
    if (b64.empty()) return {};
    DWORD n = 0;
    CryptStringToBinaryA(b64.data(), (DWORD)b64.size(), CRYPT_STRING_BASE64,
                         nullptr, &n, nullptr, nullptr);
    std::string out(n, '\0');
    CryptStringToBinaryA(b64.data(), (DWORD)b64.size(), CRYPT_STRING_BASE64,
                         (BYTE*)out.data(), &n, nullptr, nullptr);
    out.resize(n);
    return out;
}

static uint64_t FiletimeToUnixMs(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;

    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

static bool GetTimes(const std::wstring& path, FILETIME& create, FILETIME& write) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    create = d.ftCreationTime; write = d.ftLastWriteTime;
    return true;
}

uint64_t LastWriteMs(const std::wstring& path) {
    FILETIME c, w;
    if (!GetTimes(path, c, w)) return 0;
    return FiletimeToUnixMs(w);
}

std::string CreationIso8601(const std::wstring& path) {
    FILETIME c, w;
    if (!GetTimes(path, c, w)) return "";
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&c, &st)) return "";
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

uint64_t FileSize(const std::wstring& path) {
    std::error_code ec;
    auto s = fs::file_size(path, ec);
    return ec ? 0 : (uint64_t)s;
}

uint64_t DirSize(const std::wstring& path) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(path, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (it->is_regular_file(fec)) total += (uint64_t)it->file_size(fec);
    }
    return total;
}

static std::string HashRaw(const wchar_t* algo, const std::string& data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr; BCRYPT_HASH_HANDLE hHash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&hAlg, algo, nullptr, 0) != 0) return out;
    DWORD hashLen = 0, cb = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0);
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0);
        out.resize(hashLen);
        BCryptFinishHash(hHash, (PUCHAR)out.data(), hashLen, 0);
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}
static std::string ToHex(const std::string& raw) {
    static const char* h = "0123456789abcdef";
    std::string s; s.reserve(raw.size() * 2);
    for (unsigned char c : raw) { s += h[c >> 4]; s += h[c & 0xF]; }
    return s;
}
std::string Sha1Hex(const std::string& bytes) { return ToHex(HashRaw(BCRYPT_SHA1_ALGORITHM, bytes)); }
std::string Sha1HexOfFile(const std::wstring& path) { return Sha1Hex(ReadFile(path)); }
std::string Md5Raw(const std::string& data) { return HashRaw(BCRYPT_MD5_ALGORITHM, data); }

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
    }
    return out;
}

}
