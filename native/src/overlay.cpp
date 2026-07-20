#include "bridge.h"
#include "http.h"
#include "util.h"

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include <mutex>
#include <thread>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")

using namespace Microsoft::WRL;
using Json = nlohmann::json;

static std::mutex g_tokMtx;
static std::string g_overlayToken;
static std::string GetToken() { std::lock_guard<std::mutex> lk(g_tokMtx); return g_overlayToken; }
static void SetToken(const std::string& t) { std::lock_guard<std::mutex> lk(g_tokMtx); g_overlayToken = t; }

static void RunBg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

static Json OverlayFetch(const std::string& endpoint, const std::string& method = "GET", const std::string& body = "") {
    std::string tok = GetToken();
    HttpHeaders h = { {"Content-Type", "application/json"}, {"Authorization", "Bearer " + tok} };
    HttpResponse r = HttpSend(method, "https://justiceclient.org" + endpoint, h, body);
    if (!r.ok) return Json::object();
    try { return Json::parse(r.body); } catch (...) { return Json::object(); }
}

static void RegisterSocialData(Bridge& b) {
    b.RegisterAsync("overlay-fetch-friends", [](const Json&, Bridge::ReplyFn reply) {
        if (GetToken().empty()) { reply(Json{ {"friends", Json::array()} }, ""); return; }
        RunBg([reply]() { reply(OverlayFetch("/api/friends.php?action=list"), ""); });
    });
    b.RegisterAsync("overlay-fetch-messages", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        if (GetToken().empty()) { reply(Json{ {"messages", Json::array()} }, ""); return; }
        std::string uid = o.value("userId", ""); long long limit = o.value("limit", 60), after = o.value("after", 0);
        RunBg([=]() {
            std::string ep = "/api/messages.php?userId=" + util::UrlEncode(uid) + "&limit=" + std::to_string(limit);
            if (after > 0) ep += "&after=" + std::to_string(after);
            reply(OverlayFetch(ep), "");
        });
    });
    b.RegisterAsync("overlay-send-message", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        if (GetToken().empty()) { reply(Json{ {"error", "Not logged in"} }, ""); return; }
        std::string uid = o.value("userId", ""), content = o.value("content", "");
        RunBg([=]() {
            Json bodyJson = { {"content", content} };
            reply(OverlayFetch("/api/messages.php?userId=" + util::UrlEncode(uid), "POST", bodyJson.dump()), "");
        });
    });
    b.RegisterAsync("overlay-check-typing", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        if (GetToken().empty()) { reply(Json{ {"typing", false} }, ""); return; }
        std::string uid = o.value("userId", "");
        RunBg([=]() { reply(OverlayFetch("/api/messages.php?action=typing&userId=" + util::UrlEncode(uid)), ""); });
    });
    b.RegisterAsync("overlay-typing", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        if (GetToken().empty()) { reply(Json::object(), ""); return; }
        std::string uid = o.value("userId", "");
        RunBg([=]() { reply(OverlayFetch("/api/messages.php?action=typing&userId=" + util::UrlEncode(uid), "POST", "{}"), ""); });
    });
    b.RegisterDefault([](const Json&) -> Json { return Json(nullptr); });
}

class OverlayWindow {
public:
    static OverlayWindow* g;

    static void Start(Bridge* mainBridge) {
        if (g) return;
        g = new OverlayWindow();
        if (!g->Create()) { delete g; g = nullptr; }
    }
    static void Stop() {
        if (!g) return;
        HWND h = g->hwnd_;
        if (h) DestroyWindow(h);
    }
    static void Toggle() { if (g) g->DoToggle(); }
    static void Notify(const Json& data) { if (g) g->PostEvent("overlay-chat-notification", Json::array({ data })); }

private:
    bool Create() {
        static const wchar_t* kCls = L"JusticeOverlayWnd";
        static bool reg = [] {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = nullptr;
            wc.lpszClassName = kCls; RegisterClassExW(&wc); return true;
        }();
        (void)reg;
        int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kCls, L"Justice Overlay", WS_POPUP, 0, 0, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;
        RegisterHotKey(hwnd_, 1, 0, VK_F9);
        return InitComposition() && InitWebView();
    }

    bool InitComposition() {
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d_, &fl, nullptr))) return false;
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(d3d_.As(&dxgi))) return false;
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_))) return false;
        if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
        return true;
    }

    bool InitWebView() {
        std::wstring udf;
        wchar_t* e = nullptr; size_t n = 0;
        if (_wdupenv_s(&e, &n, L"LOCALAPPDATA") == 0 && e) { udf = e; free(e); }
        udf += L"\\JusticeLauncher\\Overlay";

        auto options = Make<CoreWebView2EnvironmentOptions>();
        options->put_AdditionalBrowserArguments(L"--disable-web-security");

        CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(r) || !env) return r;
                    ComPtr<ICoreWebView2Environment3> env3;
                    if (FAILED(env->QueryInterface(IID_PPV_ARGS(&env3)))) return E_NOINTERFACE;
                    env3->CreateCoreWebView2CompositionController(hwnd_,
                        Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                            [this](HRESULT r, ICoreWebView2CompositionController* comp) -> HRESULT {
                                if (FAILED(r) || !comp) return r;
                                comp_ = comp;
                                comp_.As(&controller_);
                                controller_->get_CoreWebView2(&webview_);

                                ComPtr<ICoreWebView2Controller2> c2;
                                if (SUCCEEDED(controller_.As(&c2))) { COREWEBVIEW2_COLOR t{ 0,0,0,0 }; c2->put_DefaultBackgroundColor(t); }

                                RECT rc; GetClientRect(hwnd_, &rc); controller_->put_Bounds(rc);
                                comp_->put_RootVisualTarget(visual_.Get());
                                target_->SetRoot(visual_.Get());
                                dcomp_->Commit();

                                bridge_ = std::make_unique<Bridge>();
                                bridge_->SetPostCallback([this](const std::wstring& s) { if (webview_) webview_->PostWebMessageAsJson(s.c_str()); });
                                RegisterSocialData(*bridge_);

                                std::wstring shim = Bridge::ToUtf16(Bridge::ShimJs());
                                webview_->AddScriptToExecuteOnDocumentCreated(shim.c_str(), nullptr);
                                EventRegistrationToken tok;
                                webview_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                        LPWSTR j = nullptr; if (SUCCEEDED(a->get_WebMessageAsJson(&j)) && j) { bridge_->HandleMessage(j); CoTaskMemFree(j); } return S_OK;
                                    }).Get(), &tok);

                                ComPtr<ICoreWebView2_3> wv3;
                                if (SUCCEEDED(webview_.As(&wv3)))
                                    wv3->SetVirtualHostNameToFolderMapping(L"justice.app", SrcFolder().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                webview_->Navigate(L"https://justice.app/overlay.html");
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
        return true;
    }

    static std::wstring SrcFolder() {
        std::wstring packaged = util::ExeDir() + L"\\src";
        if (util::Exists(packaged + L"\\overlay.html")) return packaged;
        std::wstring d; wchar_t* e = nullptr; size_t n = 0;
        if (_wdupenv_s(&e, &n, L"JUSTICE_SRC") == 0 && e) { d = e; free(e); }
        else { int len = MultiByteToWideChar(CP_UTF8, 0, JUSTICE_SRC_DIR, -1, nullptr, 0); d.resize(len ? len - 1 : 0); MultiByteToWideChar(CP_UTF8, 0, JUSTICE_SRC_DIR, -1, d.data(), len); }
        for (auto& c : d) if (c == L'/') c = L'\\';
        return d;
    }

    void PostEvent(const std::string& channel, const Json& args) {
        if (bridge_) bridge_->Emit(channel, args);
    }

    void DoToggle() {
        interactive_ = !interactive_;
        LONG ex = GetWindowLong(hwnd_, GWL_EXSTYLE);
        if (interactive_) {
            ex &= ~WS_EX_TRANSPARENT; SetWindowLong(hwnd_, GWL_EXSTYLE, ex);
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(hwnd_);
            if (controller_) controller_->put_IsVisible(TRUE);
        } else {
            ex |= WS_EX_TRANSPARENT; SetWindowLong(hwnd_, GWL_EXSTYLE, ex);
            if (controller_) controller_->put_IsVisible(FALSE);
            ShowWindow(hwnd_, SW_HIDE);
        }
        PostEvent("overlay-toggle", Json::array({ interactive_ }));
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        OverlayWindow* self = (m == WM_NCCREATE)
            ? (OverlayWindow*)((CREATESTRUCT*)l)->lpCreateParams
            : (OverlayWindow*)GetWindowLongPtr(h, GWLP_USERDATA);
        if (m == WM_NCCREATE) { SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)self); self->hwnd_ = h; }
        if (self) switch (m) {
            case WM_NCHITTEST: if (!self->interactive_) return HTTRANSPARENT; break;
            case WM_HOTKEY: self->DoToggle(); return 0;
            case WM_NCDESTROY:
                UnregisterHotKey(h, 1);
                if (self->controller_) self->controller_->Close();
                if (OverlayWindow::g == self) OverlayWindow::g = nullptr;
                delete self; return 0;
        }
        return DefWindowProc(h, m, w, l);
    }

    HWND hwnd_ = nullptr;
    bool interactive_ = false;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<ICoreWebView2CompositionController> comp_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    std::unique_ptr<Bridge> bridge_;
};
OverlayWindow* OverlayWindow::g = nullptr;

void RegisterOverlayHandlers(Bridge& b) {
    Bridge* bp = &b;

    RegisterSocialData(b);

    b.Register("overlay-game-start", [bp](const Json& a) -> Json {
        if (a.is_array() && !a.empty() && a[0].is_string()) SetToken(a[0].get<std::string>());
        OverlayWindow::Start(bp);
        return nullptr;
    });
    b.Register("overlay-game-stop", [](const Json&) -> Json {
        OverlayWindow::Stop(); SetToken(""); return nullptr;
    });
    b.Register("overlay-notify", [](const Json& a) -> Json {
        OverlayWindow::Notify((a.is_array() && !a.empty()) ? a[0] : Json::object()); return nullptr;
    });
    b.Register("overlay-enable-mouse", [](const Json&) -> Json { return nullptr; });
    b.Register("overlay-disable-mouse", [](const Json&) -> Json { return nullptr; });
}
