#include "http.h"
#include <windows.h>
#include <winhttp.h>

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static HttpResponse HttpSendAttempt(const std::string& method, const std::string& url,
                      const HttpHeaders& headers, const std::string& body,
                      DWORD proxyMode, bool ignoreCerts) {
    HttpResponse out;

    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = _countof(path);
    uc.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return out;

    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET session = WinHttpOpen(L"JusticeLauncher/1.0",
                                    proxyMode,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    { DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                     WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 ;
      WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos)); }

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) { WinHttpCloseHandle(session); return out; }

    HINTERNET request = WinHttpOpenRequest(
        connect, Widen(method).c_str(), path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return out; }

    { DWORD rp = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
      WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &rp, sizeof(rp)); }
    if (ignoreCerts) {
        DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
    }

    std::wstring headerBlock;
    for (const auto& h : headers) {
        headerBlock += Widen(h.first) + L": " + Widen(h.second) + L"\r\n";
    }
    if (!headerBlock.empty()) {
        WinHttpAddRequestHeaders(request, headerBlock.c_str(), (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    BOOL sent = WinHttpSendRequest(request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(), (DWORD)body.size(), 0);

    if (sent && WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, codeLen = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeLen, WINHTTP_NO_HEADER_INDEX);
        out.status = (long)code;
        out.ok = true;

        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) break;
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
            chunk.resize(read);
            out.body += chunk;
        } while (avail > 0);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
}

HttpResponse HttpSend(const std::string& method, const std::string& url,
                      const HttpHeaders& headers, const std::string& body) {
    struct Mode { DWORD proxy; bool ignoreCerts; };
    static const Mode modes[] = {
        { WINHTTP_ACCESS_TYPE_NO_PROXY,      false },
        { WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, false },
        { WINHTTP_ACCESS_TYPE_NO_PROXY,      true  },
        { WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, true  },
    };
    HttpResponse out;
    for (const auto& m : modes) {
        out = HttpSendAttempt(method, url, headers, body, m.proxy, m.ignoreCerts);
        if (out.ok) return out;
    }
    return out;
}

nlohmann::json HttpJson(const std::string& method, const std::string& url,
                        const HttpHeaders& headers, const std::string& body) {
    HttpHeaders h = headers;
    bool hasUA = false, hasAccept = false, hasCT = false;
    for (auto& kv : h) {
        std::string k = kv.first;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k == "user-agent") hasUA = true;
        else if (k == "accept") hasAccept = true;
        else if (k == "content-type") hasCT = true;
    }
    if (!hasUA)     h.emplace_back("User-Agent", "JusticeLauncher/1.0");
    if (!hasAccept) h.emplace_back("Accept", "application/json");
    if (!body.empty() && !hasCT) h.emplace_back("Content-Type", "application/json");

    HttpResponse r = HttpSend(method, url, h, body);
    if (!r.ok) return nlohmann::json{ {"__httpError", "request failed"} };
    try {
        return nlohmann::json::parse(r.body);
    } catch (...) {
        return nlohmann::json(r.body);
    }
}
