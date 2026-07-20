#include "bridge.h"
#include "http.h"
#include "download.h"
#include "util.h"
#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <string>

using Json = nlohmann::json;

static void RunBg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }
static std::string Field(const Json& o, const char* k) {
    return (o.is_object() && o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : "";
}
static std::wstring U(const std::string& s) {
    std::wstring w = util::Utf8ToWide(s); for (auto& c : w) if (c == L'/') c = L'\\'; return w;
}

static std::string AsStrId(const Json& o, const char* k) {
    if (!o.is_object() || !o.contains(k)) return "";
    const Json& v = o[k];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return std::to_string((long long)v.get<double>());
    return "";
}
static int LoaderType(const std::string& loader) {
    return loader == "forge" ? 1 : loader == "cauldron" ? 2 : loader == "liteloader" ? 3
         : loader == "fabric" ? 4 : loader == "quilt" ? 5 : loader == "neoforge" ? 6 : 0;
}

static const char* MR = "https://api.modrinth.com/v2";
static Json MrGet(const std::string& endpoint) {
    return HttpJson("GET", std::string(MR) + endpoint, { {"User-Agent", "JusticeLauncher/1.0"} });
}

static Json DownloadInto(Bridge* bp, const std::wstring& dir, const std::string& fileName,
                         const std::string& url, const std::string& progressChannel) {
    util::EnsureDir(dir);
    std::wstring dest = dir + L"\\" + U(fileName);
    bool ok = DownloadFile(url, dest, [bp, progressChannel](double p) {
        bp->EmitAsync(progressChannel, Json{ {"done", (long long)(p * 1000)}, {"total", 1000} });
    });
    return ok ? Json{ {"success", true} } : Json{ {"error", "download failed"} };
}

void RegisterModHandlers(Bridge& b) {
    Bridge* bp = &b;

    auto mrSearch = [](const Json& a, Bridge::ReplyFn reply, const char* projectType) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string query = o.value("query", ""), mc = o.value("mcVersion", ""),
                    cat = o.value("category", ""), loader = o.value("loader", "");
        long long offset = o.value("offset", 0);
        std::string pt = projectType;
        RunBg([=]() {
            std::string facets = "[[\"project_type:" + pt + "\"]";
            if (!mc.empty())     facets += ",[\"versions:" + mc + "\"]";
            if (!cat.empty())    facets += ",[\"categories:" + cat + "\"]";
            if (!loader.empty()) facets += ",[\"categories:" + loader + "\"]";
            facets += "]";
            std::string url = std::string(MR) + "/search?query=" + util::UrlEncode(query) +
                "&limit=20&offset=" + std::to_string(offset) + "&index=relevance&facets=" + util::UrlEncode(facets);
            reply(HttpJson("GET", url, { {"User-Agent", "JusticeLauncher/1.0"} }), "");
        });
    };
    b.RegisterAsync("mr-search",    [mrSearch](const Json& a, Bridge::ReplyFn r) { mrSearch(a, r, "mod"); });
    b.RegisterAsync("mr-rp-search", [mrSearch](const Json& a, Bridge::ReplyFn r) { mrSearch(a, r, "resourcepack"); });

    auto mrVersions = [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string pid = o.value("projectId", ""), mc = o.value("mcVersion", ""), loader = o.value("loader", "");
        RunBg([=]() {
            std::string url = "/project/" + pid + "/version?";
            if (!mc.empty())     url += "game_versions=" + util::UrlEncode("[\"" + mc + "\"]") + "&";
            if (!loader.empty()) url += "loaders=" + util::UrlEncode("[\"" + loader + "\"]");
            Json d = MrGet(url);
            reply(d.is_array() ? d : Json::array(), "");
        });
    };
    b.RegisterAsync("mr-get-versions",    mrVersions);
    b.RegisterAsync("mr-rp-get-versions", mrVersions);

    b.RegisterAsync("mr-get-project", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string pid = o.value("projectId", "");
        RunBg([=]() { Json d = MrGet("/project/" + util::UrlEncode(pid)); reply(d.is_object() ? d : Json::object(), ""); });
    });
    b.RegisterAsync("mr-get-projects", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        Json ids = o.contains("ids") ? o["ids"] : Json::array();
        RunBg([=]() {
            if (!ids.is_array() || ids.empty()) { reply(Json::array(), ""); return; }
            Json d = MrGet("/projects?ids=" + util::UrlEncode(ids.dump()));
            reply(d.is_array() ? d : Json::array(), "");
        });
    });
    b.RegisterAsync("mr-categories", [](const Json&, Bridge::ReplyFn reply) {
        RunBg([reply]() {
            Json c = MrGet("/tag/category"); Json out = Json::array();
            if (c.is_array()) for (auto& x : c) if (x.value("project_type", "") == "mod") out.push_back(x.value("name", ""));
            reply(out, "");
        });
    });
    b.RegisterAsync("mr-rp-categories", [](const Json&, Bridge::ReplyFn reply) {
        RunBg([reply]() {
            Json c = MrGet("/tag/category"); Json out = Json::array();
            if (c.is_array()) for (auto& x : c) if (x.value("project_type", "") == "resourcepack") out.push_back(x.value("name", ""));
            reply(out, "");
        });
    });
    b.RegisterAsync("mr-install-mod", [bp](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string vid = Field(o, "versionId"), fn = Field(o, "fileName"), url = Field(o, "downloadUrl");
        RunBg([=]() { reply(DownloadInto(bp, util::ModsDir(vid), fn, url, "mr-dl-progress"), ""); });
    });
    b.RegisterAsync("mr-rp-install", [bp](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string vid = Field(o, "versionId"), fn = Field(o, "fileName"), url = Field(o, "downloadUrl");
        RunBg([=]() { reply(DownloadInto(bp, util::InstanceDir(vid) + L"\\resourcepacks", fn, url, "mr-rp-dl-progress"), ""); });
    });

    static const std::string CF_PROXY = "https://justiceclient.org/api/curseforge.php";

    b.RegisterAsync("cf-search", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string query = o.value("query", ""), mc = o.value("mcVersion", ""),
                    loader = o.value("loader", "");
        int classId = 6; if (o.contains("classId")) classId = AsStrId(o, "classId").empty() ? 6 : std::atoi(AsStrId(o, "classId").c_str());
        int offset = AsStrId(o, "offset").empty() ? 0 : std::atoi(AsStrId(o, "offset").c_str());
        int sortField = AsStrId(o, "sortField").empty() ? 2 : std::atoi(AsStrId(o, "sortField").c_str());
        std::string category = AsStrId(o, "category");
        RunBg([=]() {
            std::string p = "gameId=432&classId=" + std::to_string(classId) +
                "&searchFilter=" + util::UrlEncode(query) + "&index=" + std::to_string(offset) +
                "&pageSize=20&sortField=" + std::to_string(sortField) + "&sortOrder=desc";
            if (!mc.empty()) p += "&gameVersion=" + util::UrlEncode(mc);
            int ml = LoaderType(loader); if (ml) p += "&modLoaderType=" + std::to_string(ml);
            if (!category.empty()) p += "&categoryId=" + util::UrlEncode(category);
            std::string url = CF_PROXY + "?path=" + util::UrlEncode("/v1/mods/search") + "&" + p;
            reply(HttpJson("GET", url), "");
        });
    });

    b.RegisterAsync("cf-get-files", [](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string modId = AsStrId(o, "modId"); if (modId.empty()) modId = AsStrId(o, "id");
        std::string mc = o.value("mcVersion", ""), loader = o.value("loader", "");
        RunBg([=]() {
            std::string p = "pageSize=50";
            if (!mc.empty()) p += "&gameVersion=" + util::UrlEncode(mc);
            int ml = LoaderType(loader); if (ml) p += "&modLoaderType=" + std::to_string(ml);
            std::string url = CF_PROXY + "?path=" + util::UrlEncode("/v1/mods/" + modId + "/files") + "&" + p;
            Json r = HttpJson("GET", url);
            reply(r.is_object() && r.contains("data") ? r["data"] : (r.is_array() ? r : Json::array()), "");
        });
    });

    auto cfInstall = [bp](const Json& a, Bridge::ReplyFn reply, const std::wstring& sub) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string vid = Field(o, "versionId");
        Json file = o.contains("file") && o["file"].is_object() ? o["file"] : Json::object();
        std::string fn = file.value("fileName", "");
        RunBg([=]() {
            std::string url;
            if (file.contains("downloadUrl") && file["downloadUrl"].is_string()) url = file["downloadUrl"].get<std::string>();
            std::string modId = AsStrId(file, "modId"), id = AsStrId(file, "id");
            if (url.empty() && !modId.empty() && !id.empty()) {
                Json r = HttpJson("GET", CF_PROXY + "?path=" + util::UrlEncode("/v1/mods/" + modId + "/files/" + id + "/download-url"));
                if (r.is_object() && r.contains("data") && r["data"].is_string()) url = r["data"].get<std::string>();
            }
            if (url.empty() && id.size() >= 5) {
                try {
                    std::string first = std::to_string(std::stoi(id.substr(0, 4)));
                    std::string second = std::to_string(std::stoll(id.substr(4)));
                    url = "https://edge.forgecdn.net/files/" + first + "/" + second + "/" + util::UrlEncode(fn);
                } catch (...) {}
            }
            if (url.empty() || fn.empty()) { reply(Json{ {"error", "Could not resolve download URL (author may block third-party downloads)"} }, ""); return; }
            std::wstring dir = sub.empty() ? util::ModsDir(vid) : (util::InstanceDir(vid) + L"\\" + sub);
            reply(DownloadInto(bp, dir, fn, url, "cf-dl-progress"), "");
        });
    };
    b.RegisterAsync("cf-install-mod",    [cfInstall](const Json& a, Bridge::ReplyFn r) { cfInstall(a, r, L""); });
    b.RegisterAsync("cf-install-rp",     [cfInstall](const Json& a, Bridge::ReplyFn r) { cfInstall(a, r, L"resourcepacks"); });
    b.RegisterAsync("cf-install-shader", [cfInstall](const Json& a, Bridge::ReplyFn r) { cfInstall(a, r, L"shaderpacks"); });

    const std::string CURRENT_VERSION = "2.2.7";
    b.RegisterAsync("check-for-update", [CURRENT_VERSION](const Json&, Bridge::ReplyFn reply) {
        RunBg([=]() {
            HttpResponse r = HttpSend("GET", "https://justiceclient.org/downloads/version.txt",
                                      { {"User-Agent", "JusticeLauncher/" + CURRENT_VERSION} });
            std::string latest = CURRENT_VERSION;
            if (r.ok && r.status == 200) {
                latest = r.body;
                while (!latest.empty() && (latest.back() == '\n' || latest.back() == '\r' || latest.back() == ' ')) latest.pop_back();
            }
            bool has = !latest.empty() && latest != CURRENT_VERSION;
            reply(Json{ {"current", CURRENT_VERSION}, {"latest", latest}, {"hasUpdate", has} }, "");
        });
    });

}
