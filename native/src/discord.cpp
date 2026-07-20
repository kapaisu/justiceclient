#include "bridge.h"
#include "util.h"
#include <windows.h>
#include <mutex>
#include <thread>
#include <cstdint>

using Json = nlohmann::json;

static const char* DISCORD_CLIENT_ID = "1484766794827698277";
static std::mutex g_mtx;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static int64_t g_start = 0;

static int64_t NowSec() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static bool SendFrame(HANDLE pipe, int32_t op, const std::string& json) {
    int32_t hdr[2] = { op, (int32_t)json.size() };
    DWORD wr = 0;
    if (!WriteFile(pipe, hdr, 8, &wr, nullptr) || wr != 8) return false;
    if (!json.empty() && (!WriteFile(pipe, json.data(), (DWORD)json.size(), &wr, nullptr) || wr != json.size())) return false;
    return true;
}

static bool EnsureConnected() {
    if (g_pipe != INVALID_HANDLE_VALUE) return true;
    for (int i = 0; i < 10; i++) {
        std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            Json hello = { {"v", 1}, {"client_id", DISCORD_CLIENT_ID} };
            if (!SendFrame(h, 0 , hello.dump())) { CloseHandle(h); continue; }

            char buf[2048]; DWORD rd = 0; ReadFile(h, buf, sizeof(buf), &rd, nullptr);
            g_pipe = h; g_start = NowSec();
            return true;
        }
    }
    return false;
}

static void SetActivity(const Json& data) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!EnsureConnected()) return;

    Json extra = data.contains("extra") && data["extra"].is_object() ? data["extra"] : data;
    Json activity = {
        {"details", extra.value("details", std::string("In the launcher"))},
        {"state", extra.value("state", std::string("Justice Launcher"))},
        {"timestamps", { {"start", g_start} }},
        {"assets", {
            {"large_image", extra.value("largeImageKey", std::string("justice_logo"))},
            {"large_text", extra.value("largeImageText", std::string("Justice Launcher"))},
        }},
        {"buttons", Json::array({ Json{ {"label", "Get Justice Launcher"}, {"url", "https://justiceclient.org"} } })},
    };
    if (extra.contains("smallImageKey")) {
        activity["assets"]["small_image"] = extra.value("smallImageKey", "");
        activity["assets"]["small_text"] = extra.value("smallImageText", "");
    }
    Json frame = {
        {"cmd", "SET_ACTIVITY"},
        {"args", { {"pid", (int)GetCurrentProcessId()}, {"activity", activity} }},
        {"nonce", std::to_string(NowSec())},
    };
    if (!SendFrame(g_pipe, 1 , frame.dump())) {
        CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE;
    }
}

void RegisterDiscordHandlers(Bridge& b) {

    b.Register("discord-presence", [](const Json& a) -> Json {
        Json data = (a.is_array() && !a.empty() && a[0].is_object()) ? a[0] : Json::object();
        std::thread([data]() { SetActivity(data); }).detach();
        return Json(nullptr);
    });
}
