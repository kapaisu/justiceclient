#include "bridge.h"
#include "http.h"
#include "download.h"
#include "zip.h"
#include "util.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <string>

namespace fs = std::filesystem;
using Json = nlohmann::json;

static void RunBg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }
static Json Arg0(const Json& a) { return (a.is_array() && !a.empty()) ? a[0] : Json(nullptr); }
static std::string ArgStr(const Json& a) { Json v = Arg0(a); return v.is_string() ? v.get<std::string>() : ""; }
static std::string F(const Json& o, const char* k) { return (o.is_object() && o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : ""; }
static std::wstring U(const std::string& s) { std::wstring w = util::Utf8ToWide(s); for (auto& c : w) if (c == L'/') c = L'\\'; return w; }
static std::string Basename(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? p : p.substr(s + 1); }
static Json ReadJson(const std::wstring& p) { std::string t = util::ReadFile(p); if (t.empty()) return Json(nullptr); try { return Json::parse(t); } catch (...) { return Json(nullptr); } }
static void WriteJson(const std::wstring& p, const Json& j) { util::EnsureDir(fs::path(p).parent_path().wstring()); util::WriteFile(p, j.dump(2)); }

static std::vector<std::string> PickFiles(const std::wstring& title,
    const std::vector<std::pair<std::wstring, std::wstring>>& filters, bool multi) {
    std::vector<std::string> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0; dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FILEMUSTEXIST | (multi ? FOS_ALLOWMULTISELECT : 0));
    if (!title.empty()) dlg->SetTitle(title.c_str());
    std::vector<COMDLG_FILTERSPEC> specs;
    for (auto& f : filters) specs.push_back({ f.first.c_str(), f.second.c_str() });
    if (!specs.empty()) dlg->SetFileTypes((UINT)specs.size(), specs.data());
    if (SUCCEEDED(dlg->Show(nullptr))) {
        auto grab = [&](IShellItem* it) { LPWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out.push_back(util::WideToUtf8(p)); CoTaskMemFree(p); } };
        if (multi) { IShellItemArray* arr = nullptr; if (SUCCEEDED(dlg->GetResults(&arr))) { DWORD n = 0; arr->GetCount(&n); for (DWORD i = 0; i < n; i++) { IShellItem* it = nullptr; if (SUCCEEDED(arr->GetItemAt(i, &it))) { grab(it); it->Release(); } } arr->Release(); } }
        else { IShellItem* it = nullptr; if (SUCCEEDED(dlg->GetResult(&it))) { grab(it); it->Release(); } }
    }
    dlg->Release();
    return out;
}

static bool CopyFileToClipboard(const std::wstring& path) {
    std::wstring dbl = path; dbl.push_back(L'\0'); dbl.push_back(L'\0');
    size_t bytes = sizeof(DROPFILES) + dbl.size() * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GHND, bytes);
    if (!h) return false;
    auto* df = (DROPFILES*)GlobalLock(h);
    df->pFiles = sizeof(DROPFILES); df->fWide = TRUE;
    memcpy((BYTE*)df + sizeof(DROPFILES), dbl.data(), dbl.size() * sizeof(wchar_t));
    GlobalUnlock(h);
    bool ok = false;
    if (OpenClipboard(nullptr)) { EmptyClipboard(); if (SetClipboardData(CF_HDROP, h)) ok = true; CloseClipboard(); }
    if (!ok) GlobalFree(h);
    return ok;
}

static std::wstring PerfPresetsPath() { return util::GameDir() + L"\\perf-presets.json"; }

void RegisterExtraHandlers(Bridge& b) {
    Bridge* bp = &b;

    b.RegisterAsync("net-jl", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a);
        std::string path   = F(o, "path");
        std::string method = F(o, "method"); if (method.empty()) method = "GET";
        std::string token  = F(o, "token");
        std::string body;
        if (o.is_object() && o.contains("body")) {
            const Json& bj = o["body"];
            if (bj.is_string()) body = bj.get<std::string>();
            else if (!bj.is_null()) body = bj.dump();
        }
        HttpHeaders h = { {"Content-Type", "application/json"}, {"Accept", "application/json"}, {"User-Agent", "JusticeLauncher"} };
        if (!token.empty()) h.push_back({ "Authorization", "Bearer " + token });
        HttpResponse r = HttpSend(method, "https://justiceclient.org" + path, h, body);
        reply(Json{ {"ok", r.ok}, {"status", r.status}, {"body", r.body} }, "");
    });

    b.Register("open-file-dialog", [](const Json& a) -> Json {
        Json o = Arg0(a); std::wstring title = util::Utf8ToWide(o.is_object() ? o.value("title", "Select File") : "Select File");
        auto files = PickFiles(title, {}, false);
        return files.empty() ? Json::array() : Json(files);
    });
    b.Register("pick-skin-file", [](const Json&) -> Json {
        auto f = PickFiles(L"Select Skin PNG", { {L"PNG Image", L"*.png"} }, false);
        return f.empty() ? Json(nullptr) : Json(f[0]);
    });
    b.Register("pick-shader-file", [](const Json&) -> Json {
        auto f = PickFiles(L"Pick Shader Pack", { {L"Shaders", L"*.zip"} }, true);
        Json out = Json::array(); for (auto& p : f) out.push_back(Json{ {"path", p}, {"name", Basename(p)} });
        return out;
    });
    b.Register("pick-rp-file", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        auto f = PickFiles(L"Add Resource Pack", { {L"Resource Packs", L"*.zip"} }, true);
        if (f.empty()) return Json{ {"cancelled", true} };
        std::wstring dir = util::InstanceDir(vid) + L"\\resourcepacks"; util::EnsureDir(dir);
        int n = 0; std::error_code ec;
        for (auto& p : f) { fs::copy_file(U(p), dir + L"\\" + U(Basename(p)), fs::copy_options::overwrite_existing, ec); if (!ec) n++; }
        return Json{ {"success", true}, {"count", n} };
    });
    b.Register("pick-schematic-file", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        auto f = PickFiles(L"Add Schematic", { {L"Schematics", L"*.litematic;*.schematic;*.schem;*.nbt"} }, true);
        if (f.empty()) return Json{ {"cancelled", true} };
        std::wstring dir = util::InstanceDir(vid) + L"\\schematics"; util::EnsureDir(dir);
        int n = 0; std::error_code ec;
        for (auto& p : f) { fs::copy_file(U(p), dir + L"\\" + U(Basename(p)), fs::copy_options::overwrite_existing, ec); if (!ec) n++; }
        return Json{ {"success", true}, {"count", n} };
    });

    b.Register("install-file", [](const Json& a) -> Json {
        Json o = Arg0(a); std::string kind = F(o, "kind");
        std::wstring sub = kind == "rp" ? L"resourcepacks" : kind == "shaders" ? L"shaderpacks" : kind == "schematics" ? L"schematics" : L"mods";
        std::wstring dir = util::InstanceDir(F(o, "versionId")) + L"\\" + sub; util::EnsureDir(dir);
        std::wstring dest = dir + L"\\" + U(F(o, "fileName"));
        std::string b64 = F(o, "dataB64");
        if (!b64.empty()) {
            std::string bytes = util::Base64Decode(b64);
            if (bytes.empty()) return Json{ {"error", "empty file data"} };
            return util::WriteFile(dest, bytes) ? Json{ {"success", true} } : Json{ {"error", "could not write file"} };
        }
        std::string sp = F(o, "sourcePath");
        if (sp.empty()) return Json{ {"error", "no file data"} };
        std::error_code ec; fs::copy_file(U(sp), dest, fs::copy_options::overwrite_existing, ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("install-shader", [](const Json& a) -> Json {
        Json o = Arg0(a);
        std::wstring dir = util::InstanceDir(F(o, "versionId")) + L"\\shaderpacks"; util::EnsureDir(dir);
        std::error_code ec; fs::copy_file(U(F(o, "sourcePath")), dir + L"\\" + U(F(o, "fileName")), fs::copy_options::overwrite_existing, ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("install-shader-bytes", [](const Json& a) -> Json {
        Json o = Arg0(a);
        std::wstring dir = util::InstanceDir(F(o, "versionId")) + L"\\shaderpacks"; util::EnsureDir(dir);
        std::string bytes;
        if (o.contains("bytes") && o["bytes"].is_array()) for (auto& x : o["bytes"]) bytes += (char)(unsigned char)x.get<int>();
        util::WriteFile(dir + L"\\" + U(F(o, "fileName")), bytes);
        return Json{ {"success", true} };
    });
    b.Register("read-file-base64", [](const Json& a) -> Json {
        std::string p = ArgStr(a); std::string bytes = util::ReadFile(U(p));
        if (bytes.empty()) return Json(nullptr);
        std::string ext; size_t d = p.find_last_of('.'); if (d != std::string::npos) ext = p.substr(d + 1);
        for (auto& c : ext) c = (char)tolower((unsigned char)c); if (ext == "jpg") ext = "jpeg";
        return "data:image/" + ext + ";base64," + util::Base64Encode(bytes);
    });
    b.Register("create-schematics-dir", [](const Json& a) -> Json {
        util::EnsureDir(util::InstanceDir(ArgStr(a)) + L"\\schematics"); return Json{ {"success", true} };
    });
    b.Register("delete-schematic", [](const Json& a) -> Json {
        Json o = Arg0(a); std::error_code ec;
        fs::remove(util::InstanceDir(F(o, "versionId")) + L"\\schematics\\" + U(F(o, "fileName")), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("save-instance-notes", [](const Json& a) -> Json {
        Json o = Arg0(a); std::wstring dir = util::InstanceDir(F(o, "versionId")); util::EnsureDir(dir);
        util::WriteFile(dir + L"\\justice-notes.txt", F(o, "notes"));
        return Json{ {"success", true} };
    });
    b.Register("copy-screenshot", [](const Json& a) -> Json {
        return CopyFileToClipboard(U(ArgStr(a))) ? Json{ {"success", true} } : Json{ {"error", "clipboard copy failed"} };
    });

    b.RegisterAsync("backup-world", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a); std::string vid = F(o, "versionId"), world = F(o, "worldName");
        RunBg([=]() {
            std::wstring wp = util::InstanceDir(vid) + L"\\saves\\" + U(world);
            if (!util::Exists(wp)) { reply(Json{ {"error", "World not found"} }, ""); return; }
            std::wstring dir = util::GameDir() + L"\\backups\\" + U(vid); util::EnsureDir(dir);
            std::wstring dest = dir + L"\\" + U(world) + L"_" + std::to_wstring(GetTickCount64()) + L".zip";
            bool ok = ZipFolder(wp, dest, world);
            reply(ok ? Json{ {"success", true}, {"file", util::WideToUtf8(dest)} } : Json{ {"error", "backup failed"} }, "");
        });
    });
    b.RegisterAsync("restore-backup", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a); std::string vid = F(o, "versionId"), bp = F(o, "backupPath");
        RunBg([=]() {
            std::wstring saves = util::InstanceDir(vid) + L"\\saves"; util::EnsureDir(saves);
            bool ok = UnzipAll(U(bp), saves);
            reply(ok ? Json{ {"success", true} } : Json{ {"error", "restore failed"} }, "");
        });
    });
    b.RegisterAsync("export-instance", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a); std::string vid = F(o, "versionId");
        RunBg([=]() {
            std::wstring inst = util::InstanceDir(vid);
            if (!util::Exists(inst)) { reply(Json{ {"error", "Instance not found"} }, ""); return; }
            std::wstring dest = util::GameDir() + L"\\" + U(vid) + L"-export.zip";
            bool ok = ZipFolder(inst, dest, vid);
            reply(ok ? Json{ {"success", true}, {"file", util::WideToUtf8(dest)} } : Json{ {"error", "export failed"} }, "");
        });
    });
    b.Register("import-instance", [](const Json&) -> Json {
        auto f = PickFiles(L"Import Instance (.zip)", { {L"Zip", L"*.zip"} }, false);
        if (f.empty()) return Json{ {"cancelled", true} };
        std::string name = Basename(f[0]); size_t dot = name.rfind(".zip"); if (dot != std::string::npos) name = name.substr(0, dot);
        std::wstring dest = util::InstanceDir(name);
        bool ok = UnzipAll(U(f[0]), dest);
        return ok ? Json{ {"success", true}, {"id", name} } : Json{ {"error", "import failed"} };
    });

    auto catMeta = []() { return util::GameDir() + L"\\skin-catalog\\catalog.json"; };
    auto catDir = []() { return util::GameDir() + L"\\skin-catalog"; };
    b.Register("skin-catalog-save", [catMeta, catDir](const Json& a) -> Json {
        Json o = Arg0(a); util::EnsureDir(catDir());
        std::string id = std::to_string(GetTickCount64());
        std::wstring dest = catDir() + L"\\" + U(id) + L".png";
        std::string dataUrl = F(o, "dataUrl"), filePath = F(o, "filePath");
        if (!dataUrl.empty()) { size_t p = dataUrl.find("base64,"); util::WriteFile(dest, util::Base64Decode(p == std::string::npos ? dataUrl : dataUrl.substr(p + 7))); }
        else if (!filePath.empty()) { std::error_code ec; fs::copy_file(U(filePath), dest, fs::copy_options::overwrite_existing, ec); }
        else return Json{ {"error", "No skin data provided"} };
        Json entries = ReadJson(catMeta()); if (!entries.is_array()) entries = Json::array();
        Json e = { {"id", id}, {"filename", id + ".png"}, {"name", o.value("name", "Skin")}, {"variant", o.value("variant", "CLASSIC")} };
        entries.insert(entries.begin(), e);
        WriteJson(catMeta(), entries);
        return Json{ {"success", true}, {"id", id} };
    });
    b.Register("skin-catalog-delete", [catMeta, catDir](const Json& a) -> Json {
        Json o = Arg0(a); std::string id = F(o, "id");
        Json entries = ReadJson(catMeta()); if (!entries.is_array()) return Json{ {"success", true} };
        Json keep = Json::array(); std::error_code ec;
        for (auto& e : entries) { if (e.value("id", "") == id) fs::remove(catDir() + L"\\" + U(e.value("filename", "")), ec); else keep.push_back(e); }
        WriteJson(catMeta(), keep); return Json{ {"success", true} };
    });
    b.Register("skin-catalog-rename", [catMeta](const Json& a) -> Json {
        Json o = Arg0(a); std::string id = F(o, "id");
        Json entries = ReadJson(catMeta()); if (!entries.is_array()) return Json{ {"success", true} };
        for (auto& e : entries) if (e.value("id", "") == id) e["name"] = F(o, "name");
        WriteJson(catMeta(), entries); return Json{ {"success", true} };
    });
    b.Register("skin-catalog-get-path", [catMeta, catDir](const Json& a) -> Json {
        Json o = Arg0(a); std::string id = F(o, "id");
        Json entries = ReadJson(catMeta());
        if (entries.is_array()) for (auto& e : entries) if (e.value("id", "") == id)
            return Json{ {"path", util::WideToUtf8(catDir() + L"\\" + U(e.value("filename", "")))}, {"variant", e.value("variant", "CLASSIC")} };
        return Json{ {"error", "Skin not found"} };
    });

    b.RegisterAsync("upload-skin", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a); std::string filePath = F(o, "filePath"), variant = o.value("variant", "classic"), b64 = F(o, "dataB64");
        RunBg([=]() {
            Json auth = AuthGetValidAuth();
            if (auth.is_null()) { reply(Json{ {"error", "Not logged in"} }, ""); return; }
            std::string img = b64.empty() ? util::ReadFile(U(filePath)) : util::Base64Decode(b64);
            if (img.empty()) { reply(Json{ {"error", "Could not read skin file"} }, ""); return; }
            std::string boundary = "----JusticeBoundary" + std::to_string(GetTickCount64());
            std::string var = variant; for (auto& c : var) c = (char)tolower((unsigned char)c);
            std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"variant\"\r\n\r\n" + var + "\r\n"
                + "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"skin.png\"\r\nContent-Type: image/png\r\n\r\n"
                + img + "\r\n--" + boundary + "--\r\n";
            HttpResponse r = HttpSend("POST", "https://api.minecraftservices.com/minecraft/profile/skins",
                { {"Authorization", "Bearer " + auth.value("mcAccessToken", "")}, {"Content-Type", "multipart/form-data; boundary=" + boundary} }, body);
            reply(r.status == 200 ? Json{ {"success", true} } : Json{ {"error", "Server returned " + std::to_string(r.status)} }, "");
        });
    });
    b.RegisterAsync("reset-skin", [](const Json&, Bridge::ReplyFn reply) {
        RunBg([=]() {
            Json auth = AuthGetValidAuth();
            if (auth.is_null()) { reply(Json{ {"error", "Not logged in"} }, ""); return; }
            HttpResponse r = HttpSend("DELETE", "https://api.minecraftservices.com/minecraft/profile/skins/active",
                { {"Authorization", "Bearer " + auth.value("mcAccessToken", "")} });
            reply(r.ok ? Json{ {"success", true} } : Json{ {"error", "reset failed"} }, "");
        });
    });

    b.RegisterAsync("install-perf-preset", [bp](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a); std::string vid = F(o, "versionId"), mc = F(o, "mcVersion");
        bool force = o.value("force", false);
        std::vector<std::string> slugs; if (o.contains("slugs")) for (auto& s : o["slugs"]) if (s.is_string()) slugs.push_back(s);
        RunBg([=]() {
            Json presets = ReadJson(PerfPresetsPath()); if (!presets.is_object()) presets = Json::object();
            std::wstring modDir = util::ModsDir(vid); util::EnsureDir(modDir);
            Json prevFiles = presets.contains(vid) ? presets[vid].value("files", Json::object()) : Json::object();
            if (force) for (auto& s : slugs) if (prevFiles.contains(s)) { std::error_code ec; fs::remove(modDir + L"\\" + U(prevFiles[s].get<std::string>()), ec); }
            Json results = Json::array(); Json fileMap = Json::object();
            for (size_t i = 0; i < slugs.size(); i++) {
                const std::string& slug = slugs[i];
                bp->EmitAsync("perf-progress", Json{ {"msg", "Installing " + slug}, {"progress", (double)i / (slugs.empty() ? 1 : slugs.size())} });
                std::string url = std::string("https://api.modrinth.com/v2/project/") + slug + "/version?game_versions=" +
                    util::UrlEncode("[\"" + mc + "\"]") + "&loaders=" + util::UrlEncode("[\"fabric\"]") + "&limit=1";
                Json vers = HttpJson("GET", url, { {"User-Agent", "JusticeLauncher/1.0"} });
                if (!vers.is_array() || vers.empty()) { results.push_back(Json{ {"slug", slug}, {"error", "No compatible version"} }); continue; }
                Json file; for (auto& f : vers[0].value("files", Json::array())) if (f.value("primary", false)) { file = f; break; }
                if (file.is_null() && vers[0].contains("files") && !vers[0]["files"].empty()) file = vers[0]["files"][0];
                if (file.is_null()) { results.push_back(Json{ {"slug", slug}, {"error", "No file"} }); continue; }
                std::string fn = file.value("filename", slug + ".jar");
                std::wstring dest = modDir + L"\\" + U(fn);
                if (force || !util::Exists(dest)) DownloadFile(file.value("url", ""), dest);
                fileMap[slug] = fn; results.push_back(Json{ {"slug", slug}, {"success", true}, {"fileName", fn} });
            }
            presets[vid] = Json{ {"slugs", slugs}, {"files", fileMap}, {"mcVersion", mc} };
            WriteJson(PerfPresetsPath(), presets);
            bp->EmitAsync("perf-progress", Json{ {"msg", "Done!"}, {"progress", 1} });
            reply(results, "");
        });
    });
    b.Register("get-perf-preset", [](const Json& a) -> Json {
        Json presets = ReadJson(PerfPresetsPath()); std::string vid = ArgStr(a);
        return (presets.is_object() && presets.contains(vid)) ? presets[vid] : Json(nullptr);
    });
    b.Register("clear-perf-preset", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        Json presets = ReadJson(PerfPresetsPath()); if (!presets.is_object() || !presets.contains(vid)) return Json{ {"success", true} };
        std::wstring modDir = util::ModsDir(vid); std::error_code ec;
        for (auto& kv : presets[vid].value("files", Json::object()).items()) fs::remove(modDir + L"\\" + U(kv.value().get<std::string>()), ec);
        presets.erase(vid); WriteJson(PerfPresetsPath(), presets);
        return Json{ {"success", true} };
    });

    b.Register("detect-lunar-client", [](const Json&) -> Json {
        std::wstring d = util::HomeDir() + L"\\.lunarclient";
        return Json{ {"installed", util::Exists(d)}, {"path", util::WideToUtf8(d)} };
    });
    b.Register("detect-feather-client", [](const Json&) -> Json {
        for (std::wstring d : { util::HomeDir() + L"\\.feather", util::HomeDir() + L"\\AppData\\Roaming\\.feather" })
            if (util::Exists(d)) return Json{ {"installed", true}, {"path", util::WideToUtf8(d)} };
        return Json{ {"installed", false} };
    });
    b.Register("get-lunar-mod-equivalents", [](const Json&) -> Json {
        return Json::array({
            {{"lunar","Freelook"},{"modrinth","perspective-mod-redux"},{"desc","Free camera rotation"}},
            {{"lunar","Keystrokes"},{"modrinth","keys"},{"desc","Show WASD/mouse input on screen"}},
            {{"lunar","FPS Display"},{"modrinth","betterf3"},{"desc","Improved debug/FPS overlay"}},
            {{"lunar","Zoom"},{"modrinth","ok-zoomer"},{"desc","Smooth camera zoom"}},
            {{"lunar","Coordinates"},{"modrinth","coordinates-display"},{"desc","Show XYZ coords on screen"}},
            {{"lunar","Minimap"},{"modrinth","xaeros-minimap"},{"desc","In-game minimap"}},
            {{"lunar","Armor Status"},{"modrinth","armorstatus"},{"desc","Show armor durability"}},
            {{"lunar","Potion Effects"},{"modrinth","appleskin"},{"desc","Enhanced potion/food display"}},
            {{"lunar","Scroll Hotbar"},{"modrinth","item-scroller"},{"desc","Scroll through hotbar"}},
            {{"lunar","Chat"},{"modrinth","chatpatches"},{"desc","Chat improvements & timestamps"}},
        });
    });

    b.RegisterAsync("install-modrinth-mod", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a); std::string vid = F(o, "versionId"), slug = F(o, "slug");
        RunBg([=]() {
            Json vers = HttpJson("GET", std::string("https://api.modrinth.com/v2/project/") + slug + "/version", { {"User-Agent", "JusticeLauncher/1.0"} });
            if (!vers.is_array() || vers.empty()) { reply(Json{ {"error", "No versions available"} }, ""); return; }
            Json file; for (auto& f : vers[0].value("files", Json::array())) if (f.value("primary", false)) { file = f; break; }
            if (file.is_null() && vers[0].contains("files") && !vers[0]["files"].empty()) file = vers[0]["files"][0];
            if (file.is_null()) { reply(Json{ {"error", "No downloadable file"} }, ""); return; }
            std::string fn = file.value("filename", slug + ".jar");
            bool ok = DownloadFile(file.value("url", ""), util::ModsDir(vid) + L"\\" + U(fn));
            reply(ok ? Json{ {"success", true}, {"fileName", fn} } : Json{ {"error", "download failed"} }, "");
        });
    });
    b.RegisterAsync("mp-search", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = Arg0(a); std::string q = o.value("query", ""), mc = o.value("mcVersion", ""), loader = o.value("loader", "");
        long long offset = o.value("offset", 0);
        RunBg([=]() {
            std::string facets = "[[\"project_type:modpack\"]";
            if (!mc.empty()) facets += ",[\"versions:" + mc + "\"]";
            if (!loader.empty()) facets += ",[\"categories:" + loader + "\"]";
            facets += "]";
            std::string url = "https://api.modrinth.com/v2/search?query=" + util::UrlEncode(q) + "&limit=20&offset=" +
                std::to_string(offset) + "&index=relevance&facets=" + util::UrlEncode(facets);
            reply(HttpJson("GET", url, { {"User-Agent", "JusticeLauncher/1.0"} }), "");
        });
    });

    b.Register("instance-vulkan-state", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        Json profiles = ReadJson(util::GameDir() + L"\\profiles.json");
        bool vulkan = profiles.is_object() && profiles.contains(vid) && profiles[vid].value("vulkan", false);
        if (!vulkan) { std::error_code ec; std::wstring md = util::ModsDir(vid);
            if (fs::exists(md, ec)) for (auto& e : fs::directory_iterator(md, ec)) { std::string n = util::WideToUtf8(e.path().filename().wstring()); for (auto& c : n) c = (char)tolower((unsigned char)c); if (n.find("vulkanmod") != std::string::npos) { vulkan = true; break; } } }
        return Json{ {"vulkan", vulkan} };
    });
    b.Register("copy-instance-data", [](const Json& a) -> Json {
        Json o = Arg0(a); std::wstring from = util::InstanceDir(F(o, "fromId")), to = util::InstanceDir(F(o, "toId"));
        if (!util::Exists(from)) return Json{ {"error", "Source instance folder is missing"} };
        util::EnsureDir(to); std::error_code ec; Json copied = Json::array();
        for (const wchar_t* f : { L"options.txt", L"servers.dat", L"optionsof.txt" }) {
            if (util::Exists(from + L"\\" + f)) { fs::copy_file(from + L"\\" + f, to + L"\\" + f, fs::copy_options::overwrite_existing, ec); if (!ec) copied.push_back(util::WideToUtf8(f)); }
        }
        if (util::Exists(from + L"\\config")) { fs::copy(from + L"\\config", to + L"\\config", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec); copied.push_back("config/"); }
        return Json{ {"success", true}, {"copied", copied} };
    });

    b.Register("show-native-notification", [](const Json&) -> Json {
        MessageBeep(MB_ICONINFORMATION); return Json{ {"success", true} };
    });
    b.Register("open-java-uninstaller", [](const Json&) -> Json {
        ShellExecuteW(nullptr, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
        return Json{ {"success", true} };
    });
    b.Register("analyse-crash", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        std::string logText = util::ReadFile(util::InstanceDir(vid) + L"\\logs\\latest.log");
        if (logText.empty()) return Json{ {"found", false} };
        if (logText.size() > 80000) logText = logText.substr(logText.size() - 80000);
        struct P { const char* re; const char* title; const char* fix; };
        static const P pats[] = {
            {"OutOfMemoryError", "Out of Memory", "Increase RAM in Settings (3-4 GB for modded)."},
            {"lwjgl", "Missing LWJGL Natives", "Delete the natives folder for this version and relaunch."},
            {"ModResolution", "Mod Conflict", "Two or more mods are incompatible."},
            {"UnsatisfiedLinkError", "Native Library Error", "Delete natives folder and relaunch."},
            {"ClassNotFoundException", "Missing Class", "Install Fabric API and check mod requirements."},
            {"StackOverflowError", "Stack Overflow", "Likely a recursive mod bug; update your mods."},
        };
        Json matches = Json::array();
        std::string low = logText; for (auto& c : low) c = (char)tolower((unsigned char)c);
        for (auto& p : pats) { std::string re = p.re; for (auto& c : re) c = (char)tolower((unsigned char)c); if (low.find(re) != std::string::npos) matches.push_back(Json{ {"title", p.title}, {"fix", p.fix} }); }
        return Json{ {"found", true}, {"matches", matches} };
    });

    for (const char* ch : { "start-server", "stop-server", "server-cmd" })
        b.Register(ch, [](const Json&) -> Json { return Json{ {"error", "Local server hosting isn't supported in the native build yet."} }; });
    b.Register("check-bedrock-installed", [](const Json&) -> Json { return Json{ {"installed", false} }; });
    for (const char* ch : { "launch-bedrock", "create-bedrock-instance" })
        b.Register(ch, [](const Json&) -> Json { return Json{ {"error", "Bedrock Edition isn't supported in the native build yet."} }; });
    b.RegisterAsync("migrate-mods", [](const Json&, Bridge::ReplyFn reply) {
        reply(Json{ {"migrated", Json::array()}, {"failed", Json::array()}, {"skipped", Json::array()} }, "");
    });
}
