#include "host.h"
#include "util.h"
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wrl.h>
#include <WebView2EnvironmentOptions.h>

using namespace Microsoft::WRL;

static const wchar_t* kClassName = L"JusticeLauncherWnd";
static const wchar_t* kVirtualHost = L"justice.app";

static const UINT WM_APP_DISPATCH = WM_APP + 1;

static std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::wstring Host::SrcFolder() const {

    std::wstring packaged = util::ExeDir() + L"\\src";
    if (util::Exists(packaged + L"\\index.html")) return packaged;

    std::wstring dir;
    wchar_t* env = nullptr; size_t len = 0;
    if (_wdupenv_s(&env, &len, L"JUSTICE_SRC") == 0 && env) { dir = env; free(env); }
    else dir = Widen(JUSTICE_SRC_DIR);
    for (auto& ch : dir) if (ch == L'/') ch = L'\\';
    return dir;
}

bool Host::Create(HINSTANCE hInst, int nCmdShow) {
    hinst_   = hInst;
    devMode_ = wcsstr(GetCommandLineW(), L"--dev") != nullptr;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProcThunk;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(8, 6, 14));
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) {
        std::wstring iconPath = SrcFolder() + L"\\..\\assets\\icon.ico";
        wc.hIcon = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, kClassName, L"Justice Launcher",
                            WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680,
                            nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    bridge_ = std::make_unique<Bridge>();
    bridge_->SetUiDispatcher([this](std::function<void()> fn) {
        PostMessage(hwnd_, WM_APP_DISPATCH, 0,
                    reinterpret_cast<LPARAM>(new std::function<void()>(std::move(fn))));
    });
    RegisterWindowHandlers();
    RegisterCoreHandlers(*bridge_);
    RegisterAuthHandlers(*bridge_);
    RegisterGameHandlers(*bridge_);
    RegisterModHandlers(*bridge_);
    RegisterExtraHandlers(*bridge_);
    RegisterDiscordHandlers(*bridge_);
    RegisterOverlayHandlers(*bridge_);
    RegisterUpdaterHandlers(*bridge_, hwnd_);

    MARGINS mg{ 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd_, &mg);

    ShowWindow(hwnd_, nCmdShow <= 0 ? SW_SHOW : nCmdShow);
    InitWebView();
    return true;
}

int Host::RunMessageLoop() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

void Host::RegisterWindowHandlers() {
    bridge_->Register("window-minimize", [this](const Bridge::Json&) -> Bridge::Json {
        ShowWindow(hwnd_, SW_MINIMIZE); return nullptr;
    });
    bridge_->Register("window-maximize", [this](const Bridge::Json&) -> Bridge::Json {
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); return nullptr;
    });
    bridge_->Register("window-close", [this](const Bridge::Json&) -> Bridge::Json {
        PostMessage(hwnd_, WM_CLOSE, 0, 0); return nullptr;
    });
    bridge_->Register("__drag-window", [this](const Bridge::Json&) -> Bridge::Json {
        ReleaseCapture();
        SendMessage(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return nullptr;
    });

    auto openUrlArg = [](const Bridge::Json& a) -> Bridge::Json {
        if (a.is_array() && !a.empty() && a[0].is_string())
            ShellExecuteW(nullptr, L"open", Bridge::ToUtf16(a[0].get<std::string>()).c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        return nullptr;
    };
    bridge_->Register("open-external", openUrlArg);
    bridge_->Register("open-url", openUrlArg);

    auto argVid = [](const Bridge::Json& a) -> std::string {
        if (a.is_array() && !a.empty() && a[0].is_string()) return a[0].get<std::string>();
        if (a.is_string()) return a.get<std::string>();
        return std::string();
    };
    auto revealDir = [](std::wstring dir) {
        if (dir.empty()) return;
        util::EnsureDir(dir);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    bridge_->Register("open-game-dir", [revealDir](const Bridge::Json&) -> Bridge::Json {
        revealDir(util::GameDir()); return nullptr;
    });
    bridge_->Register("open-instance-dir", [revealDir, argVid](const Bridge::Json& a) -> Bridge::Json {
        std::string v = argVid(a);
        revealDir(v.empty() ? util::GameDir() : util::InstanceDir(v)); return nullptr;
    });
    bridge_->Register("open-mods-dir", [revealDir, argVid](const Bridge::Json& a) -> Bridge::Json {
        std::string v = argVid(a);
        revealDir(v.empty() ? util::GameDir() : util::ModsDir(v)); return nullptr;
    });
    bridge_->Register("open-saves-dir", [revealDir, argVid](const Bridge::Json& a) -> Bridge::Json {
        std::string v = argVid(a);
        revealDir(v.empty() ? util::GameDir() : util::InstanceDir(v) + L"\\saves"); return nullptr;
    });
    bridge_->Register("open-shaders-dir", [revealDir, argVid](const Bridge::Json& a) -> Bridge::Json {
        std::string v = argVid(a);
        revealDir(v.empty() ? util::GameDir() : util::InstanceDir(v) + L"\\shaderpacks"); return nullptr;
    });
    bridge_->Register("open-screenshots-dir", [revealDir, argVid](const Bridge::Json& a) -> Bridge::Json {
        std::string v = argVid(a);
        revealDir(v.empty() ? util::GameDir() : util::InstanceDir(v) + L"\\screenshots"); return nullptr;
    });
}

void Host::InitWebView() {
    std::wstring udf;
    {
        wchar_t* env = nullptr; size_t len = 0;
        if (_wdupenv_s(&env, &len, L"LOCALAPPDATA") == 0 && env) { udf = env; free(env); }
    }
    udf += L"\\JusticeLauncher\\WebView2";

    auto options = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-web-security");

    CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(r) || !env) {
                    MessageBoxW(hwnd_, L"Failed to initialize WebView2 runtime.",
                                L"Justice Launcher", MB_ICONERROR);
                    return r;
                }
                env->CreateCoreWebView2Controller(hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT r, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(r) || !controller) return r;
                            controller_ = controller;
                            controller_->get_CoreWebView2(&webview_);

                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(controller_.As(&c2))) {
                                COREWEBVIEW2_COLOR bg{ 255, 8, 6, 14 };
                                c2->put_DefaultBackgroundColor(bg);
                            }

                            ComPtr<ICoreWebView2Settings> s;
                            if (SUCCEEDED(webview_->get_Settings(&s)) && s) {
                                s->put_AreDevToolsEnabled(devMode_ ? TRUE : FALSE);
                                s->put_AreDefaultContextMenusEnabled(devMode_ ? TRUE : FALSE);
                                s->put_IsStatusBarEnabled(FALSE);
                                s->put_IsZoomControlEnabled(FALSE);
                            }

                            std::wstring shim = Bridge::ToUtf16(Bridge::ShimJs());
                            webview_->AddScriptToExecuteOnDocumentCreated(shim.c_str(), nullptr);

                            EventRegistrationToken tok;
                            webview_->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                        LPWSTR json = nullptr;
                                        if (SUCCEEDED(a->get_WebMessageAsJson(&json)) && json) {
                                            bridge_->HandleMessage(json);
                                            CoTaskMemFree(json);
                                        }
                                        return S_OK;
                                    }).Get(), &tok);

                            EventRegistrationToken navTok, winTok;
                            webview_->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(a->get_Uri(&uri)) && uri) {
                                            if (wcsncmp(uri, L"https://justice.app", 19) != 0 &&
                                                wcsncmp(uri, L"about:", 6) != 0) {
                                                a->put_Cancel(TRUE);
                                                ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                                            }
                                            CoTaskMemFree(uri);
                                        }
                                        return S_OK;
                                    }).Get(), &navTok);
                            webview_->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(a->get_Uri(&uri)) && uri) {
                                            ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                                            CoTaskMemFree(uri);
                                        }
                                        a->put_Handled(TRUE);
                                        return S_OK;
                                    }).Get(), &winTok);

                            ComPtr<ICoreWebView2_3> wv3;
                            if (SUCCEEDED(webview_.As(&wv3))) {
                                wv3->SetVirtualHostNameToFolderMapping(
                                    kVirtualHost, SrcFolder().c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            bridge_->SetPostCallback([this](const std::wstring& s) {
                                if (webview_) webview_->PostWebMessageAsJson(s.c_str());
                            });

                            ResizeToClient();
                            webview_->Navigate(L"https://justice.app/index.html");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void Host::ResizeToClient() {
    if (!controller_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    controller_->put_Bounds(rc);
}

LRESULT CALLBACK Host::WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    Host* self = nullptr;
    if (m == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(l);
        self = reinterpret_cast<Host*>(cs->lpCreateParams);
        SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<Host*>(GetWindowLongPtr(h, GWLP_USERDATA));
    }
    return self ? self->WndProc(h, m, w, l) : DefWindowProc(h, m, w, l);
}

LRESULT Host::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE:
        if (w == TRUE) return 0;
        break;

    case WM_NCHITTEST: {
        if (IsZoomed(h)) return HTCLIENT;
        const LONG b = 6;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        RECT rc; GetWindowRect(h, &rc);
        bool left = pt.x < rc.left + b, right = pt.x >= rc.right - b;
        bool top = pt.y < rc.top + b, bottom = pt.y >= rc.bottom - b;
        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)   return HTLEFT;
        if (right)  return HTRIGHT;
        if (top)    return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(l);
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfo(mon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
        mmi->ptMinTrackSize.x = MulDiv(900, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(600, dpi, 96);
        return 0;
    }

    case WM_SIZE: {
        ResizeToClient();
        bool mx = IsZoomed(h) != 0;
        if (mx != wasMax_) {
            wasMax_ = mx;
            if (bridge_) bridge_->Emit("window-state",
                Bridge::Json::array({ mx ? "maximized" : "restored" }));
        }
        break;
    }

    case WM_DPICHANGED: {
        RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_ACTIVATE:
        return 0;

    case WM_APP_DISPATCH: {
        auto* fn = reinterpret_cast<std::function<void()>*>(l);
        if (fn) { (*fn)(); delete fn; }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}
