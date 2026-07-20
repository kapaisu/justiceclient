#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include "host.h"
#include "bridge.h"

int RunCorsTest();
int RunUpdaterDemo();

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    {
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--selftest") == 0) {
                std::wstring mode = (i + 1 < argc) ? argv[i + 1] : L"";
                std::wstring a1 = (i + 2 < argc) ? argv[i + 2] : L"";
                int rc = (mode == L"cors") ? RunCorsTest()
                       : (mode == L"updater-demo") ? RunUpdaterDemo()
                       : GameSelfTest(mode, a1);
                LocalFree(argv);
                if (SUCCEEDED(co)) CoUninitialize();
                return rc;
            }
        }
        if (argv) LocalFree(argv);
    }

    Host host;
    if (!host.Create(hInst, nCmdShow)) {
        MessageBoxW(nullptr, L"Failed to create the launcher window.",
                    L"Justice Launcher", MB_ICONERROR);
        if (SUCCEEDED(co)) CoUninitialize();
        return 1;
    }

    int rc = host.RunMessageLoop();
    if (SUCCEEDED(co)) CoUninitialize();
    return rc;
}
