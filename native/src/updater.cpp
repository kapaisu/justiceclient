#include "bridge.h"
#include "http.h"
#include "download.h"
#include "util.h"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <thread>
#include <string>
#include <atomic>

using namespace Microsoft::WRL;
using Json = nlohmann::json;

static const char* CURRENT_VERSION = "2.2.7";
static const char* VERSION_URL    = "https://justiceclient.org/downloads/version.txt";
static const char* UPDATE_EXE_URL = "https://justiceclient.org/downloads/justice-launcher-setup.exe";
static const UINT  WM_UP_DISPATCH = WM_APP + 1;

static void RunBg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

static std::wstring UpdaterSrcFolder() {
    std::wstring packaged = util::ExeDir() + L"\\src";
    if (util::Exists(packaged + L"\\updater.html")) return packaged;
    std::wstring d; wchar_t* e = nullptr; size_t n = 0;
    if (_wdupenv_s(&e, &n, L"JUSTICE_SRC") == 0 && e) { d = e; free(e); }
    else d = util::Utf8ToWide(JUSTICE_SRC_DIR);
    for (auto& c : d) if (c == L'/') c = L'\\';
    return d;
}

class UpdaterWindow {
public:
    static UpdaterWindow* g;
    static void Start(HWND mainHwnd, bool demo = false) {
        if (g) return;
        g = new UpdaterWindow();
        g->main_ = mainHwnd;
        g->demo_ = demo;
        if (!g->Create()) { delete g; g = nullptr; }
        else if (mainHwnd) ShowWindow(mainHwnd, SW_HIDE);
    }

private:
    bool Create() {
        static const wchar_t* kCls = L"JusticeUpdaterWnd";
        static bool reg = [] {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = CreateSolidBrush(RGB(8, 7, 13));
            wc.lpszClassName = kCls;
            wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
            RegisterClassExW(&wc); return true;
        }();
        (void)reg;
        int w = 520, h = 440;
        int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kCls, L"Justice Launcher — Update",
            WS_POPUP, (sx - w) / 2, (sy - h) / 2, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOW);
        InitWebView();
        return true;
    }

    void InitWebView() {
        std::wstring udf; wchar_t* e = nullptr; size_t n = 0;
        if (_wdupenv_s(&e, &n, L"LOCALAPPDATA") == 0 && e) { udf = e; free(e); }
        udf += L"\\JusticeLauncher\\Updater";
        auto options = Make<CoreWebView2EnvironmentOptions>();

        CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(r) || !env) { Finish(); return r; }
                    env->CreateCoreWebView2Controller(hwnd_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT r, ICoreWebView2Controller* c) -> HRESULT {
                                if (FAILED(r) || !c) { Finish(); return r; }
                                controller_ = c; controller_->get_CoreWebView2(&webview_);
                                RECT rc; GetClientRect(hwnd_, &rc); controller_->put_Bounds(rc);

                                bridge_ = std::make_unique<Bridge>();
                                bridge_->SetPostCallback([this](const std::wstring& s) { if (webview_) webview_->PostWebMessageAsJson(s.c_str()); });
                                bridge_->SetUiDispatcher([this](std::function<void()> fn) {
                                    PostMessage(hwnd_, WM_UP_DISPATCH, 0, (LPARAM)new std::function<void()>(std::move(fn)));
                                });
                                RegisterHandlers();

                                std::wstring shim = Bridge::ToUtf16(Bridge::ShimJs());
                                webview_->AddScriptToExecuteOnDocumentCreated(shim.c_str(), nullptr);
                                EventRegistrationToken tok;
                                webview_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                        LPWSTR j = nullptr; if (SUCCEEDED(a->get_WebMessageAsJson(&j)) && j) { bridge_->HandleMessage(j); CoTaskMemFree(j); } return S_OK;
                                    }).Get(), &tok);
                                ComPtr<ICoreWebView2_3> wv3;
                                if (SUCCEEDED(webview_.As(&wv3)))
                                    wv3->SetVirtualHostNameToFolderMapping(L"justice.app", UpdaterSrcFolder().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                webview_->Navigate(L"https://justice.app/updater.html");
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
    }

    void RegisterHandlers() {
        bridge_->Register("window-minimize", [this](const Json&) -> Json { ShowWindow(hwnd_, SW_MINIMIZE); return nullptr; });
        bridge_->Register("window-close", [this](const Json&) -> Json { CancelAndRestore(); return nullptr; });
        bridge_->Register("__drag-window", [this](const Json&) -> Json { ReleaseCapture(); SendMessage(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0); return nullptr; });
        bridge_->Register("updater-ready", [this](const Json&) -> Json { StartFlow(); return nullptr; });
        bridge_->Register("retry-update", [this](const Json&) -> Json { StartFlow(); return nullptr; });
        bridge_->RegisterDefault([](const Json&) -> Json { return nullptr; });
    }

    void Emit(const std::string& ch, const Json& args) { if (bridge_) bridge_->EmitAsync(ch, args); }

    void DemoSequence() {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        Emit("update-found", Json{ {"current", "2.1.2"}, {"latest", "2.1.3"} });
        unsigned long long total = 82ULL * 1024 * 1024;
        for (int i = 0; i <= 100; i += 2) {
            Emit("update-download-progress", Json{ {"percent", (double)i}, {"transferred", total * i / 100}, {"total", total}, {"speed", 8.4 * 1024 * 1024} });
            std::this_thread::sleep_for(std::chrono::milliseconds(55));
        }
        Emit("update-download-complete", Json::array());
        running_ = false;
    }

    void StartFlow() {
        if (running_.exchange(true)) return;
        Emit("update-checking", Json::array());
        if (demo_) { RunBg([this]() { DemoSequence(); }); return; }
        RunBg([this]() {
            HttpResponse r = HttpSend("GET", VERSION_URL, { {"User-Agent", std::string("JusticeLauncher/") + CURRENT_VERSION} });
            std::string latest = CURRENT_VERSION;
            if (r.ok && r.status == 200) {
                latest = r.body;
                while (!latest.empty() && (latest.back() == '\n' || latest.back() == '\r' || latest.back() == ' ' || latest.back() == '\t')) latest.pop_back();
            }
            if (latest.empty() || latest == CURRENT_VERSION) {
                Emit("update-not-needed", Json::array());
                std::this_thread::sleep_for(std::chrono::milliseconds(1400));
                running_ = false;
                PostToUi([this]() { CancelAndRestore(); });
                return;
            }
            Emit("update-found", Json{ {"current", CURRENT_VERSION}, {"latest", latest} });

            std::wstring tmp; wchar_t* t = nullptr; size_t n = 0;
            if (_wdupenv_s(&t, &n, L"TEMP") == 0 && t) { tmp = t; free(t); }
            std::wstring dir = tmp + L"\\justice-launcher-update"; util::EnsureDir(dir);
            std::wstring dest = dir + L"\\justice-launcher-setup-" + util::Utf8ToWide(latest) + L".exe";

            auto lastT = std::chrono::steady_clock::now();
            unsigned long long lastBytes = 0; double speed = 0;
            bool ok = DownloadFileBytes(UPDATE_EXE_URL, dest,
                [&](unsigned long long got, unsigned long long total) {
                    auto now = std::chrono::steady_clock::now();
                    double ms = std::chrono::duration<double, std::milli>(now - lastT).count();
                    if (ms >= 400) { speed = (double)(got - lastBytes) / (ms / 1000.0); lastT = now; lastBytes = got; }
                    double pct = total ? (double)got / (double)total * 100.0 : 0;
                    Emit("update-download-progress", Json{ {"percent", pct}, {"transferred", got}, {"total", total}, {"speed", speed} });
                });

            if (!ok) { Emit("update-error", Json{ {"message", "Download failed. Check your connection and try again."} }); running_ = false; return; }
            Emit("update-download-complete", Json::array());
            std::this_thread::sleep_for(std::chrono::milliseconds(900));
            PostToUi([this, dest]() {
                ShellExecuteW(nullptr, L"open", dest.c_str(), L"/S", nullptr, SW_SHOWNORMAL);
                PostQuitMessage(0);
            });
        });
    }

    void PostToUi(std::function<void()> fn) { PostMessage(hwnd_, WM_UP_DISPATCH, 0, (LPARAM)new std::function<void()>(std::move(fn))); }

    void CancelAndRestore() {
        if (main_) { ShowWindow(main_, SW_SHOW); SetForegroundWindow(main_); }
        if (hwnd_) DestroyWindow(hwnd_);
    }
    void Finish() { CancelAndRestore(); }

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        UpdaterWindow* self = (m == WM_NCCREATE) ? (UpdaterWindow*)((CREATESTRUCT*)l)->lpCreateParams
                                                 : (UpdaterWindow*)GetWindowLongPtr(h, GWLP_USERDATA);
        if (m == WM_NCCREATE) { SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)self); self->hwnd_ = h; }
        if (self) switch (m) {
            case WM_SIZE: if (self->controller_) { RECT rc; GetClientRect(h, &rc); self->controller_->put_Bounds(rc); } break;
            case WM_UP_DISPATCH: { auto* fn = (std::function<void()>*)l; if (fn) { (*fn)(); delete fn; } return 0; }
            case WM_CLOSE: self->CancelAndRestore(); return 0;
            case WM_NCDESTROY:
                if (self->controller_) self->controller_->Close();
                if (UpdaterWindow::g == self) UpdaterWindow::g = nullptr;
                delete self; return 0;
        }
        return DefWindowProc(h, m, w, l);
    }

    HWND hwnd_ = nullptr, main_ = nullptr;
    bool demo_ = false;
    std::atomic<bool> running_{ false };
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    std::unique_ptr<Bridge> bridge_;
};
UpdaterWindow* UpdaterWindow::g = nullptr;

void RegisterUpdaterHandlers(Bridge& b, void* mainHwndV) {
    HWND mainHwnd = (HWND)mainHwndV;
    b.Register("download-update", [mainHwnd](const Json&) -> Json {
        UpdaterWindow::Start(mainHwnd);
        return Json{ {"ok", true} };
    });
}

int RunUpdaterDemo() {
    UpdaterWindow::Start(nullptr, true);
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
