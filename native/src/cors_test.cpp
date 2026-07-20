#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include "util.h"
#include <fstream>

using namespace Microsoft::WRL;

static std::wstring CorsLogPath() {
    std::wstring lp; wchar_t* t = nullptr; size_t n = 0;
    if (_wdupenv_s(&t, &n, L"TEMP") == 0 && t) { lp = std::wstring(t) + L"\\jl_selftest.log"; free(t); }
    return lp;
}

int RunCorsTest() {
    { std::ofstream f(CorsLogPath(), std::ios::trunc); }
    auto write = [](const std::string& s) { std::ofstream f(CorsLogPath(), std::ios::app | std::ios::binary); f << s; };

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool done = false;

    std::wstring udf; { wchar_t* e = nullptr; size_t m = 0; if (_wdupenv_s(&e, &m, L"LOCALAPPDATA") == 0 && e) { udf = e; free(e); } }
    udf += L"\\JusticeLauncher\\CorsTest";

    auto options = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-web-security");

    CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(r) || !env) { write("CORS ENV FAIL\n"); done = true; PostQuitMessage(0); return r; }
                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [&](HRESULT r, ICoreWebView2Controller* c) -> HRESULT {
                            if (FAILED(r) || !c) { write("CORS CONTROLLER FAIL\n"); done = true; PostQuitMessage(0); return r; }
                            controller = c; controller->get_CoreWebView2(&webview);
                            EventRegistrationToken tok;
                            webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [&](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                    LPWSTR s = nullptr;
                                    if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) { write(util::WideToUtf8(s) + "\n"); CoTaskMemFree(s); }
                                    done = true; PostQuitMessage(0); return S_OK;
                                }).Get(), &tok);
                            const wchar_t* html =
                                L"data:text/html,<script>"
                                L"fetch('https://justiceclient.org/api/user.php?action=me')"
                                L".then(r=>r.text().then(t=>chrome.webview.postMessage('CORS OK status='+r.status+' body='+t.slice(0,60))))"
                                L".catch(e=>chrome.webview.postMessage('CORS FAIL '+e))"
                                L"</script>";
                            webview->Navigate(html);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    MSG msg; DWORD start = GetTickCount();
    while (!done && GetTickCount() - start < 20000) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else Sleep(10);
    }
    if (!done) write("CORS TIMEOUT (no result in 20s)\n");
    return 0;
}
