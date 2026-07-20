#include "bridge.h"
#include "http.h"
#include "util.h"

#include <windows.h>
#include <wincrypt.h>
#include <wrl.h>
#include <WebView2.h>

#include <mutex>
#include <thread>
#include <string>

using namespace Microsoft::WRL;
using Json = nlohmann::json;

static const char* MS_CLIENT_ID = "00000000402b5328";
static const char* MS_AUTH_URL   = "https://login.live.com/oauth20_authorize.srf";
static const char* MS_TOKEN_URL  = "https://login.live.com/oauth20_token.srf";
static const char* XBL_AUTH_URL  = "https://user.auth.xboxlive.com/user/authenticate";
static const char* XSTS_AUTH_URL = "https://xsts.auth.xboxlive.com/xsts/authorize";
static const char* MC_AUTH_URL   = "https://api.minecraftservices.com/authentication/login_with_xbox";
static const char* MC_PROFILE_URL= "https://api.minecraftservices.com/minecraft/profile";
static const char* REDIRECT_URI  = "https://login.live.com/oauth20_desktop.srf";

struct AuthError : std::runtime_error {
    long xerr = 0;
    explicit AuthError(const std::string& m, long x = 0) : std::runtime_error(m), xerr(x) {}
};

static uint64_t NowMs() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

static std::mutex g_acctMtx;

static std::string DpapiEncrypt(const std::string& plain) {
    DATA_BLOB in{ (DWORD)plain.size(), (BYTE*)plain.data() }, out{};
    if (!CryptProtectData(&in, L"justice-launcher", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string s((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}
static bool DpapiDecrypt(const std::string& enc, std::string& out) {
    DATA_BLOB in{ (DWORD)enc.size(), (BYTE*)enc.data() }, o{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &o)) return false;
    out.assign((char*)o.pbData, o.cbData);
    LocalFree(o.pbData);
    return true;
}

static Json EmptyStore() {
    return Json{ {"accounts", Json::array()}, {"activeUuid", nullptr} };
}

static Json LoadNoLock() {
    std::string raw = util::ReadFile(util::GameDir() + L"\\accounts.json");
    if (raw.empty()) return EmptyStore();
    size_t i = 0;
    while (i < raw.size() && ((unsigned char)raw[i] == 0xEF || (unsigned char)raw[i] == 0xBB ||
           (unsigned char)raw[i] == 0xBF || isspace((unsigned char)raw[i]))) i++;
    Json parsed; bool ok = false;
    if (i < raw.size() && (raw[i] == '{' || raw[i] == '[')) {
        try { parsed = Json::parse(raw); ok = true; } catch (...) {}
    }
    if (!ok) {
        std::string dec;
        if (DpapiDecrypt(raw, dec)) { try { parsed = Json::parse(dec); ok = true; } catch (...) {} }
    }
    if (ok && parsed.is_object() && parsed.contains("accounts") && parsed["accounts"].is_array())
        return parsed;
    return EmptyStore();
}
static void SaveNoLock(const Json& store) {
    util::EnsureDir(util::GameDir());
    std::string enc = DpapiEncrypt(store.dump());
    if (enc.empty()) enc = store.dump();
    util::WriteFile(util::GameDir() + L"\\accounts.json", enc);
}

static Json LoadAccounts() { std::lock_guard<std::mutex> lk(g_acctMtx); return LoadNoLock(); }

static Json GetActiveAccount() {
    std::lock_guard<std::mutex> lk(g_acctMtx);
    Json s = LoadNoLock();
    auto& accts = s["accounts"];
    if (accts.empty()) return Json(nullptr);
    if (s["activeUuid"].is_string()) {
        std::string a = s["activeUuid"];
        for (auto& x : accts) if (x.value("uuid", "") == a) return x;
    }
    return accts[0];
}
static void AddOrUpdate(const Json& authData) {
    std::lock_guard<std::mutex> lk(g_acctMtx);
    Json s = LoadNoLock();
    std::string uuid = authData.value("uuid", "");
    bool found = false;
    for (auto& x : s["accounts"]) if (x.value("uuid", "") == uuid) { x = authData; found = true; break; }
    if (!found) s["accounts"].push_back(authData);
    s["activeUuid"] = uuid;
    SaveNoLock(s);
}
static void RemoveAccount(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(g_acctMtx);
    Json s = LoadNoLock();
    Json keep = Json::array();
    for (auto& x : s["accounts"]) if (x.value("uuid", "") != uuid) keep.push_back(x);
    s["accounts"] = keep;
    if (s["activeUuid"].is_string() && s["activeUuid"] == uuid)
        s["activeUuid"] = keep.empty() ? Json(nullptr) : Json(keep[0].value("uuid", ""));
    SaveNoLock(s);
}
static bool SwitchAccount(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(g_acctMtx);
    Json s = LoadNoLock();
    for (auto& x : s["accounts"]) if (x.value("uuid", "") == uuid) {
        s["activeUuid"] = uuid; SaveNoLock(s); return true;
    }
    return false;
}

static Json PublicAccounts(const Json& store) {
    Json out = Json::array();
    for (auto& a : store["accounts"])
        out.push_back({ {"uuid", a.value("uuid", "")},
                        {"username", a.value("username", "")},
                        {"authType", a.value("authType", "msa")} });
    return out;
}

static std::string Str(const Json& j, const char* k) { return j.value(k, std::string()); }

static Json GetMSTokens(const std::string& code) {
    std::string form = "client_id=" + std::string(MS_CLIENT_ID) +
        "&code=" + util::UrlEncode(code) +
        "&grant_type=authorization_code&redirect_uri=" + util::UrlEncode(REDIRECT_URI);
    Json r = HttpJson("POST", MS_TOKEN_URL,
                      { {"Content-Type", "application/x-www-form-urlencoded"} }, form);
    if (!r.is_object() || !r.contains("access_token")) throw AuthError("MS token exchange failed");
    return r;
}
static Json RefreshMSTokens(const std::string& refresh) {
    std::string form = "client_id=" + std::string(MS_CLIENT_ID) +
        "&refresh_token=" + util::UrlEncode(refresh) +
        "&grant_type=refresh_token&redirect_uri=" + util::UrlEncode(REDIRECT_URI);
    Json r = HttpJson("POST", MS_TOKEN_URL,
                      { {"Content-Type", "application/x-www-form-urlencoded"} }, form);
    if (!r.is_object() || !r.contains("access_token")) throw AuthError("MS refresh failed");
    return r;
}
static void ParseXui(const Json& r, std::string& token, std::string& userHash) {
    token = Str(r, "Token");
    if (r.contains("DisplayClaims") && r["DisplayClaims"].contains("xui") &&
        r["DisplayClaims"]["xui"].is_array() && !r["DisplayClaims"]["xui"].empty())
        userHash = r["DisplayClaims"]["xui"][0].value("uhs", "");
}
static void GetXBLToken(const std::string& msAccess, std::string& token, std::string& uhs) {
    Json body = { {"Properties", {{"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"},
                                   {"RpsTicket", "d=" + msAccess}}},
                  {"RelyingParty", "http://auth.xboxlive.com"}, {"TokenType", "JWT"} };
    Json r = HttpJson("POST", XBL_AUTH_URL, {}, body.dump());
    ParseXui(r, token, uhs);
    if (token.empty()) throw AuthError("XBL auth failed");
}
static void GetXSTSToken(const std::string& xbl, std::string& token, std::string& uhs) {
    Json body = { {"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", Json::array({xbl})}}},
                  {"RelyingParty", "rp://api.minecraftservices.com/"}, {"TokenType", "JWT"} };
    Json r = HttpJson("POST", XSTS_AUTH_URL, {}, body.dump());
    ParseXui(r, token, uhs);
    if (token.empty()) {
        long xerr = r.is_object() ? (long)r.value("XErr", 0LL) : 0;
        if (xerr == 2148916233) throw AuthError("This Microsoft account has no Xbox account. Create one at xbox.com.", xerr);
        if (xerr == 2148916238) throw AuthError("This is a child account. Add it to a family at xbox.com.", xerr);
        throw AuthError("XSTS auth failed");
    }
}
static std::string GetMCToken(const std::string& xsts, const std::string& uhs) {
    Json body = { {"identityToken", "XBL3.0 x=" + uhs + ";" + xsts} };
    Json r = HttpJson("POST", MC_AUTH_URL, {}, body.dump());
    if (!r.is_object() || !r.contains("access_token")) throw AuthError("Minecraft auth failed");
    return Str(r, "access_token");
}
static Json GetMCProfile(const std::string& mcToken) {
    Json r = HttpJson("GET", MC_PROFILE_URL, { {"Authorization", "Bearer " + mcToken} });
    if (!r.is_object() || !r.contains("id"))
        throw AuthError("Could not fetch Minecraft profile. Make sure you own Minecraft Java Edition.");
    return r;
}

static Json DoFullAuth(const std::string& code) {
    Json ms = GetMSTokens(code);
    std::string xblTok, xblUhs; GetXBLToken(Str(ms, "access_token"), xblTok, xblUhs);
    std::string xsTok, xsUhs;  GetXSTSToken(xblTok, xsTok, xsUhs);
    std::string mcTok = GetMCToken(xsTok, xsUhs);
    Json profile = GetMCProfile(mcTok);
    Json authData = {
        {"mcAccessToken", mcTok},
        {"uuid", profile.value("id", "")},
        {"username", profile.value("name", "")},
        {"msRefreshToken", ms.value("refresh_token", "")},
        {"expiresAt", NowMs() + (uint64_t)ms.value("expires_in", 86400) * 1000ULL}
    };
    AddOrUpdate(authData);
    return authData;
}
static Json RefreshAuth(const Json& acc) {
    Json ms = RefreshMSTokens(acc.value("msRefreshToken", ""));
    std::string xblTok, xblUhs; GetXBLToken(Str(ms, "access_token"), xblTok, xblUhs);
    std::string xsTok, xsUhs;  GetXSTSToken(xblTok, xsTok, xsUhs);
    std::string mcTok = GetMCToken(xsTok, xsUhs);
    Json profile = GetMCProfile(mcTok);
    Json authData = {
        {"mcAccessToken", mcTok},
        {"uuid", profile.value("id", acc.value("uuid", ""))},
        {"username", profile.value("name", acc.value("username", ""))},
        {"msRefreshToken", ms.contains("refresh_token") ? ms.value("refresh_token", "")
                                                        : acc.value("msRefreshToken", "")},
        {"expiresAt", NowMs() + (uint64_t)ms.value("expires_in", 86400) * 1000ULL}
    };
    AddOrUpdate(authData);
    return authData;
}

static Json GetValidAuth() {
    Json acc = GetActiveAccount();
    if (acc.is_null()) return acc;
    uint64_t exp = acc.value("expiresAt", 0ULL);
    if (NowMs() > (exp > 600000ULL ? exp - 600000ULL : 0ULL)) {
        if (acc.value("authType", "") == "token") return acc;
        try { return RefreshAuth(acc); } catch (...) { return Json(nullptr); }
    }
    return acc;
}
static Json GetValidAuthForUUID(const std::string& uuid) {
    Json store = LoadAccounts();
    Json acc = Json(nullptr);
    for (auto& x : store["accounts"]) if (x.value("uuid", "") == uuid) { acc = x; break; }
    if (acc.is_null()) return acc;
    uint64_t exp = acc.value("expiresAt", 0ULL);
    if (NowMs() > (exp > 600000ULL ? exp - 600000ULL : 0ULL)) {
        if (acc.value("authType", "") == "token") return acc;
        try { return RefreshAuth(acc); } catch (...) { return Json(nullptr); }
    }
    return acc;
}

class AuthWindow {
public:
    using Done = std::function<void(bool ok, std::string code, std::string error)>;

    static void Open(Done done) {
        auto* self = new AuthWindow(std::move(done));
        if (!self->Create()) { self->Finish(false, "", "Could not open sign-in window"); }
    }

private:
    explicit AuthWindow(Done d) : done_(std::move(d)) {}

    static std::wstring UrlDecode(const std::wstring& s) {
        std::string in = util::WideToUtf8(s), out;
        for (size_t i = 0; i < in.size(); i++) {
            if (in[i] == '%' && i + 2 < in.size()) {
                out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
            } else if (in[i] == '+') out += ' ';
            else out += in[i];
        }
        return util::Utf8ToWide(out);
    }
    static std::string QueryValue(const std::wstring& uri, const std::wstring& key) {
        size_t q = uri.find(L'?');
        if (q == std::wstring::npos) return {};
        std::wstring qs = uri.substr(q + 1);
        size_t pos = 0;
        while (pos < qs.size()) {
            size_t amp = qs.find(L'&', pos);
            std::wstring pair = qs.substr(pos, amp == std::wstring::npos ? std::wstring::npos : amp - pos);
            size_t eq = pair.find(L'=');
            if (eq != std::wstring::npos && pair.substr(0, eq) == key)
                return util::WideToUtf8(UrlDecode(pair.substr(eq + 1)));
            if (amp == std::wstring::npos) break;
            pos = amp + 1;
        }
        return {};
    }

    bool Create() {
        static bool reg = [] {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = L"JusticeAuthWnd";
            RegisterClassExW(&wc);
            return true;
        }();
        (void)reg;

        int w = 520, h = 680;
        int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
        hwnd_ = CreateWindowExW(0, L"JusticeAuthWnd", L"Sign in with Microsoft",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                (sx - w) / 2, (sy - h) / 2, w, h,
                                nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOW);
        InitWebView();
        return true;
    }

    void InitWebView() {
        std::wstring udf;
        wchar_t* env = nullptr; size_t len = 0;
        if (_wdupenv_s(&env, &len, L"LOCALAPPDATA") == 0 && env) { udf = env; free(env); }
        udf += L"\\JusticeLauncher\\Auth";

        std::string authUrl = std::string(MS_AUTH_URL) + "?client_id=" + MS_CLIENT_ID +
            "&response_type=code&scope=" + util::UrlEncode("XboxLive.signin XboxLive.offline_access") +
            "&redirect_uri=" + util::UrlEncode(REDIRECT_URI) + "&prompt=select_account";
        navUrl_ = util::Utf8ToWide(authUrl);

        CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT r, ICoreWebView2Environment* e) -> HRESULT {
                    if (FAILED(r) || !e) { Finish(false, "", "WebView2 unavailable"); return r; }
                    e->CreateCoreWebView2Controller(hwnd_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT r, ICoreWebView2Controller* c) -> HRESULT {
                                if (FAILED(r) || !c) { Finish(false, "", "WebView2 controller failed"); return r; }
                                controller_ = c;
                                controller_->get_CoreWebView2(&webview_);
                                Resize();
                                EventRegistrationToken t;
                                webview_->add_NavigationStarting(
                                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                                            LPWSTR uri = nullptr;
                                            if (SUCCEEDED(a->get_Uri(&uri)) && uri) {
                                                std::wstring u(uri); CoTaskMemFree(uri);
                                                if (u.rfind(util::Utf8ToWide(REDIRECT_URI), 0) == 0) {
                                                    a->put_Cancel(TRUE);
                                                    std::string code = QueryValue(u, L"code");
                                                    std::string err = QueryValue(u, L"error");
                                                    if (!code.empty()) Finish(true, code, "");
                                                    else Finish(false, "", err.empty() ? "No auth code returned" : err);
                                                }
                                            }
                                            return S_OK;
                                        }).Get(), &t);
                                webview_->Navigate(navUrl_.c_str());
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
    }

    void Resize() {
        if (!controller_) return;
        RECT rc; GetClientRect(hwnd_, &rc);
        controller_->put_Bounds(rc);
    }

    void Finish(bool ok, std::string code, std::string error) {
        if (finished_) return;
        finished_ = true;
        if (done_) done_(ok, code, error);
        if (hwnd_) DestroyWindow(hwnd_);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        AuthWindow* self = nullptr;
        if (m == WM_NCCREATE) {
            self = (AuthWindow*)((CREATESTRUCT*)l)->lpCreateParams;
            SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)self);
            self->hwnd_ = h;
        } else {
            self = (AuthWindow*)GetWindowLongPtr(h, GWLP_USERDATA);
        }
        if (self) {
            switch (m) {
            case WM_SIZE: self->Resize(); break;
            case WM_CLOSE:
                self->Finish(false, "", "Cancelled");
                return 0;
            case WM_NCDESTROY:
                if (self->controller_) self->controller_->Close();
                delete self;
                return 0;
            }
        }
        return DefWindowProc(h, m, w, l);
    }

    Done done_;
    bool finished_ = false;
    HWND hwnd_ = nullptr;
    std::wstring navUrl_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
};

Json AuthGetValidAuth() { return GetValidAuth(); }

static void RunBg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

void RegisterAuthHandlers(Bridge& b) {
    Bridge* bp = &b;

    b.RegisterAsync("auth-status", [](const Json&, Bridge::ReplyFn reply) {
        RunBg([reply] {
            Json store = LoadAccounts();
            Json pub = PublicAccounts(store);
            Json auth = GetValidAuth();
            if (auth.is_null()) {
                Json active = GetActiveAccount();
                if (!active.is_null())
                    reply(Json{ {"loggedIn", false}, {"needsRefresh", true},
                                {"username", active.value("username", "")},
                                {"uuid", active.value("uuid", "")},
                                {"activeUuid", store["activeUuid"]}, {"accounts", pub} }, "");
                else
                    reply(Json{ {"loggedIn", false}, {"accounts", pub} }, "");
                return;
            }
            reply(Json{ {"loggedIn", true}, {"username", auth.value("username", "")},
                        {"uuid", auth.value("uuid", "")},
                        {"mcAccessToken", auth.value("mcAccessToken", "")},
                        {"activeUuid", store["activeUuid"]}, {"accounts", pub} }, "");
        });
    });

    b.Register("get-accounts", [](const Json&) -> Json {
        Json store = LoadAccounts();
        return Json{ {"accounts", PublicAccounts(store)}, {"activeUuid", store["activeUuid"]} };
    });

    b.RegisterAsync("switch-account", [bp](const Json& args, Bridge::ReplyFn reply) {
        std::string uuid = (args.is_array() && !args.empty() && args[0].is_string()) ? args[0].get<std::string>() : "";
        RunBg([bp, uuid, reply] {
            if (!SwitchAccount(uuid)) { reply(Json{ {"error", "Account not found"} }, ""); return; }
            Json auth = GetValidAuthForUUID(uuid);
            if (auth.is_null()) { reply(Json{ {"error", "Could not refresh token for this account"} }, ""); return; }
            Json store = LoadAccounts();
            bp->EmitAsync("auth-changed", Json{ {"loggedIn", true}, {"username", auth.value("username", "")},
                {"uuid", auth.value("uuid", "")}, {"mcAccessToken", auth.value("mcAccessToken", "")},
                {"accounts", PublicAccounts(store)}, {"activeUuid", uuid} });
            reply(Json{ {"success", true}, {"username", auth.value("username", "")}, {"uuid", auth.value("uuid", "")} }, "");
        });
    });

    b.RegisterAsync("remove-account", [bp](const Json& args, Bridge::ReplyFn reply) {
        std::string uuid = (args.is_array() && !args.empty() && args[0].is_string()) ? args[0].get<std::string>() : "";
        RunBg([bp, uuid, reply] {
            RemoveAccount(uuid);
            Json store = LoadAccounts();
            Json next = GetValidAuth();
            Json payload = next.is_null()
                ? Json{ {"loggedIn", false}, {"accounts", Json::array()} }
                : Json{ {"loggedIn", true}, {"username", next.value("username", "")},
                        {"uuid", next.value("uuid", "")}, {"activeUuid", store["activeUuid"]},
                        {"accounts", PublicAccounts(store)} };
            bp->EmitAsync("auth-changed", payload);
            reply(Json{ {"success", true} }, "");
        });
    });

    b.RegisterAsync("auth-login", [bp](const Json&, Bridge::ReplyFn reply) {
        AuthWindow::Open([bp, reply](bool ok, std::string code, std::string error) {
            if (!ok) { reply(Json{ {"error", error.empty() ? "Auth failed" : error} }, ""); return; }
            RunBg([bp, code, reply] {
                try {
                    Json authData = DoFullAuth(code);
                    bp->EmitAsync("auth-changed", Json{ {"loggedIn", true},
                        {"username", authData.value("username", "")}, {"uuid", authData.value("uuid", "")} });
                    reply(Json{ {"success", true}, {"username", authData.value("username", "")},
                                {"uuid", authData.value("uuid", "")} }, "");
                } catch (const std::exception& e) {
                    reply(Json{ {"error", e.what()} }, "");
                }
            });
        });
    });

    b.RegisterAsync("auth-logout", [bp](const Json& args, Bridge::ReplyFn reply) {
        std::string uuid = (args.is_array() && !args.empty() && args[0].is_string()) ? args[0].get<std::string>() : "";
        RunBg([bp, uuid, reply] {
            if (!uuid.empty()) RemoveAccount(uuid);
            else { Json a = GetActiveAccount(); if (!a.is_null()) RemoveAccount(a.value("uuid", "")); }
            Json store = LoadAccounts();
            Json next = GetValidAuth();
            Json payload = next.is_null()
                ? Json{ {"loggedIn", false}, {"accounts", Json::array()} }
                : Json{ {"loggedIn", true}, {"username", next.value("username", "")},
                        {"uuid", next.value("uuid", "")}, {"activeUuid", store["activeUuid"]},
                        {"accounts", PublicAccounts(store)} };
            bp->EmitAsync("auth-changed", payload);
            reply(Json{ {"success", true} }, "");
        });
    });

    b.RegisterAsync("auth-token-login", [bp](const Json& args, Bridge::ReplyFn reply) {
        Json a = (args.is_array() && !args.empty()) ? args[0] : Json::object();
        std::string token = a.value("token", "");
        std::string wantUuid = a.value("uuid", "");
        RunBg([bp, token, wantUuid, reply] {
            try {
                Json profile = GetMCProfile(token);
                std::string uuid = profile.value("id", "");
                std::string name = profile.value("name", "");
                auto strip = [](std::string s){ std::string o; for(char c: s) if(c!='-') o+=c; return o; };
                if (!wantUuid.empty() && strip(wantUuid) != strip(uuid))
                    throw AuthError("Supplied UUID does not match the token owner (" + name + ")");
                Json authData = { {"mcAccessToken", token}, {"uuid", uuid}, {"username", name},
                                  {"msRefreshToken", nullptr}, {"expiresAt", NowMs() + 86400ULL * 1000ULL},
                                  {"authType", "token"} };
                AddOrUpdate(authData);
                Json store = LoadAccounts();
                bp->EmitAsync("auth-changed", Json{ {"loggedIn", true}, {"username", name}, {"uuid", uuid},
                    {"activeUuid", store["activeUuid"]}, {"accounts", PublicAccounts(store)} });
                reply(Json{ {"success", true}, {"username", name}, {"uuid", uuid} }, "");
            } catch (const std::exception& e) {
                reply(Json{ {"error", e.what()} }, "");
            }
        });
    });

    b.RegisterAsync("get-skin-url", [](const Json&, Bridge::ReplyFn reply) {
        RunBg([reply] {
            Json auth = GetValidAuth();
            if (auth.is_null()) { reply(Json{ {"error", "Not logged in"} }, ""); return; }
            try {
                Json p = HttpJson("GET", MC_PROFILE_URL, { {"Authorization", "Bearer " + auth.value("mcAccessToken", "")} });
                Json skin = Json(nullptr), cape = Json(nullptr);
                if (p.contains("skins")) for (auto& s : p["skins"]) if (s.value("state", "") == "ACTIVE") { skin = s; break; }
                if (p.contains("capes")) for (auto& c : p["capes"]) if (c.value("state", "") == "ACTIVE") { cape = c; break; }
                reply(Json{ {"skinUrl", skin.is_null() ? Json(nullptr) : Json(skin.value("url", ""))},
                            {"variant", skin.is_null() ? "CLASSIC" : skin.value("variant", "CLASSIC")},
                            {"capeName", cape.is_null() ? Json(nullptr) : Json(cape.value("alias", ""))},
                            {"capeUrl", cape.is_null() ? Json(nullptr) : Json(cape.value("url", ""))},
                            {"username", p.value("name", "")}, {"uuid", p.value("id", "")} }, "");
            } catch (const std::exception& e) { reply(Json{ {"error", e.what()} }, ""); }
        });
    });

    b.Register("save-offline-skin", [](const Json& args) -> Json {
        if (!args.is_array() || args.empty() || !args[0].is_string()) return Json{ {"error", "bad args"} };
        std::string dataUrl = args[0];
        const std::string pfx = "base64,";
        size_t p = dataUrl.find(pfx);
        std::string b64 = p == std::string::npos ? dataUrl : dataUrl.substr(p + pfx.size());
        std::wstring path = util::GameDir() + L"\\offline-skin.png";
        util::EnsureDir(util::GameDir());
        util::WriteFile(path, util::Base64Decode(b64));
        return Json{ {"success", true} };
    });
    b.Register("clear-offline-skin", [](const Json&) -> Json {
        std::wstring path = util::GameDir() + L"\\offline-skin.png";
        DeleteFileW(path.c_str());
        return Json{ {"success", true} };
    });
    b.Register("get-offline-skin", [](const Json&) -> Json {
        std::wstring path = util::GameDir() + L"\\offline-skin.png";
        if (!util::Exists(path)) return Json{ {"exists", false} };
        std::string bytes = util::ReadFile(path);
        return Json{ {"exists", true}, {"dataUrl", "data:image/png;base64," + util::Base64Encode(bytes)} };
    });
}
