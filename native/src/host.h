#pragma once
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <memory>
#include <string>
#include "bridge.h"

class Host {
public:
    bool Create(HINSTANCE hInst, int nCmdShow);
    int  RunMessageLoop();
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void InitWebView();
    void ResizeToClient();
    void RegisterWindowHandlers();
    std::wstring SrcFolder() const;

    HWND        hwnd_    = nullptr;
    HINSTANCE   hinst_   = nullptr;
    bool        devMode_ = false;
    bool        wasMax_  = false;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2>           webview_;
    std::unique_ptr<Bridge>                         bridge_;
};
