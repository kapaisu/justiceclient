#include "bridge.h"
#include "http.h"
#include "util.h"
#include <windows.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;
using Json = nlohmann::json;

static Json ReadJson(const std::wstring& path) {
    std::string t = util::ReadFile(path);
    if (t.empty()) return Json(nullptr);
    try { return Json::parse(t); } catch (...) { return Json(nullptr); }
}
static void WriteJson(const std::wstring& path, const Json& j) {
    util::EnsureDir(util::GameDir());
    util::WriteFile(path, j.dump(2));
}
static Json Arg0(const Json& a) { return (a.is_array() && !a.empty()) ? a[0] : Json(nullptr); }
static std::string ArgStr(const Json& a) { Json v = Arg0(a); return v.is_string() ? v.get<std::string>() : ""; }
static std::string Field(const Json& o, const char* k) {
    return (o.is_object() && o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : "";
}
static void RunBg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

static std::wstring ProfilesPath() { return util::GameDir() + L"\\profiles.json"; }
static std::wstring ServersPath()  { return util::GameDir() + L"\\servers.json"; }

template <class F>
static void ForEach(const std::wstring& dir, F cb) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code fec;
        std::string name = util::WideToUtf8(e.path().filename().wstring());
        cb(name, e.path().wstring(), e.is_directory(fec));
    }
}
static bool EndsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

void RegisterCoreHandlers(Bridge& b) {

    b.Register("get-settings", [](const Json&) -> Json {
        Json j = ReadJson(util::GameDir() + L"\\settings.json");
        return j.is_object() ? j : Json::object();
    });
    b.Register("save-settings", [](const Json& a) -> Json {
        if (a.is_array() && !a.empty()) WriteJson(util::GameDir() + L"\\settings.json", a[0]);
        return Json{ {"ok", true} };
    });
    b.Register("get-theme-enabled", [](const Json&) -> Json {
        Json j = ReadJson(util::GameDir() + L"\\settings.json");
        return j.is_object() ? j.value("themeEnabled", false) : false;
    });
    b.Register("set-theme-enabled", [](const Json& a) -> Json {
        Json j = ReadJson(util::GameDir() + L"\\settings.json");
        if (!j.is_object()) j = Json::object();
        j["themeEnabled"] = (a.is_array() && !a.empty()) ? a[0] : false;
        WriteJson(util::GameDir() + L"\\settings.json", j);
        return Json{ {"ok", true} };
    });
    b.Register("get-system-info", [](const Json&) -> Json {
        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
        SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
        const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                         : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86";
        return Json{ {"platform", "win32"}, {"arch", arch},
                     {"totalMemMb", (long long)(ms.ullTotalPhys / 1048576ull)},
                     {"freeMemMb", (long long)(ms.ullAvailPhys / 1048576ull)},
                     {"cpus", (int)si.dwNumberOfProcessors} };
    });

    b.Register("get-installed-versions", [](const Json&) -> Json {
        Json j = ReadJson(ProfilesPath());
        return j.is_object() ? j : Json::object();
    });
    b.Register("rename-version", [](const Json& a) -> Json {
        Json o = Arg0(a);
        std::string vid = Field(o, "versionId");
        Json profiles = ReadJson(ProfilesPath());
        if (!profiles.is_object() || !profiles.contains(vid)) return Json{ {"error", "Version not found"} };
        std::string cn = Field(o, "customName");
        profiles[vid]["customName"] = cn.empty() ? Json(nullptr) : Json(cn);
        WriteJson(ProfilesPath(), profiles);
        return Json{ {"success", true} };
    });
    b.Register("delete-version", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        Json profiles = ReadJson(ProfilesPath());
        if (profiles.is_object() && profiles.contains(vid)) { profiles.erase(vid); WriteJson(ProfilesPath(), profiles); }
        std::error_code ec;
        fs::remove_all(util::GameDir() + L"\\versions\\" + util::Utf8ToWide(vid), ec);
        fs::remove_all(util::InstanceDir(vid), ec);
        return Json{ {"success", true} };
    });

    b.Register("get-mods", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        std::wstring dir = util::ModsDir(vid);
        Json out = Json::array();
        ForEach(dir, [&](const std::string& name, const std::wstring& full, bool) {
            if (!EndsWith(name, ".jar") && !EndsWith(name, ".jar.disabled")) return;
            std::string disp = name;
            size_t p;
            if ((p = disp.rfind(".jar.disabled")) != std::string::npos) disp = disp.substr(0, p);
            else if ((p = disp.rfind(".jar")) != std::string::npos) disp = disp.substr(0, p);
            out.push_back({ {"name", name}, {"displayName", disp}, {"enabled", EndsWith(name, ".jar")},
                           {"size", util::FileSize(full)}, {"addedAt", util::CreationIso8601(full)},
                           {"path", util::WideToUtf8(full)} });
        });
        std::sort(out.begin(), out.end(), [](const Json& x, const Json& y) {
            return x.value("displayName", "") < y.value("displayName", "");
        });
        return out;
    });
    b.Register("get-mod-count", [](const Json& a) -> Json {
        int n = 0;
        ForEach(util::ModsDir(ArgStr(a)), [&](const std::string& name, const std::wstring&, bool) {
            if (EndsWith(name, ".jar") || EndsWith(name, ".jar.disabled")) n++;
        });
        return n;
    });
    b.Register("install-mod", [](const Json& a) -> Json {
        Json o = Arg0(a);
        std::string vid = Field(o, "versionId"), fn = Field(o, "fileName");
        if (vid.empty() || fn.empty()) return Json{ {"error", "missing versionId or fileName"} };
        util::EnsureDir(util::ModsDir(vid));
        std::wstring dest = util::ModsDir(vid) + L"\\" + util::Utf8ToWide(fn);
        std::string b64 = Field(o, "dataB64");
        if (!b64.empty()) {
            std::string bytes = util::Base64Decode(b64);
            if (bytes.empty()) return Json{ {"error", "empty mod data"} };
            return util::WriteFile(dest, bytes) ? Json{ {"success", true} } : Json{ {"error", "could not write mod file"} };
        }
        std::string sp = Field(o, "sourcePath");
        if (sp.empty()) return Json{ {"error", "no mod data"} };
        std::error_code ec;
        fs::copy_file(util::Utf8ToWide(sp), dest, fs::copy_options::overwrite_existing, ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("delete-mod", [](const Json& a) -> Json {
        Json o = Arg0(a);
        std::error_code ec;
        fs::remove(util::ModsDir(Field(o, "versionId")) + L"\\" + util::Utf8ToWide(Field(o, "fileName")), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("toggle-mod", [](const Json& a) -> Json {
        Json o = Arg0(a);
        std::string vid = Field(o, "versionId"), fn = Field(o, "fileName");
        std::wstring dir = util::ModsDir(vid);
        std::string nn = EndsWith(fn, ".jar.disabled") ? fn.substr(0, fn.size() - 9) : fn + ".disabled";
        std::error_code ec;
        fs::rename(dir + L"\\" + util::Utf8ToWide(fn), dir + L"\\" + util::Utf8ToWide(nn), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true}, {"newName", nn} };
    });

    auto listPacks = [](const std::wstring& dir) -> Json {
        Json out = Json::array();
        ForEach(dir, [&](const std::string& name, const std::wstring& full, bool isDir) {
            if (!isDir && !EndsWith(name, ".zip") && !EndsWith(name, ".zip.disabled")) return;
            out.push_back({ {"name", name}, {"enabled", !EndsWith(name, ".disabled")},
                           {"size", isDir ? 0 : (long long)util::FileSize(full)} });
        });
        std::sort(out.begin(), out.end(), [](const Json& x, const Json& y) {
            return x.value("name", "") < y.value("name", "");
        });
        return out;
    };
    auto countPacks = [](const std::wstring& dir) -> int {
        int n = 0;
        ForEach(dir, [&](const std::string& name, const std::wstring&, bool) {
            if (EndsWith(name, ".zip") || EndsWith(name, ".zip.disabled")) n++;
        });
        return n;
    };
    auto togglePack = [](const std::wstring& dir, const std::string& fn) -> Json {
        std::string nn = EndsWith(fn, ".disabled") ? fn.substr(0, fn.size() - 9) : fn + ".disabled";
        std::error_code ec;
        fs::rename(dir + L"\\" + util::Utf8ToWide(fn), dir + L"\\" + util::Utf8ToWide(nn), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true}, {"newName", nn} };
    };

    b.Register("get-shaders", [listPacks](const Json& a) -> Json {
        return listPacks(util::InstanceDir(ArgStr(a)) + L"\\shaderpacks");
    });
    b.Register("get-shader-count", [countPacks](const Json& a) -> Json {
        return countPacks(util::InstanceDir(ArgStr(a)) + L"\\shaderpacks");
    });
    b.Register("delete-shader", [](const Json& a) -> Json {
        Json o = Arg0(a); std::error_code ec;
        fs::remove(util::InstanceDir(Field(o, "versionId")) + L"\\shaderpacks\\" + util::Utf8ToWide(Field(o, "fileName")), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("toggle-shader", [togglePack](const Json& a) -> Json {
        Json o = Arg0(a);
        return togglePack(util::InstanceDir(Field(o, "versionId")) + L"\\shaderpacks", Field(o, "fileName"));
    });
    b.Register("get-resource-packs", [listPacks](const Json& a) -> Json {
        return listPacks(util::InstanceDir(ArgStr(a)) + L"\\resourcepacks");
    });
    b.Register("get-resource-pack-count", [countPacks](const Json& a) -> Json {
        return countPacks(util::InstanceDir(ArgStr(a)) + L"\\resourcepacks");
    });
    b.Register("delete-rp", [](const Json& a) -> Json {
        Json o = Arg0(a); std::error_code ec;
        fs::remove(util::InstanceDir(Field(o, "versionId")) + L"\\resourcepacks\\" + util::Utf8ToWide(Field(o, "fileName")), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("toggle-rp", [togglePack](const Json& a) -> Json {
        Json o = Arg0(a);
        return togglePack(util::InstanceDir(Field(o, "versionId")) + L"\\resourcepacks", Field(o, "fileName"));
    });

    b.Register("get-worlds", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        std::wstring saves = util::InstanceDir(vid) + L"\\saves";
        Json out = Json::array();
        ForEach(saves, [&](const std::string& name, const std::wstring& full, bool isDir) {
            if (!isDir) return;
            out.push_back({ {"name", name}, {"path", util::WideToUtf8(full)},
                           {"modified", util::LastWriteMs(full)}, {"size", util::DirSize(full)} });
        });
        std::sort(out.begin(), out.end(), [](const Json& x, const Json& y) {
            return x.value("modified", 0ull) > y.value("modified", 0ull);
        });
        return out;
    });
    b.Register("get-world-count", [](const Json& a) -> Json {
        int n = 0;
        ForEach(util::InstanceDir(ArgStr(a)) + L"\\saves", [&](const std::string&, const std::wstring&, bool isDir) {
            if (isDir) n++;
        });
        return n;
    });
    b.Register("get-backups", [](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        std::wstring dir = util::GameDir() + L"\\backups\\" + util::Utf8ToWide(vid);
        Json out = Json::array();
        ForEach(dir, [&](const std::string& name, const std::wstring& full, bool) {
            if (!EndsWith(name, ".zip")) return;
            out.push_back({ {"name", name}, {"path", util::WideToUtf8(full)},
                           {"size", util::FileSize(full)}, {"created", util::LastWriteMs(full)} });
        });
        std::sort(out.begin(), out.end(), [](const Json& x, const Json& y) {
            return x.value("created", 0ull) > y.value("created", 0ull);
        });
        return out;
    });
    b.Register("delete-backup", [](const Json& a) -> Json {
        std::error_code ec; fs::remove(util::Utf8ToWide(ArgStr(a)), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });

    auto isImg = [](const std::string& n) {
        std::string l = Lower(n);
        return EndsWith(l, ".png") || EndsWith(l, ".jpg") || EndsWith(l, ".jpeg");
    };
    b.Register("get-screenshots", [isImg](const Json& a) -> Json {
        std::string vid = ArgStr(a);
        std::wstring dir = util::InstanceDir(vid) + L"\\screenshots";
        Json out = Json::array();
        ForEach(dir, [&](const std::string& name, const std::wstring& full, bool) {
            if (!isImg(name)) return;
            out.push_back({ {"name", name}, {"path", util::WideToUtf8(full)}, {"size", util::FileSize(full)},
                           {"created", util::LastWriteMs(full)}, {"versionId", vid} });
        });
        std::sort(out.begin(), out.end(), [](const Json& x, const Json& y) {
            return x.value("created", 0ull) > y.value("created", 0ull);
        });
        return out;
    });
    b.Register("get-all-screenshots", [isImg](const Json&) -> Json {
        Json profiles = ReadJson(ProfilesPath());
        Json out = Json::array();
        if (profiles.is_object()) {
            for (auto it = profiles.begin(); it != profiles.end(); ++it) {
                std::string vid = it.key();
                std::wstring dir = util::InstanceDir(vid) + L"\\screenshots";
                ForEach(dir, [&](const std::string& name, const std::wstring& full, bool) {
                    if (!isImg(name)) return;
                    out.push_back({ {"name", name}, {"path", util::WideToUtf8(full)}, {"versionId", vid},
                                   {"size", util::FileSize(full)}, {"created", util::LastWriteMs(full)} });
                });
            }
        }
        std::sort(out.begin(), out.end(), [](const Json& x, const Json& y) {
            return x.value("created", 0ull) > y.value("created", 0ull);
        });
        return out;
    });
    b.Register("delete-screenshot", [](const Json& a) -> Json {
        std::error_code ec; fs::remove(util::Utf8ToWide(ArgStr(a)), ec);
        return ec ? Json{ {"error", ec.message()} } : Json{ {"success", true} };
    });
    b.Register("screenshot-to-dataurl", [](const Json& a) -> Json {
        std::string p = ArgStr(a);
        std::string bytes = util::ReadFile(util::Utf8ToWide(p));
        if (bytes.empty()) return Json(nullptr);
        std::string l = Lower(p);
        std::string mime = (EndsWith(l, ".jpg") || EndsWith(l, ".jpeg")) ? "image/jpeg" : "image/png";
        return "data:" + mime + ";base64," + util::Base64Encode(bytes);
    });

    b.Register("get-schematics", [](const Json& a) -> Json {
        std::wstring dir = util::InstanceDir(ArgStr(a)) + L"\\schematics";
        if (!util::Exists(dir)) return Json{ {"exists", false}, {"files", Json::array()} };
        Json files = Json::array();
        ForEach(dir, [&](const std::string& name, const std::wstring& full, bool) {
            std::string l = Lower(name);
            if (EndsWith(l, ".litematic") || EndsWith(l, ".schematic") || EndsWith(l, ".schem") || EndsWith(l, ".nbt"))
                files.push_back({ {"name", name}, {"size", util::FileSize(full)} });
        });
        std::sort(files.begin(), files.end(), [](const Json& x, const Json& y) {
            return x.value("name", "") < y.value("name", "");
        });
        return Json{ {"exists", true}, {"files", files} };
    });

    b.Register("get-servers", [](const Json&) -> Json {
        Json j = ReadJson(ServersPath());
        return j.is_array() ? j : Json::array();
    });
    b.Register("save-server", [](const Json& a) -> Json {
        Json o = Arg0(a);
        Json list = ReadJson(ServersPath()); if (!list.is_array()) list = Json::array();
        std::string id = Field(o, "id");
        if (id.empty()) { char buf[24]; snprintf(buf, sizeof(buf), "%llx", (unsigned long long)GetTickCount64()); id = buf; }
        int port = 25565;
        if (o.contains("port")) { if (o["port"].is_number()) port = o["port"].get<int>();
                                  else if (o["port"].is_string()) { try { port = std::stoi(o["port"].get<std::string>()); } catch (...) {} } }
        Json entry = { {"id", id}, {"name", Field(o, "name")}, {"address", Field(o, "address")}, {"port", port} };
        bool replaced = false;
        for (auto& s : list) if (s.value("id", "") == id) { s = entry; replaced = true; break; }
        if (!replaced) list.push_back(entry);
        WriteJson(ServersPath(), list);
        return Json{ {"success", true}, {"server", entry} };
    });
    b.Register("delete-server", [](const Json& a) -> Json {
        std::string id = ArgStr(a);
        Json list = ReadJson(ServersPath()); if (!list.is_array()) list = Json::array();
        Json keep = Json::array();
        for (auto& s : list) if (s.value("id", "") != id) keep.push_back(s);
        WriteJson(ServersPath(), keep);
        return Json{ {"success", true} };
    });

    b.Register("get-perf-mods", [](const Json&) -> Json {
        return Json{
            {"sodium",       {{"slug", "sodium"}, {"name", "Sodium"}, {"desc", "Modern rendering engine — massive FPS boost"}}},
            {"iris",         {{"slug", "iris"}, {"name", "Iris Shaders"}, {"desc", "Shader support for Fabric + Sodium"}}},
            {"lithium",      {{"slug", "lithium"}, {"name", "Lithium"}, {"desc", "Game logic optimisations, better TPS"}}},
            {"indium",       {{"slug", "indium"}, {"name", "Indium"}, {"desc", "Sodium addon for mod compatibility"}}},
            {"ferritecore",  {{"slug", "ferrite-core"}, {"name", "FerriteCore"}, {"desc", "Significantly reduces memory usage"}}},
            {"entityculling",{{"slug", "entityculling"}, {"name", "Entity Culling"}, {"desc", "Skip rendering hidden entities"}}},
        };
    });

    b.RegisterAsync("get-versions", [](const Json&, Bridge::ReplyFn reply) {
        RunBg([reply] {
            Json m = HttpJson("GET", "https://launchermeta.mojang.com/mc/game/version_manifest_v2.json");
            if (!m.is_object() || !m.contains("versions")) { reply(Json{ {"error", "Could not load version manifest"} }, ""); return; }
            Json out = Json::array();
            for (auto& v : m["versions"])
                out.push_back({ {"id", v.value("id", "")}, {"type", v.value("type", "")},
                               {"url", v.value("url", "")}, {"releaseTime", v.value("releaseTime", "")} });
            reply(out, "");
        });
    });
    b.RegisterAsync("get-fabric-versions", [](const Json& a, Bridge::ReplyFn reply) {
        std::string mc = ArgStr(a);
        RunBg([mc, reply] {
            Json l;
            for (int attempt = 0; attempt < 3; attempt++) {
                l = HttpJson("GET", "https://meta.fabricmc.net/v2/versions/loader/" + mc);
                if (l.is_array() && !l.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
            }
            Json out = Json::array();
            if (l.is_array()) {
                int i = 0;
                for (auto& x : l) {
                    if (i++ >= 10) break;
                    if (x.contains("loader"))
                        out.push_back({ {"loader", x["loader"].value("version", "")}, {"stable", x["loader"].value("stable", false)} });
                }
            }
            reply(out, "");
        });
    });
    b.RegisterAsync("get-forge-versions", [](const Json& a, Bridge::ReplyFn reply) {
        std::string mc = ArgStr(a);
        RunBg([mc, reply] {
            std::string recBuild, latBuild;
            Json promo = HttpJson("GET", "https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json");
            if (promo.is_object() && promo.contains("promos")) {
                auto& promos = promo["promos"];
                std::string rk = mc + "-recommended", lk = mc + "-latest";
                if (promos.contains(rk) && promos[rk].is_string()) recBuild = promos[rk].get<std::string>();
                if (promos.contains(lk) && promos[lk].is_string()) latBuild = promos[lk].get<std::string>();
            }

            std::vector<std::string> builds;
            HttpResponse r = HttpSend("GET", "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml",
                                      { {"User-Agent", "JusticeLauncher/1.0"} });
            if (r.ok && r.status == 200) {
                const std::string& xml = r.body;
                const std::string prefix = mc + "-";
                const std::string legacy = "-" + mc;
                size_t pos = 0;
                while (true) {
                    size_t s = xml.find("<version>", pos);
                    if (s == std::string::npos) break;
                    s += 9;
                    size_t e = xml.find("</version>", s);
                    if (e == std::string::npos) break;
                    std::string ver = xml.substr(s, e - s);
                    pos = e + 10;
                    if (ver.rfind(prefix, 0) != 0) continue;
                    std::string build = ver.substr(prefix.size());
                    if (build.size() > legacy.size() &&
                        build.compare(build.size() - legacy.size(), legacy.size(), legacy) == 0)
                        build = build.substr(0, build.size() - legacy.size());
                    builds.push_back(build);
                }
            }

            Json out = Json::array();
            std::vector<std::string> seen;
            auto seenAlready = [&](const std::string& b) { return std::find(seen.begin(), seen.end(), b) != seen.end(); };
            auto emit = [&](const std::string& b, bool rec, bool lat) {
                out.push_back(Json{ {"version", b},
                                    {"type", rec ? "recommended" : lat ? "latest" : "normal"},
                                    {"recommended", rec}, {"latest", lat} });
                seen.push_back(b);
            };
            if (!recBuild.empty()) emit(recBuild, true, recBuild == latBuild);
            if (!latBuild.empty() && !seenAlready(latBuild)) emit(latBuild, false, true);
            for (auto& b : builds) if (!seenAlready(b)) emit(b, false, false);
            reply(out, "");
        });
    });

    b.RegisterDefault([](const Json&) -> Json { return Json(nullptr); });
}
