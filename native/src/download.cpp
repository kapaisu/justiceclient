#include "download.h"
#include "util.h"
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace fs = std::filesystem;

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static bool DownloadAttempt(const std::string& url, const std::wstring& dest,
                            std::function<void(unsigned long long, unsigned long long)> onBytes,
                            const std::string& expectedSha1, DWORD proxyMode, bool ignoreCerts) {
    std::error_code ec;
    fs::create_directories(fs::path(dest).parent_path(), ec);

    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[8192] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = _countof(path);
    uc.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET session = WinHttpOpen(L"JusticeLauncher/1.0", proxyMode,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    WinHttpSetTimeouts(session, 20000, 20000, 45000, 45000);
    { DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                     WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 ;
      WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos)); }

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) { WinHttpCloseHandle(session); return false; }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

    { DWORD rp = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
      WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &rp, sizeof(rp)); }
    if (ignoreCerts) {
        DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
    }

    bool ok = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, cl = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &cl, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            unsigned long long total = 0;
            wchar_t lenBuf[32] = {}; DWORD lb = sizeof(lenBuf);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                    lenBuf, &lb, WINHTTP_NO_HEADER_INDEX))
                total = _wcstoui64(lenBuf, nullptr, 10);

            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (out) {
                std::string all; all.reserve((size_t)total);
                std::vector<char> buf(65536);
                unsigned long long got = 0; DWORD avail = 0; ok = true;
                do {
                    if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
                    if (!avail) break;
                    DWORD toRead = avail < buf.size() ? avail : (DWORD)buf.size(), read = 0;
                    if (!WinHttpReadData(request, buf.data(), toRead, &read)) { ok = false; break; }
                    out.write(buf.data(), read);
                    if (!expectedSha1.empty()) all.append(buf.data(), read);
                    got += read;
                    if (onBytes) onBytes(got, total);
                } while (avail > 0);
                out.close();
                if (ok && total && got != total) ok = false;
                if (ok && !expectedSha1.empty()) {
                    std::string want = expectedSha1;
                    for (auto& c : want) c = (char)tolower((unsigned char)c);
                    if (util::Sha1Hex(all) != want) ok = false;
                }
            }
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok) { std::error_code e; fs::remove(dest, e); }
    return ok;
}

static bool DownloadImpl(const std::string& url, const std::wstring& dest,
                         std::function<void(unsigned long long, unsigned long long)> onBytes,
                         const std::string& expectedSha1) {
    struct Mode { DWORD proxy; bool ignoreCerts; };
    static const Mode modes[] = {
        { WINHTTP_ACCESS_TYPE_NO_PROXY,      false },
        { WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, false },
        { WINHTTP_ACCESS_TYPE_NO_PROXY,      true  },
        { WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, true  },
    };
    for (const auto& m : modes)
        if (DownloadAttempt(url, dest, onBytes, expectedSha1, m.proxy, m.ignoreCerts)) return true;
    return false;
}

bool DownloadFile(const std::string& url, const std::wstring& dest,
                  std::function<void(double)> onProgress, const std::string& expectedSha1) {
    return DownloadImpl(url, dest, [&](unsigned long long got, unsigned long long total) {
        if (total && onProgress) onProgress((double)got / (double)total);
    }, expectedSha1);
}

bool DownloadFileBytes(const std::string& url, const std::wstring& dest,
                       std::function<void(unsigned long long, unsigned long long)> onBytes) {
    return DownloadImpl(url, dest, std::move(onBytes), {});
}

int DownloadBatch(const std::vector<DlTask>& tasks, int concurrency,
                  std::function<void(int, int)> onProgress) {
    std::atomic<int> next{ 0 }, done{ 0 }, ok{ 0 };
    std::mutex progMtx;
    int total = (int)tasks.size();
    if (concurrency < 1) concurrency = 1;
    if (concurrency > total) concurrency = total ? total : 1;

    auto worker = [&]() {
        for (;;) {
            int i = next.fetch_add(1);
            if (i >= total) break;
            bool good = false;
            for (int attempt = 0; attempt < 3 && !good; attempt++) {
                if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
                good = DownloadFile(tasks[i].url, tasks[i].dest);
            }
            if (good) ok.fetch_add(1);
            int d = done.fetch_add(1) + 1;
            if (onProgress) { std::lock_guard<std::mutex> lk(progMtx); onProgress(d, total); }
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < concurrency; i++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    return ok.load();
}
