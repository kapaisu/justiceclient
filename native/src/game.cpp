#include "bridge.h"
#include "http.h"
#include "download.h"
#include "zip.h"
#include "util.h"

#include <windows.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <fstream>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <algorithm>

namespace fs = std::filesystem;
using Json = nlohmann::json;

static std::wstring VersionsDir()  { return util::GameDir() + L"\\versions"; }
static std::wstring LibrariesDir() { return util::GameDir() + L"\\libraries"; }
static std::wstring AssetsDir()    { return util::GameDir() + L"\\assets"; }
static std::wstring JavaDirFor(int major) {
    return util::GameDir() + (major == 8 ? L"\\java8" : major == 25 ? L"\\java25" : L"\\java");
}
static std::wstring U(const std::string& s) {
    std::wstring w = util::Utf8ToWide(s);
    for (auto& c : w) if (c == L'/') c = L'\\';
    return w;
}
static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string Pct(double p) { return std::to_string((int)(p * 100)) + "%"; }

static int AsInt(const Json& o, const char* k, int def) {
    if (!o.is_object() || !o.contains(k)) return def;
    const Json& v = o[k];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number()) return (int)v.get<double>();
    if (v.is_string()) { try { return std::stoi(v.get<std::string>()); } catch (...) { return def; } }
    return def;
}

static Json ReadJsonFile(const std::wstring& p) {
    std::string t = util::ReadFile(p);
    if (t.empty()) return Json(nullptr);
    try { return Json::parse(t); } catch (...) { return Json(nullptr); }
}

using ProgressFn = std::function<void(const std::string&, double)>;
using LogFn = std::function<void(const std::string&)>;

static bool EvalRules(const Json& lib) {
    if (!lib.contains("rules") || !lib["rules"].is_array() || lib["rules"].empty()) return true;
    bool allowed = false;
    for (auto& r : lib["rules"]) {
        bool matches = !r.contains("os") || (r["os"].value("name", "") == "windows");
        if (matches) allowed = r.value("action", "") == "allow";
    }
    return allowed;
}
static std::string MavenToPath(const std::string& name) {
    std::string ext = "jar", coord = name;
    auto at = coord.find('@');
    if (at != std::string::npos) { ext = coord.substr(at + 1); coord = coord.substr(0, at); }
    std::vector<std::string> parts;
    size_t pos = 0, c;
    while ((c = coord.find(':', pos)) != std::string::npos) { parts.push_back(coord.substr(pos, c - pos)); pos = c + 1; }
    parts.push_back(coord.substr(pos));
    if (parts.size() < 3) return name;
    std::string g = parts[0]; for (auto& ch : g) if (ch == '.') ch = '/';
    std::string a = parts[1], v = parts[2], cls = parts.size() > 3 ? parts[3] : "";
    std::string file = cls.empty() ? a + "-" + v + "." + ext : a + "-" + v + "-" + cls + "." + ext;
    return g + "/" + a + "/" + v + "/" + file;
}

static std::string JStr(const Json& o, const char* key, const std::string& def = "") {
    if (!o.is_object()) return def;
    auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : def;
}
static std::wstring ProfilesPath() { return util::GameDir() + L"\\profiles.json"; }
static void SaveProfile(const std::string& id, const std::string& type, const std::string& mc,
                        const std::string& loader, const std::string& customName, bool vulkan) {
    Json p = ReadJsonFile(ProfilesPath()); if (!p.is_object()) p = Json::object();
    p[id] = { {"type", type}, {"mcVersion", mc},
              {"loaderVersion", loader.empty() ? Json(nullptr) : Json(loader)},
              {"customName", customName.empty() ? Json(nullptr) : Json(customName)},
              {"vulkan", vulkan} };
    util::EnsureDir(util::GameDir());
    util::WriteFile(ProfilesPath(), p.dump(2));
}
static bool IsVulkanProfile(const std::string& id) {
    Json p = ReadJsonFile(ProfilesPath());
    return p.is_object() && p.contains(id) && p[id].value("vulkan", false);
}
static std::string ProfileMc(const std::string& id) {
    Json p = ReadJsonFile(ProfilesPath());
    if (!p.is_object() || !p.contains(id) || !p[id].is_object()) return "";
    return JStr(p[id], "mcVersion");
}
static std::string UniqueProfileId(const std::string& base) {
    Json profiles = ReadJsonFile(ProfilesPath());
    auto taken = [&](const std::string& id) {
        if (util::Exists(VersionsDir() + L"\\" + U(id))) return true;
        if (profiles.is_object() && profiles.contains(id)) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int n = 2; ; n++) {
        std::string cand = base + "-" + std::to_string(n);
        if (!taken(cand)) return cand;
    }
}
static std::string SlugId(const std::string& name) {
    std::string s;
    bool prevDash = false;
    for (char c : name) {
        unsigned char uc = (unsigned char)c;
        if (isalnum(uc)) { s += (char)tolower(uc); prevDash = false; }
        else if (c == '.' || c == '_' || c == '-') { s += c; prevDash = false; }
        else if (isspace(uc)) { if (!s.empty() && !prevDash) { s += '-'; prevDash = true; } }
    }
    size_t start = s.find_first_not_of("-.");
    size_t end = s.find_last_not_of("-.");
    if (start == std::string::npos) return "instance";
    s = s.substr(start, end - start + 1);
    return s.empty() ? "instance" : s;
}

static std::wstring BundleDir() {
    std::wstring packaged = util::ExeDir() + L"\\bundled-mods\\justice";
    if (util::Exists(packaged)) return packaged;
    return U(JUSTICE_SRC_DIR) + L"\\..\\bundled-mods\\justice";
}
static std::string JusticeFileFor(const std::string& mc) {
    struct M { const char* file; std::vector<std::string> vers; };
    static const std::vector<M> mods = {
        {"justice-1.8.9.jar",          {"1.8.9"}},
        {"justice-1.21-1.21.1.jar",    {"1.21", "1.21.1"}},
        {"justice-1.21.2-1.21.4.jar",  {"1.21.2", "1.21.3", "1.21.4"}},
        {"justice-1.21.6-1.21.8.jar",  {"1.21.6", "1.21.7", "1.21.8"}},
        {"justice-1.21.9-1.21.10.jar", {"1.21.9", "1.21.10"}},
        {"justice-1.21.11.jar",        {"1.21.11"}},
        {"justice-26.1.jar",           {"26.1"}},
        {"justice-26.1.1.jar",         {"26.1.1"}},
        {"justice-26.1.2.jar",         {"26.1.2"}},
        {"justice-26.2.jar",           {"26.2"}},
    };
    for (auto& m : mods) for (auto& v : m.vers) if (v == mc) return m.file;
    return "";
}
static std::string JusticeVariant(const std::string& file, bool vulkan) {
    if (!vulkan) return file;
    if (file.rfind("justice-", 0) == 0) return "JusticeV-" + file.substr(8);
    return file;
}
static void EnsureJusticeMod(const std::string& profileId, const std::string& mc, LogFn log) {
    std::string base = JusticeFileFor(mc);
    if (base.empty()) { log("[JusticeMod] No Justice build for MC " + mc + ", skipping.\n"); return; }
    bool vulkan = IsVulkanProfile(profileId);
    std::string chosen = JusticeVariant(base, vulkan);
    std::wstring modsDir = util::ModsDir(profileId);
    util::EnsureDir(modsDir);

    std::error_code ec;
    if (fs::exists(modsDir, ec)) for (auto& e : fs::directory_iterator(modsDir, ec)) {
        std::string n = util::WideToUtf8(e.path().filename().wstring());
        std::string ln = Lower(n);
        if ((ln.rfind("justice-", 0) == 0 || ln.rfind("justicev-", 0) == 0) && n != chosen)
            fs::remove(e.path(), ec);
    }
    std::wstring src = BundleDir() + L"\\" + U(chosen);
    std::wstring dst = modsDir + L"\\" + U(chosen);
    if (!util::Exists(src)) { log("[JusticeMod] Bundled jar not found: " + chosen + "\n"); return; }
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    log("[JusticeMod] Installed " + chosen + "\n");
}

static Json InstallVanilla(const std::string& mc, ProgressFn prog, LogFn log, bool skipSave) {
    Json manifest = HttpJson("GET", "https://launchermeta.mojang.com/mc/game/version_manifest_v2.json");
    Json entry;
    if (manifest.contains("versions")) for (auto& v : manifest["versions"]) if (v.value("id", "") == mc) { entry = v; break; }
    if (entry.is_null()) throw std::runtime_error(mc + " not in manifest");
    prog("Fetching " + mc + " metadata...", 0.02);
    Json vJson = HttpJson("GET", entry.value("url", ""));
    std::wstring vDir = VersionsDir() + L"\\" + U(mc);
    util::EnsureDir(vDir);
    util::WriteFile(vDir + L"\\" + U(mc) + L".json", vJson.dump(2));

    std::wstring jar = vDir + L"\\" + U(mc) + L".jar";
    if (!util::Exists(jar)) {
        std::string url = vJson["downloads"]["client"].value("url", "");
        if (!DownloadFile(url, jar, [&](double p) { prog("Client JAR " + Pct(p), 0.02 + p * 0.18); }))
            throw std::runtime_error("client jar download failed");
    }

    std::vector<DlTask> libTasks;
    for (auto& lib : vJson.value("libraries", Json::array())) {
        if (!EvalRules(lib)) continue;
        if (!lib.contains("downloads")) continue;
        auto& d = lib["downloads"];
        if (d.contains("artifact") && d["artifact"].contains("url") && d["artifact"].contains("path"))
            libTasks.push_back({ d["artifact"]["url"], LibrariesDir() + L"\\" + U(d["artifact"]["path"].get<std::string>()) });
        if (d.contains("classifiers"))
            for (auto it = d["classifiers"].begin(); it != d["classifiers"].end(); ++it)
                if (it.key().find("windows") != std::string::npos && it.value().contains("url") && it.value().contains("path"))
                    libTasks.push_back({ it.value()["url"], LibrariesDir() + L"\\" + U(it.value()["path"].get<std::string>()) });
    }
    prog("Downloading " + std::to_string(libTasks.size()) + " libraries...", 0.22);
    DownloadBatch(libTasks, 16, [&](int dn, int t) { prog("Libraries " + std::to_string(dn) + "/" + std::to_string(t), 0.22 + (t ? (double)dn / t : 1) * 0.25); });

    auto& ai = vJson["assetIndex"];
    std::string aiId = ai.value("id", "");
    std::wstring indexPath = AssetsDir() + L"\\indexes\\" + U(aiId) + L".json";
    bool idxOk = util::Exists(indexPath);
    if (idxOk && ai.contains("sha1")) idxOk = util::Sha1HexOfFile(indexPath) == Lower(ai.value("sha1", ""));
    if (!idxOk) DownloadFile(ai.value("url", ""), indexPath, {}, ai.value("sha1", ""));
    Json idx = ReadJsonFile(indexPath);

    std::vector<DlTask> assetTasks;
    if (idx.contains("objects"))
        for (auto it = idx["objects"].begin(); it != idx["objects"].end(); ++it) {
            std::string hash = it.value().value("hash", "");
            if (hash.size() < 2) continue;
            std::string sub = hash.substr(0, 2);
            std::wstring dest = AssetsDir() + L"\\objects\\" + U(sub) + L"\\" + U(hash);
            if (!util::Exists(dest))
                assetTasks.push_back({ "https://resources.download.minecraft.net/" + sub + "/" + hash, dest });
        }
    prog("Downloading " + std::to_string(assetTasks.size()) + " assets...", 0.48);
    DownloadBatch(assetTasks, 48, [&](int dn, int t) { prog("Assets " + std::to_string(dn) + "/" + std::to_string(t), 0.48 + (t ? (double)dn / t : 1) * 0.5); });

    if (!skipSave) SaveProfile(mc, "vanilla", mc, "", "", false);
    return vJson;
}

static std::string InstallFabric(const std::string& mc, const std::string& loader,
                                 const std::string& customName, bool vulkan, ProgressFn prog, LogFn log) {
    std::wstring vanillaJson = VersionsDir() + L"\\" + U(mc) + L"\\" + U(mc) + L".json";
    if (!util::Exists(vanillaJson))
        InstallVanilla(mc, [&](const std::string& s, double p) { prog(s, p * 0.45); }, log, true);

    prog("Fetching Fabric profile...", 0.46);
    std::string profileId = customName.empty()
        ? UniqueProfileId("fabric-loader-" + loader + "-" + mc)
        : UniqueProfileId(SlugId(customName));
    Json profile;
    for (int attempt = 0; attempt < 3; attempt++) {
        profile = HttpJson("GET", "https://meta.fabricmc.net/v2/versions/loader/" + mc + "/" + loader + "/profile/json");
        if (profile.is_object() && profile.contains("libraries")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(600 * (attempt + 1)));
    }
    if (!profile.is_object() || !profile.contains("libraries")) throw std::runtime_error("Fabric profile fetch failed (network timeout — please try again)");
    profile["id"] = profileId;
    std::wstring fabricDir = VersionsDir() + L"\\" + U(profileId);
    util::EnsureDir(fabricDir);
    util::WriteFile(fabricDir + L"\\" + U(profileId) + L".json", profile.dump(2));

    std::vector<DlTask> tasks;
    for (auto& lib : profile.value("libraries", Json::array())) {
        if (!lib.contains("name")) continue;
        std::string name = lib["name"];
        std::string rel = MavenToPath(name);
        std::string base = lib.value("url", "https://repo1.maven.org/maven2/");
        if (!base.empty() && base.back() != '/') base += "/";
        tasks.push_back({ base + rel, LibrariesDir() + L"\\" + U(rel) });
    }
    prog("Downloading " + std::to_string(tasks.size()) + " Fabric libraries...", 0.47);
    DownloadBatch(tasks, 16, [&](int dn, int t) { prog("Fabric libs " + std::to_string(dn) + "/" + std::to_string(t), 0.47 + (t ? (double)dn / t : 1) * 0.4); });

    try {
        prog("Installing Fabric API...", 0.93);
        std::string url = "https://api.modrinth.com/v2/project/fabric-api/version?game_versions=" +
            util::UrlEncode("[\"" + mc + "\"]") + "&loaders=" + util::UrlEncode("[\"fabric\"]");
        Json vers = HttpJson("GET", url);
        if (vers.is_array() && !vers.empty()) {
            Json rel; for (auto& v : vers) if (v.value("version_type", "") == "release") { rel = v; break; }
            if (rel.is_null()) rel = vers[0];
            Json primary;
            for (auto& f : rel.value("files", Json::array())) if (f.value("primary", false)) { primary = f; break; }
            if (primary.is_null() && rel.contains("files") && !rel["files"].empty()) primary = rel["files"][0];
            if (!primary.is_null() && primary.contains("url")) {
                std::wstring apiDir = util::ModsDir(profileId); util::EnsureDir(apiDir);
                std::string fn = primary.value("filename", "fabric-api.jar");
                DownloadFile(primary.value("url", ""), apiDir + L"\\" + U(fn));
                log("[Fabric] API installed: " + fn + "\n");
            }
        }
    } catch (...) { log("[Fabric] API auto-install skipped\n"); }

    SaveProfile(profileId, "fabric", mc, loader, customName, vulkan);
    try { EnsureJusticeMod(profileId, mc, log); } catch (...) {}
    return profileId;
}

static std::string AdoptiumUrl(int major) {
    return "https://api.adoptium.net/v3/binary/latest/" + std::to_string(major) +
           "/ga/windows/x64/jdk/hotspot/normal/eclipse?project=jdk";
}
static std::wstring BundledJavaIn(const std::wstring& dir) {
    if (!util::Exists(dir)) return L"";
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::wstring p = e.path().wstring() + L"\\bin\\java.exe";
        if (util::Exists(p)) return p;
    }
    return L"";
}
static std::wstring FindJava() {
    std::wstring b = BundledJavaIn(JavaDirFor(21));
    if (!b.empty()) return b;
    wchar_t* jh = nullptr; size_t len = 0;
    if (_wdupenv_s(&jh, &len, L"JAVA_HOME") == 0 && jh) {
        std::wstring p = std::wstring(jh) + L"\\bin\\java.exe"; free(jh);
        if (util::Exists(p)) return p;
    }
    return L"";
}
static std::wstring InstallJava(int major, ProgressFn prog) {
    std::wstring dir = JavaDirFor(major);
    util::EnsureDir(dir);
    std::wstring zip = dir + L"\\jdk.zip";
    prog("Downloading Java " + std::to_string(major) + "...", 0);
    if (!DownloadFile(AdoptiumUrl(major), zip, [&](double p) { prog("Downloading Java " + std::to_string(major) + "... " + Pct(p), p * 0.85); }))
        throw std::runtime_error("Java download failed");
    prog("Extracting Java...", 0.85);
    if (!UnzipAll(zip, dir)) throw std::runtime_error("Java extraction failed");
    DeleteFileW(zip.c_str());
    std::wstring b = BundledJavaIn(dir);
    if (b.empty()) throw std::runtime_error("Java binary not found after extraction");
    prog("Java " + std::to_string(major) + " ready!", 1);
    return b;
}
static bool McNeedsJava8(const std::string& mc) {
    int a = 0, b = 0; sscanf_s(mc.c_str(), "%d.%d", &a, &b);
    return a == 1 && b <= 12 && b > 0;
}
static bool McNeedsJava25(const std::string& mc) {
    int a = 0, b = 0; sscanf_s(mc.c_str(), "%d.%d", &a, &b);
    return a >= 26 || (a == 1 && b >= 26);
}

static std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0; i < a.size(); i++) {
        size_t bs = 0; while (i < a.size() && a[i] == L'\\') { bs++; i++; }
        if (i == a.size()) { out.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(bs, L'\\'); out += a[i]; }
    }
    out += L'"';
    return out;
}

static DWORD SpawnStreaming(const std::wstring& exe, const std::vector<std::wstring>& args,
                            const std::wstring& cwd, std::function<void(const std::string&)> onOut,
                            std::function<void(int)> onExit) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = QuoteArg(exe);
    for (auto& a : args) { cmd += L' '; cmd += QuoteArg(a); }
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return 0; }
    CloseHandle(pi.hThread);
    DWORD pid = pi.dwProcessId;
    HANDLE proc = pi.hProcess;

    std::thread([rd, proc, onOut, onExit]() {
        std::vector<char> buf(8192);
        DWORD n = 0;
        while (ReadFile(rd, buf.data(), (DWORD)buf.size(), &n, nullptr) && n > 0)
            if (onOut) onOut(std::string(buf.data(), n));
        CloseHandle(rd);
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 0; GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        if (onExit) onExit((int)code);
    }).detach();
    return pid;
}

static int RunProcessSync(const std::wstring& exe, const std::vector<std::string>& args, LogFn log) {
    std::wstring cmd = QuoteArg(exe);
    for (auto& a : args) { cmd += L' '; cmd += QuoteArg(util::Utf8ToWide(a)); }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); return -1;
    }
    CloseHandle(wr); CloseHandle(pi.hThread);
    char b[4096]; DWORD n = 0;
    while (ReadFile(rd, b, sizeof(b), &n, nullptr) && n > 0) if (log) log(std::string(b, n));
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess);
    return (int)code;
}

static void AddForgeLibs(const Json& libs, std::vector<DlTask>& tasks) {
    for (auto& lib : libs) {
        if (lib.contains("downloads") && lib["downloads"].contains("artifact")) {
            auto& art = lib["downloads"]["artifact"];
            if (art.contains("path") && art.contains("url")) {
                std::wstring dest = LibrariesDir() + L"\\" + U(art["path"].get<std::string>());
                if (!util::Exists(dest)) tasks.push_back({ art["url"], dest });
            }
        } else if (lib.contains("name")) {
            std::string rel = MavenToPath(lib["name"]);
            std::wstring dest = LibrariesDir() + L"\\" + U(rel);
            if (!util::Exists(dest)) {
                std::string base = lib.value("url", "https://maven.minecraftforge.net/");
                if (!base.empty() && base.back() != '/') base += "/";
                tasks.push_back({ base.empty() ? "https://libraries.minecraft.net/" + rel : base + rel, dest });
            }
        }
    }
}

static std::string InstallForge(const std::string& mc, const std::string& forgeVersion,
                                const std::string& customName, ProgressFn prog, LogFn log) {
    std::wstring vanillaJson = VersionsDir() + L"\\" + U(mc) + L"\\" + U(mc) + L".json";
    if (!util::Exists(vanillaJson)) InstallVanilla(mc, [&](const std::string& s, double p) { prog(s, p * 0.2); }, log, true);

    std::string profileId = UniqueProfileId("forge-" + mc + "-" + forgeVersion);
    prog("Downloading Forge installer...", 0.22);
    std::vector<std::string> cands = { mc + "-" + forgeVersion, mc + "-" + forgeVersion + "-" + mc };
    { int dots = 0; for (char c : mc) if (c == '.') dots++; if (dots == 1) cands.push_back(mc + "-" + forgeVersion + "-" + mc + ".0"); }

    std::string artifactVer; std::wstring installerDest;
    for (auto& cand : cands) {
        std::string url = "https://maven.minecraftforge.net/net/minecraftforge/forge/" + cand + "/forge-" + cand + "-installer.jar";
        std::wstring dest = LibrariesDir() + L"\\net\\minecraftforge\\forge\\" + U(cand) + L"\\forge-" + U(cand) + L"-installer.jar";
        if (util::Exists(dest) || DownloadFile(url, dest)) { artifactVer = cand; installerDest = dest; break; }
    }
    if (artifactVer.empty()) throw std::runtime_error("Could not find Forge installer for " + mc + "-" + forgeVersion);

    prog("Extracting Forge installer...", 0.30);
    std::string ipStr = ZipReadEntry(installerDest, "install_profile.json");
    if (ipStr.empty()) throw std::runtime_error("Invalid Forge installer: missing install_profile.json");
    Json installProfile = Json::parse(ipStr);
    std::string vjStr = ZipReadEntry(installerDest, "version.json");
    bool isModern = !vjStr.empty();
    std::wstring forgeDir = VersionsDir() + L"\\" + U(profileId); util::EnsureDir(forgeDir);
    std::wstring installerDir = fs::path(installerDest).parent_path().wstring();

    if (!isModern) {
        Json legacy = installProfile.value("versionInfo", Json(nullptr));
        if (legacy.is_null()) throw std::runtime_error("Invalid legacy Forge installer");
        legacy["id"] = profileId;
        if (!legacy.contains("inheritsFrom")) legacy["inheritsFrom"] = mc;

        Json inst = installProfile.value("install", Json::object());
        std::string filePath = inst.value("filePath", "");
        std::string libCoord = inst.value("path", "");
        if (!filePath.empty() && !libCoord.empty()) {
            std::wstring dst = LibrariesDir() + L"\\" + U(MavenToPath(libCoord));
            util::EnsureDir(fs::path(dst).parent_path().wstring());
            std::string bytes = ZipReadEntry(installerDest, filePath);
            if (!bytes.empty()) { util::WriteFile(dst, bytes); log("[Forge] Extracted " + filePath + "\n"); }
            else log("[Forge] Missing " + filePath + " inside installer\n");
        }

        if (legacy.contains("libraries") && legacy["libraries"].is_array()) {
            for (auto& lib : legacy["libraries"]) {
                if (lib.contains("url") && lib["url"].is_string()) {
                    std::string u = lib["url"].get<std::string>();
                    if (u.find("files.minecraftforge.net/maven") != std::string::npos)
                        lib["url"] = "https://maven.minecraftforge.net/";
                }
            }
        }

        util::WriteFile(forgeDir + L"\\" + U(profileId) + L".json", legacy.dump(2));
        std::vector<DlTask> tasks; AddForgeLibs(legacy.value("libraries", Json::array()), tasks);
        prog("Downloading Forge libraries...", 0.5);
        DownloadBatch(tasks, 48, [&](int d, int t) { prog("Forge libs " + std::to_string(d) + "/" + std::to_string(t), 0.5 + (t ? (double)d / t : 1) * 0.48); });
        SaveProfile(profileId, "forge", mc, forgeVersion, customName, false);
        try { EnsureJusticeMod(profileId, mc, log); } catch (...) {}
        return profileId;
    }

    Json versionJson = Json::parse(vjStr);
    versionJson["id"] = profileId;
    if (!versionJson.contains("inheritsFrom")) versionJson["inheritsFrom"] = mc;
    util::WriteFile(forgeDir + L"\\" + U(profileId) + L".json", versionJson.dump(2));

    prog("Extracting Forge libraries...", 0.32);
    ZipExtractByPrefix(installerDest, "maven/", LibrariesDir());
    ZipExtractByPrefix(installerDest, "data/", installerDir);

    std::vector<DlTask> tasks;
    AddForgeLibs(versionJson.value("libraries", Json::array()), tasks);
    AddForgeLibs(installProfile.value("libraries", Json::array()), tasks);
    prog("Downloading Forge libraries...", 0.36);
    DownloadBatch(tasks, 48, [&](int d, int t) { prog("Forge libs " + std::to_string(d) + "/" + std::to_string(t), 0.36 + (t ? (double)d / t : 1) * 0.30); });

    if (installProfile.contains("processors") && installProfile["processors"].is_array() && !installProfile["processors"].empty()) {
        prog("Running Forge processors...", 0.68);
        std::wstring javaPath = FindJava();
        if (javaPath.empty()) javaPath = InstallJava(21, [&](const std::string& s, double p) { prog(s, p); });
        std::wstring clientJar = VersionsDir() + L"\\" + U(mc) + L"\\" + U(mc) + L".jar";

        auto resolveVar = [&](const std::string& v) -> std::string {
            if (v.size() >= 2 && v.front() == '[' && v.back() == ']') return util::WideToUtf8(LibrariesDir()) + "\\" + MavenToPath(v.substr(1, v.size() - 2));
            if (v.rfind("/data/", 0) == 0) return util::WideToUtf8(installerDir) + "\\" + v.substr(6);
            return v;
        };
        std::map<std::string, std::string> dataVars;
        Json profileData = installProfile.value("data", Json::object());
        for (auto it = profileData.begin(); it != profileData.end(); ++it) {
            std::string v = it.value().is_string() ? it.value().get<std::string>() : it.value().value("client", "");
            dataVars[it.key()] = resolveVar(v);
        }
        dataVars["SIDE"] = "client"; dataVars["MINECRAFT_JAR"] = util::WideToUtf8(clientJar);
        dataVars["INSTALLER"] = util::WideToUtf8(installerDest); dataVars["ROOT"] = util::WideToUtf8(util::GameDir());
        dataVars["LIBRARY_DIR"] = util::WideToUtf8(LibrariesDir());
        auto resolveArg = [&](const std::string& arg) -> std::string {
            if (arg.size() >= 2 && arg.front() == '[' && arg.back() == ']') return util::WideToUtf8(LibrariesDir()) + "\\" + MavenToPath(arg.substr(1, arg.size() - 2));
            if (arg.size() >= 2 && arg.front() == '{' && arg.back() == '}') { auto f = dataVars.find(arg.substr(1, arg.size() - 2)); return f != dataVars.end() ? f->second : arg; }
            if (arg == "/data/client.lzma") return util::WideToUtf8(installerDir) + "\\client.lzma";
            return arg;
        };

        auto& procs = installProfile["processors"];
        int idx = 0;
        for (auto& proc : procs) {
            idx++;
            if (proc.contains("sides") && proc["sides"].is_array()) {
                bool client = false; for (auto& s : proc["sides"]) if (s.get<std::string>() == "client") client = true;
                if (!client) continue;
            }
            prog("Processor " + std::to_string(idx) + "/" + std::to_string(procs.size()), 0.68 + (double)idx / procs.size() * 0.28);
            if (proc.contains("outputs")) {
                bool allExist = true;
                for (auto it = proc["outputs"].begin(); it != proc["outputs"].end(); ++it)
                    if (!util::Exists(U(resolveArg(it.key())))) { allExist = false; break; }
                if (allExist) { log("Processor " + std::to_string(idx) + " skipped (outputs exist)\n"); continue; }
            }
            std::wstring procJarPath = LibrariesDir() + L"\\" + U(MavenToPath(proc.value("jar", "")));
            std::string manifest = ZipReadEntry(procJarPath, "META-INF/MANIFEST.MF");
            std::string mainClass;
            { size_t p = manifest.find("Main-Class:"); if (p != std::string::npos) { size_t s = p + 11; while (s < manifest.size() && manifest[s] == ' ') s++; size_t e = s; while (e < manifest.size() && manifest[e] != '\r' && manifest[e] != '\n') e++; mainClass = manifest.substr(s, e - s); } }
            if (mainClass.empty()) { log("Processor " + std::to_string(idx) + ": no main class\n"); continue; }
            std::string cp = util::WideToUtf8(procJarPath);
            for (auto& ce : proc.value("classpath", Json::array())) cp += ";" + util::WideToUtf8(LibrariesDir() + L"\\" + U(MavenToPath(ce.get<std::string>())));
            std::vector<std::string> args = { "-Xmx1G", "-Xms512M", "-cp", cp, mainClass };
            for (auto& a : proc.value("args", Json::array())) args.push_back(resolveArg(a.get<std::string>()));
            log("Running processor " + std::to_string(idx) + ": " + mainClass + "\n");
            int code = RunProcessSync(javaPath, args, log);
            if (code != 0) throw std::runtime_error("Forge processor " + std::to_string(idx) + " failed (exit " + std::to_string(code) + ")");
        }
    }
    prog("Forge installed!", 1);
    SaveProfile(profileId, "forge", mc, forgeVersion, customName, false);
    try { EnsureJusticeMod(profileId, mc, log); } catch (...) {}
    return profileId;
}

static std::vector<std::string> BuildClasspath(const Json& vJson) {
    std::vector<std::string> cp;
    for (auto& lib : vJson.value("libraries", Json::array())) {
        if (!EvalRules(lib)) continue;
        if (lib.value("name", "").find(":natives-") != std::string::npos) continue;
        bool hasArtifact = lib.contains("downloads") && lib["downloads"].contains("artifact");
        if (lib.contains("natives") && !hasArtifact) continue;
        std::wstring p;
        std::string mavenPath, sha1;
        std::vector<std::string> urls;
        if (hasArtifact && lib["downloads"]["artifact"].contains("path")) {
            mavenPath = lib["downloads"]["artifact"]["path"].get<std::string>();
            p = LibrariesDir() + L"\\" + U(mavenPath);
            sha1 = lib["downloads"]["artifact"].value("sha1", "");
            std::string u = lib["downloads"]["artifact"].value("url", "");
            if (!u.empty()) urls.push_back(u);
        } else if (lib.contains("name") && !lib.contains("downloads")) {
            mavenPath = MavenToPath(lib["name"].get<std::string>());
            p = LibrariesDir() + L"\\" + U(mavenPath);
            if (lib.contains("url") && lib["url"].is_string()) {
                std::string b = lib["url"].get<std::string>();
                if (!b.empty() && b.back() != '/') b += '/';
                if (!b.empty()) urls.push_back(b + mavenPath);
            }
        }
        if (p.empty()) continue;
        if (!mavenPath.empty())
            for (const char* repo : { "https://libraries.minecraft.net/",
                                      "https://maven.minecraftforge.net/",
                                      "https://repo1.maven.org/maven2/" })
                urls.push_back(std::string(repo) + mavenPath);

        bool valid = util::Exists(p) && util::FileSize(p) > 0 && ZipIsValid(p);
        for (auto& url : urls) {
            if (valid) break;
            for (int a = 0; a < 2 && !valid; a++) {
                std::error_code de; fs::remove(p, de);
                DownloadFile(url, p, {}, sha1);
                valid = util::Exists(p) && util::FileSize(p) > 0 && ZipIsValid(p);
            }
        }
        if (valid) cp.push_back(util::WideToUtf8(p));
    }
    return cp;
}
static void ExtractAllNatives(const Json& vJson, const std::wstring& nativesDir, LogFn log) {
    int total = 0;
    auto grab = [&](const std::string& path, const std::string& url, const std::string& sha1) {
        std::wstring full = LibrariesDir() + L"\\" + U(path);
        if (!util::Exists(full) || util::FileSize(full) == 0 || !ZipIsValid(full)) {
            if (!url.empty()) { std::error_code de; fs::remove(full, de); DownloadFile(url, full, {}, sha1); }
        }
        if (util::Exists(full)) total += ExtractNativeLibs(full, nativesDir);
    };
    for (auto& lib : vJson.value("libraries", Json::array())) {
        if (!EvalRules(lib)) continue;
        if (!lib.contains("downloads")) continue;
        auto& d = lib["downloads"];
        if (d.contains("artifact") && d["artifact"].contains("path")) {
            std::string path = d["artifact"]["path"];
            if (path.find("natives-windows") != std::string::npos &&
                path.find("windows-arm64") == std::string::npos &&
                path.find("windows-x86") == std::string::npos)
                grab(path, d["artifact"].value("url", ""), d["artifact"].value("sha1", ""));
        }
        if (d.contains("classifiers"))
            for (auto it = d["classifiers"].begin(); it != d["classifiers"].end(); ++it)
                if (it.key().find("windows") != std::string::npos &&
                    it.key().find("arm64") == std::string::npos && it.key().find("x86") == std::string::npos &&
                    it.value().contains("path"))
                    grab(it.value()["path"], it.value().value("url", ""), it.value().value("sha1", ""));
    }
    log("Natives: extracted " + std::to_string(total) + " files\n");
}
static std::string Subst(std::string s, const std::map<std::string, std::string>& vars) {
    for (auto& kv : vars) {
        std::string key = "${" + kv.first + "}";
        size_t pos = 0;
        while ((pos = s.find(key, pos)) != std::string::npos) { s.replace(pos, key.size(), kv.second); pos += kv.second.size(); }
    }
    return s;
}
static std::string OfflineUuid(const std::string& name) {
    std::string h = util::Md5Raw("OfflinePlayer:" + name);
    if (h.size() < 16) return "00000000-0000-0000-0000-000000000000";
    unsigned char* b = (unsigned char*)h.data();
    b[6] = (b[6] & 0x0f) | 0x30; b[8] = (b[8] & 0x3f) | 0x80;
    std::string hex = util::Sha1Hex(""); hex.clear();
    static const char* d = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { hex += d[b[i] >> 4]; hex += d[b[i] & 0xf]; }
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}
static std::string DashUuid(std::string u) {
    std::string s; for (char c : u) if (c != '-') s += c;
    if (s.size() != 32) return u;
    return s.substr(0, 8) + "-" + s.substr(8, 4) + "-" + s.substr(12, 4) + "-" + s.substr(16, 4) + "-" + s.substr(20, 12);
}

static std::string MakeReportId(const std::string& user, int code) {
    std::string seed = user + "|" + std::to_string((unsigned long long)GetTickCount64()) + "|" + std::to_string(code);
    std::string h = util::Sha1Hex(seed);
    std::string id = "JC-";
    for (size_t i = 0; i < 8 && i < h.size(); i++) id += (char)toupper((unsigned char)h[i]);
    return id;
}

using EmitFn = std::function<void(const std::string&, const nlohmann::json&)>;
static void LaunchGame(const Json& p, EmitFn emit) {
    auto log = [emit](const std::string& m) { emit("game-log", Json::array({ m })); };
    auto lp = [emit](const std::string& s, double pr) { emit("launch-progress", Json{ {"step", s}, {"progress", pr} }); };
    auto status = [emit](const std::string& s) { emit("game-status", Json{ {"status", s} }); };
    auto done = [emit](bool ok, const std::string& err) { emit("launch-done", ok ? Json{ {"ok", true} } : Json{ {"ok", false}, {"error", err} }); };

    std::string versionId = p.value("versionId", "");
    try {
        std::wstring versionDir = VersionsDir() + L"\\" + U(versionId);
        std::wstring jsonPath = versionDir + L"\\" + U(versionId) + L".json";
        if (!util::Exists(jsonPath)) { done(false, "Not installed: " + versionId); return; }
        status("launching");
        lp("Authenticating", 0.05);

        bool offline = p.value("offlineMode", false);
        Json auth = offline ? Json(nullptr) : AuthGetValidAuth();
        std::string username = p.value("username", "Player");
        std::string uuid = "00000000-0000-0000-0000-000000000000", token = "offline", userType = "legacy";
        if (!auth.is_null()) {
            username = auth.value("username", username);
            uuid = DashUuid(auth.value("uuid", uuid));
            token = auth.value("mcAccessToken", "");
            userType = "msa";
            log("Authenticated as " + username + "\n");
        } else {
            std::string clean; for (char c : username) if (isalnum((unsigned char)c) || c == '_') clean += c;
            if (clean.empty()) clean = "Player"; if (clean.size() > 16) clean = clean.substr(0, 16);
            username = clean; uuid = OfflineUuid(clean);
            log("Offline mode: " + username + "\n");
        }

        Json vJson = ReadJsonFile(jsonPath);
        Json parentJson(nullptr);
        std::string inherits = vJson.value("inheritsFrom", "");
        if (!inherits.empty()) {
            std::wstring pp = VersionsDir() + L"\\" + U(inherits) + L"\\" + U(inherits) + L".json";
            if (!util::Exists(pp)) {
                log("Parent " + inherits + " missing — downloading...\n");
                InstallVanilla(inherits, [&](const std::string& s, double pr) { emit("install-progress", Json{ {"step", s}, {"progress", pr} }); }, log, true);
            }
            parentJson = ReadJsonFile(pp);
        }

        std::string mainClass = vJson.value("mainClass", parentJson.is_null() ? "" : parentJson.value("mainClass", ""));
        bool isFabric = mainClass.find("fabric") != std::string::npos || mainClass.find("quilt") != std::string::npos;
        bool isForge = mainClass.find("forge") != std::string::npos || mainClass.find("fml") != std::string::npos ||
                       mainClass.find("cpw.mods") != std::string::npos || mainClass.find("neoforge") != std::string::npos ||
                       vJson.contains("minecraftArguments");
        bool isLegacyForge = isForge && vJson.contains("minecraftArguments") && !vJson.contains("arguments");

        if (isFabric || isForge) {
            std::string jmc = ProfileMc(versionId);
            if (jmc.empty()) jmc = inherits;
            if (jmc.empty()) jmc = versionId;
            lp("Updating Justice mod", 0.3);
            try { EnsureJusticeMod(versionId, jmc, log); } catch (...) {}
        }

        std::wstring baseDir = parentJson.is_null() ? versionDir : (VersionsDir() + L"\\" + U(inherits));
        std::wstring nativesDir = baseDir + L"\\natives";
        util::EnsureDir(nativesDir);
        lp("Extracting natives", 0.35);
        ExtractAllNatives(parentJson.is_null() ? vJson : parentJson, nativesDir, log);

        lp("Building classpath", 0.5);
        std::vector<std::string> cp = BuildClasspath(vJson);
        if (!parentJson.is_null()) { auto pc = BuildClasspath(parentJson); cp.insert(cp.end(), pc.begin(), pc.end()); }
        std::wstring clientJar = parentJson.is_null() ? (versionDir + L"\\" + U(versionId) + L".jar")
                                                      : (VersionsDir() + L"\\" + U(inherits) + L"\\" + U(inherits) + L".jar");
        if (!util::Exists(clientJar)) { done(false, "Client JAR missing"); status("closed"); return; }
        if (!isForge || isLegacyForge) cp.push_back(util::WideToUtf8(clientJar));

        std::vector<std::string> finalCp; std::vector<std::string> seen;
        for (auto& e : cp) {
            std::string norm = e; for (auto& c : norm) if (c == '\\') c = '/';
            size_t s1 = norm.find_last_of('/'); std::string noFile = s1 == std::string::npos ? norm : norm.substr(0, s1);
            size_t s2 = noFile.find_last_of('/'); std::string key = s2 == std::string::npos ? noFile : noFile.substr(0, s2);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key); finalCp.push_back(e);
        }

        std::string resolvedMc = inherits.empty() ? versionId : inherits;
        lp("Locating Java", 0.6);
        std::wstring javaPath;
        int major = McNeedsJava25(resolvedMc) ? 25 : McNeedsJava8(resolvedMc) ? 8 : 21;
        javaPath = major == 21 ? FindJava() : BundledJavaIn(JavaDirFor(major));
        if (javaPath.empty()) {
            status("installing-java");
            log("Java " + std::to_string(major) + " not found — downloading...\n");
            javaPath = InstallJava(major, [&](const std::string& s, double pr) { emit("java-install-progress", Json{ {"step", s}, {"progress", pr} }); });
        }
        log("Java: " + util::WideToUtf8(javaPath) + "\n");

        std::wstring instDir = util::InstanceDir(versionId);
        util::EnsureDir(util::ModsDir(versionId));
        std::string aiId = (parentJson.is_null() ? vJson : parentJson).value("assetIndex", Json::object()).value("id", resolvedMc);

        std::map<std::string, std::string> vars = {
            {"natives_directory", util::WideToUtf8(nativesDir)}, {"launcher_name", "justice-launcher"},
            {"launcher_version", "1.0"}, {"auth_player_name", username}, {"auth_uuid", uuid},
            {"auth_access_token", token}, {"user_type", userType}, {"version_name", versionId},
            {"game_directory", util::WideToUtf8(instDir)}, {"assets_root", util::WideToUtf8(AssetsDir())},
            {"assets_index_name", aiId}, {"version_type", "release"}, {"library_directory", util::WideToUtf8(LibrariesDir())},
            {"classpath_separator", ";"}, {"user_properties", "{}"},
        };

        std::vector<std::string> jvm = {
            "-Xmx" + std::to_string(AsInt(p, "ram", 2048)) + "M", "-Xms512M",
            "-Djava.library.path=" + util::WideToUtf8(nativesDir),
            "-Dorg.lwjgl.librarypath=" + util::WideToUtf8(nativesDir),
        };
        if (isFabric) { jvm.push_back("-Dfabric.gameJarPath=" + util::WideToUtf8(clientJar)); jvm.push_back("-Dfabric.development=false"); }
        if (isForge && vJson.contains("arguments") && vJson["arguments"].contains("jvm"))
            for (auto& a : vJson["arguments"]["jvm"]) if (a.is_string()) jvm.push_back(Subst(a.get<std::string>(), vars));
        if (offline) {
            std::wstring skinPath = util::GameDir() + L"\\offline-skin.png";
            if (util::Exists(skinPath)) jvm.push_back("-Djustice.offline.skin=" + util::WideToUtf8(skinPath));
        }

        std::vector<std::string> game = {
            "--username", username, "--version", versionId, "--gameDir", util::WideToUtf8(instDir),
            "--assetsDir", util::WideToUtf8(AssetsDir()), "--assetIndex", aiId, "--uuid", uuid,
            "--accessToken", token, "--userType", userType, "--versionType", "release",
        };
        if (isFabric) { game.push_back("--launchTarget"); game.push_back("client"); }
        if (isForge && !isLegacyForge && vJson.contains("arguments") && vJson["arguments"].contains("game"))
            for (auto& a : vJson["arguments"]["game"]) if (a.is_string()) {
                std::string r = Subst(a.get<std::string>(), vars);
                if (std::find(game.begin(), game.end(), r) == game.end()) game.push_back(r);
            }
        if (isLegacyForge) {
            game.clear();
            std::stringstream ss(vJson.value("minecraftArguments", "")); std::string t;
            while (ss >> t) game.push_back(Subst(t, vars));
        }
        std::string serverAddr = p.value("serverAddress", "");
        if (!serverAddr.empty()) { game.push_back("--server"); game.push_back(serverAddr); game.push_back("--port"); game.push_back(std::to_string(AsInt(p, "serverPort", 25565))); }

        std::string extraFlags = p.value("jvmFlags", "");
        std::vector<std::string> args = jvm;
        if (!extraFlags.empty()) { std::stringstream ss(extraFlags); std::string t; while (ss >> t) args.push_back(t); }
        args.push_back("-cp");
        std::string cpStr; for (size_t i = 0; i < finalCp.size(); i++) { if (i) cpStr += ";"; cpStr += finalCp[i]; }
        args.push_back(cpStr);
        args.push_back(mainClass);
        for (auto& g : game) args.push_back(g);

        lp("Spawning Java", 0.8);
        log("Main class: " + mainClass + "\nClasspath: " + std::to_string(finalCp.size()) + " JARs\n");
        std::vector<std::wstring> wargs; for (auto& a : args) wargs.push_back(util::Utf8ToWide(a));

        auto readyFired = std::make_shared<std::atomic<bool>>(false);
        auto fireReady = [done, readyFired]() { if (!readyFired->exchange(true)) done(true, ""); };
        auto crashBuf = std::make_shared<std::string>();
        auto crashMtx = std::make_shared<std::mutex>();
        std::string crashUser = username, crashVer = versionId;

        DWORD pid = SpawnStreaming(javaPath, wargs, instDir,
            [log, fireReady, crashBuf, crashMtx](const std::string& chunk) {
                log(chunk);
                { std::lock_guard<std::mutex> lk(*crashMtx); crashBuf->append(chunk); if (crashBuf->size() > 150000) crashBuf->erase(0, crashBuf->size() - 150000); }
                std::string l = Lower(chunk);
                if (l.find("openal initialized") != std::string::npos || l.find("lwjgl") != std::string::npos ||
                    l.find("reloading resourcemanager") != std::string::npos) fireReady();
            },
            [emit, status, fireReady, crashBuf, crashMtx, crashUser, crashVer](int code) {
                emit("game-log", Json::array({ "\nExit code: " + std::to_string(code) + "\n" }));
                status("closed"); emit("overlay-game-stop", Json::array());
                if (code != 0) {
                    std::string id = MakeReportId(crashUser, code);
                    emit("game-crashed", Json{ {"crashId", id}, {"exitCode", code}, {"version", crashVer} });
                }
            });
        if (!pid) { done(false, "Failed to start Java process"); status("closed"); return; }
        lp("Loading Minecraft", 0.9);
        emit("game-launched-overlay", Json::array());

        std::thread([fireReady]() { std::this_thread::sleep_for(std::chrono::seconds(12)); fireReady(); }).detach();
    } catch (const std::exception& e) {
        log(std::string("\nLaunch error: ") + e.what() + "\n");
        status("closed"); done(false, e.what());
    }
}

static void RunBg(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

static const std::string CFMP_PROXY = "https://justiceclient.org/api/curseforge.php";

static std::string JNumStr(const Json& o, const char* k) {
    if (!o.is_object() || !o.contains(k)) return "";
    const Json& v = o[k];
    if (v.is_number_integer())  return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float())    return std::to_string((long long)v.get<double>());
    if (v.is_string())          return v.get<std::string>();
    return "";
}

static std::string CfPackFileUrl(const std::string& modId, const std::string& fileId, const std::string& fileName, const Json& fileObj) {
    if (fileObj.is_object()) {
        auto it = fileObj.find("downloadUrl");
        if (it != fileObj.end() && it->is_string() && !it->get<std::string>().empty()) return it->get<std::string>();
    }
    try {
        Json r = HttpJson("GET", CFMP_PROXY + "?path=" + util::UrlEncode("/v1/mods/" + modId + "/files/" + fileId + "/download-url"));
        if (r.is_object() && r.contains("data") && r["data"].is_string() && !r["data"].get<std::string>().empty())
            return r["data"].get<std::string>();
    } catch (...) {}
    if (fileId.size() >= 4 && !fileName.empty()) {
        std::string a = fileId.substr(0, fileId.size() - 3);
        std::string b = fileId.substr(fileId.size() - 3);
        size_t nz = b.find_first_not_of('0'); b = (nz == std::string::npos) ? std::string("0") : b.substr(nz);
        return "https://edge.forgecdn.net/files/" + a + "/" + b + "/" + util::UrlEncode(fileName);
    }
    return "";
}

static void RemoveAutoFabricApi(const std::string& profileId) {
    std::error_code ec;
    std::wstring md = util::ModsDir(profileId);
    for (auto it = fs::directory_iterator(md, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string fn = it->path().filename().string();
        std::string low = fn; for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("fabric-api") != std::string::npos && low.size() >= 4 && low.substr(low.size() - 4) == ".jar")
            { std::error_code e2; fs::remove(it->path(), e2); }
    }
}

static Json InstallModrinthPack(const Json& p, ProgressFn prog, LogFn log) {
    std::string url = p.value("url", "");
    std::string fileName = p.value("fileName", std::string("pack.mrpack"));
    std::string packName = p.value("packName", "");
    if (url.empty()) throw std::runtime_error("No modpack URL provided");
    std::wstring tmp = util::GameDir() + L"\\tmp-modpack"; util::EnsureDir(tmp);
    std::wstring zipPath = tmp + L"\\" + U(fileName);
    prog("Downloading modpack...", 0.02);
    if (!DownloadFile(url, zipPath, [&](double pr) { prog("Downloading modpack...", 0.02 + pr * 0.12); }))
        throw std::runtime_error("Failed to download the modpack file");
    prog("Reading modpack...", 0.14);
    std::string idxStr = ZipReadEntry(zipPath, "modrinth.index.json");
    if (idxStr.empty()) throw std::runtime_error("Not a valid .mrpack (missing modrinth.index.json)");
    Json index = Json::parse(idxStr, nullptr, false);
    if (!index.is_object()) throw std::runtime_error("Invalid modpack manifest");
    Json deps = index.value("dependencies", Json::object());
    std::string mc = deps.value("minecraft", "");
    std::string fabric = deps.contains("fabric-loader") ? deps.value("fabric-loader", "") : deps.value("quilt-loader", "");
    std::string forge = deps.contains("neoforge") ? deps.value("neoforge", "") : deps.value("forge", "");
    std::string name = packName.empty() ? index.value("name", std::string("Modpack")) : packName;
    if (mc.empty()) throw std::runtime_error("This modpack is missing its Minecraft version");
    std::string profileId;
    if (!fabric.empty()) { profileId = InstallFabric(mc, fabric, name, false, [&](const std::string& s, double pr) { prog(s, 0.15 + pr * 0.4); }, log); RemoveAutoFabricApi(profileId); }
    else if (!forge.empty()) profileId = InstallForge(mc, forge, name, [&](const std::string& s, double pr) { prog(s, 0.15 + pr * 0.4); }, log);
    else throw std::runtime_error("This modpack needs Fabric or Forge, which couldn't be detected.");
    std::wstring instDir = util::InstanceDir(profileId);
    Json files = index.value("files", Json::array());
    int total = 0; for (auto& f : files) if (f.contains("downloads") && f["downloads"].is_array() && !f["downloads"].empty()) total++;
    int n = 0;
    for (auto& f : files) {
        if (!f.contains("downloads") || !f["downloads"].is_array() || f["downloads"].empty()) continue;
        std::string rel = f.value("path", "");
        if (rel.rfind("overrides/", 0) == 0) rel = rel.substr(10);
        if (rel.empty()) continue;
        std::string dl = f["downloads"][0].get<std::string>();
        std::wstring dest = instDir + L"\\" + U(rel);
        util::EnsureDir(fs::path(dest).parent_path().wstring());
        n++;
        prog("Downloading mods (" + std::to_string(n) + "/" + std::to_string(total) + ")...", 0.58 + (total ? (double)n / total : 1) * 0.34);
        DownloadFile(dl, dest);
    }
    prog("Applying overrides...", 0.95);
    ZipExtractByPrefix(zipPath, "overrides/", instDir);
    ZipExtractByPrefix(zipPath, "client-overrides/", instDir);
    try { EnsureJusticeMod(profileId, mc, log); } catch (...) {}
    std::error_code ec; fs::remove_all(tmp, ec);
    return Json{ {"success", true}, {"profileId", profileId}, {"versionId", profileId}, {"name", name} };
}

static Json InstallCfPack(const Json& p, ProgressFn prog, LogFn log) {
    Json fileObj = p.value("file", Json::object());
    std::string packName = p.value("packName", "");
    std::string fileName = fileObj.value("fileName", std::string("modpack.zip"));
    std::string modId = JNumStr(fileObj, "modId");
    std::string fileId = JNumStr(fileObj, "id");
    prog("Downloading modpack...", 0.02);
    std::string url = CfPackFileUrl(modId, fileId, fileName, fileObj);
    if (url.empty()) throw std::runtime_error("Could not resolve the modpack download URL");
    std::wstring tmp = util::GameDir() + L"\\tmp-modpack"; util::EnsureDir(tmp);
    std::wstring zipPath = tmp + L"\\" + U(fileName);
    if (!DownloadFile(url, zipPath, [&](double pr) { prog("Downloading modpack...", 0.02 + pr * 0.12); }))
        throw std::runtime_error("Failed to download the modpack file");
    prog("Reading modpack manifest...", 0.14);
    std::string manStr = ZipReadEntry(zipPath, "manifest.json");
    if (manStr.empty()) throw std::runtime_error("Not a valid CurseForge modpack (missing manifest.json)");
    Json man = Json::parse(manStr, nullptr, false);
    if (!man.is_object()) throw std::runtime_error("Invalid modpack manifest");
    Json mcObj = man.value("minecraft", Json::object());
    std::string mc = mcObj.value("version", "");
    Json loaders = mcObj.value("modLoaders", Json::array());
    std::string primary;
    for (auto& l : loaders) if (l.value("primary", false)) { primary = l.value("id", ""); break; }
    if (primary.empty() && !loaders.empty()) primary = loaders[0].value("id", "");
    std::string fabric, forge;
    if (primary.rfind("fabric-", 0) == 0) fabric = primary.substr(7);
    else if (primary.rfind("neoforge-", 0) == 0) forge = primary.substr(9);
    else if (primary.rfind("forge-", 0) == 0) forge = primary.substr(6);
    std::string name = packName.empty() ? man.value("name", std::string("Modpack")) : packName;
    if (mc.empty()) throw std::runtime_error("This modpack is missing its Minecraft version");
    std::string profileId;
    if (!fabric.empty()) { profileId = InstallFabric(mc, fabric, name, false, [&](const std::string& s, double pr) { prog(s, 0.15 + pr * 0.3); }, log); RemoveAutoFabricApi(profileId); }
    else if (!forge.empty()) profileId = InstallForge(mc, forge, name, [&](const std::string& s, double pr) { prog(s, 0.15 + pr * 0.3); }, log);
    else throw std::runtime_error("This modpack needs Fabric or Forge, which couldn't be detected.");
    std::wstring instDir = util::InstanceDir(profileId);
    std::wstring modsDir = util::ModsDir(profileId); util::EnsureDir(modsDir);
    prog("Applying overrides...", 0.5);
    std::string overrideName = man.value("overrides", std::string("overrides"));
    ZipExtractByPrefix(zipPath, overrideName + "/", instDir);
    Json files = man.value("files", Json::array());
    int total = (int)files.size(), n = 0;
    for (auto& entry : files) {
        n++;
        std::string pid = JNumStr(entry, "projectID");
        std::string fid = JNumStr(entry, "fileID");
        prog("Downloading mods (" + std::to_string(n) + "/" + std::to_string(total) + ")...", 0.55 + (total ? (double)n / total : 1) * 0.4);
        try {
            Json info = HttpJson("GET", CFMP_PROXY + "?path=" + util::UrlEncode("/v1/mods/" + pid + "/files/" + fid));
            Json fobj = (info.is_object() && info.contains("data")) ? info["data"] : Json::object();
            std::string fn = fobj.value("fileName", "");
            if (fn.empty()) continue;
            std::string dl = CfPackFileUrl(pid, fid, fn, fobj);
            if (!dl.empty()) DownloadFile(dl, modsDir + L"\\" + U(fn));
        } catch (...) {}
    }
    try { EnsureJusticeMod(profileId, mc, log); } catch (...) {}
    std::error_code ec; fs::remove_all(tmp, ec);
    return Json{ {"success", true}, {"profileId", profileId}, {"versionId", profileId}, {"name", name} };
}

void RegisterGameHandlers(Bridge& b) {
    {
        std::wstring purgeMarker = util::GameDir() + L"\\.libpurge-2.1.8";
        if (!util::Exists(purgeMarker)) {
            std::error_code ec;
            fs::remove_all(util::GameDir() + L"\\libraries", ec);
            util::EnsureDir(util::GameDir());
            util::WriteFile(purgeMarker, "done");
        }
    }
    Bridge* bp = &b;
    auto progTick = std::make_shared<std::atomic<unsigned long long>>(0);
    auto prog = [bp, progTick](const std::string& s, double p) {
        unsigned long long now = GetTickCount64();
        if (p >= 0.999 || now - progTick->load() >= 100) { progTick->store(now); bp->EmitAsync("install-progress", Json{ {"step", s}, {"progress", p} }); }
    };
    auto log = [bp](const std::string& m) { bp->EmitAsync("game-log", Json::array({ m })); };

    auto jlSafePath = [](const std::string& rel) -> std::wstring {
        if (rel.empty() || rel.find("..") != std::string::npos) return L"";
        std::string r = rel; for (auto& c : r) if (c == '/') c = '\\';
        while (!r.empty() && (r.front() == '\\')) r.erase(r.begin());
        return util::GameDir() + L"\\" + U(r);
    };
    b.Register("jl-missing", [jlSafePath](const Json& a) -> Json {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        Json miss = Json::array();
        if (o.contains("paths") && o["paths"].is_array())
            for (auto& p : o["paths"]) {
                if (!p.is_string()) continue;
                std::wstring full = jlSafePath(p.get<std::string>());
                if (full.empty() || !util::Exists(full) || util::FileSize(full) == 0) miss.push_back(p);
            }
        return Json{ {"missing", miss} };
    });
    b.Register("jl-write", [jlSafePath](const Json& a) -> Json {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::wstring full = jlSafePath(o.value("path", ""));
        if (full.empty()) return Json{ {"error", "bad path"} };
        util::EnsureDir(fs::path(full).parent_path().wstring());
        std::string b64 = o.value("dataB64", "");
        std::string bytes = b64.empty() ? o.value("text", "") : util::Base64Decode(b64);
        return util::WriteFile(full, bytes) ? Json{ {"success", true} } : Json{ {"error", "write failed"} };
    });
    b.Register("jl-unique-profile-id", [](const Json& a) -> Json {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string base = o.value("base", "");
        if (base.empty() && a.is_array() && !a.empty() && a[0].is_string()) base = a[0].get<std::string>();
        if (base.empty()) return Json{ {"error", "missing base"} };
        return Json{ {"id", UniqueProfileId(base)} };
    });
    b.RegisterAsync("jl-finalize-install", [bp](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        auto S = [&](const char* k) -> std::string {
            auto it = o.find(k);
            return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
        };
        std::string profileId = S("profileId"), type = S("type"), mc = S("mc");
        std::string loader = S("loader"), customName = S("customName");
        bool vulkan = o.contains("vulkan") && o["vulkan"].is_boolean() && o["vulkan"].get<bool>();
        RunBg([=]() {
            if (profileId.empty() || mc.empty()) { reply(Json{ {"error", "missing profileId/mc"} }, ""); return; }
            SaveProfile(profileId, type.empty() ? "vanilla" : type, mc, loader, customName, vulkan);
            if (type == "fabric" || type == "forge") { try { EnsureJusticeMod(profileId, mc, [](const std::string&) {}); } catch (...) {} }
            reply(Json{ {"success", true}, {"profileId", profileId} }, "");
        });
    });

    b.RegisterAsync("install-vanilla", [bp, prog, log](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string mc = JStr(o, "mcVersion"), cn = JStr(o, "customName");
        RunBg([=]() {
            try { InstallVanilla(mc, prog, log, false); if (!cn.empty()) SaveProfile(mc, "vanilla", mc, "", cn, false);
                  bp->EmitAsync("install-progress", Json{ {"step", "✓ Installed!"}, {"progress", 1} }); reply(Json{ {"success", true} }, ""); }
            catch (const std::exception& e) { reply(Json{ {"error", e.what()} }, ""); }
        });
    });

    b.RegisterAsync("install-fabric", [bp, prog, log](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string mc = JStr(o, "mcVersion"), loader = JStr(o, "loaderVersion"), cn = JStr(o, "customName");
        bool vulkan = o.is_object() && o.contains("vulkan") && o["vulkan"].is_boolean() && o["vulkan"].get<bool>();
        RunBg([=]() {
            try { std::string id = InstallFabric(mc, loader, cn, vulkan, prog, log);
                  bp->EmitAsync("install-progress", Json{ {"step", "✓ Fabric installed!"}, {"progress", 1} });
                  reply(Json{ {"success", true}, {"profileId", id} }, ""); }
            catch (const std::exception& e) { reply(Json{ {"error", e.what()} }, ""); }
        });
    });

    b.RegisterAsync("install-forge", [bp, prog, log](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        std::string mc = JStr(o, "mcVersion"), fv = JStr(o, "forgeVersion"), cn = JStr(o, "customName");
        RunBg([=]() {
            try { std::string id = InstallForge(mc, fv, cn, prog, log);
                  bp->EmitAsync("install-progress", Json{ {"step", "✓ Forge installed!"}, {"progress", 1} });
                  reply(Json{ {"success", true}, {"profileId", id} }, ""); }
            catch (const std::exception& e) { reply(Json{ {"error", e.what()} }, ""); }
        });
    });

    b.RegisterAsync("install-modpack", [bp, log](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        auto lastT = std::make_shared<std::atomic<unsigned long long>>(0);
        auto lastS = std::make_shared<std::string>();
        auto mpProg = [bp, lastT, lastS](const std::string& step, double p) {
            unsigned long long now = GetTickCount64();
            if (step != *lastS || now - lastT->load() >= 150) { *lastS = step; lastT->store(now); bp->EmitAsync("mp-dl-progress", Json{ {"done", (int)(p * 100)}, {"total", 100}, {"step", step} }); }
        };
        RunBg([=]() {
            try { reply(InstallModrinthPack(o, mpProg, log), ""); }
            catch (const std::exception& e) { reply(Json{ {"error", e.what()} }, ""); }
        });
    });
    b.RegisterAsync("cf-install-modpack", [bp, log](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        auto lastT = std::make_shared<std::atomic<unsigned long long>>(0);
        auto lastS = std::make_shared<std::string>();
        auto mpProg = [bp, lastT, lastS](const std::string& step, double p) {
            unsigned long long now = GetTickCount64();
            if (step != *lastS || now - lastT->load() >= 150) { *lastS = step; lastT->store(now); bp->EmitAsync("mp-dl-progress", Json{ {"done", (int)(p * 100)}, {"total", 100}, {"step", step} }); }
        };
        RunBg([=]() {
            try { reply(InstallCfPack(o, mpProg, log), ""); }
            catch (const std::exception& e) { reply(Json{ {"error", e.what()} }, ""); }
        });
    });

    b.RegisterAsync("ensure-justice-mod", [log](const Json& a, Bridge::ReplyFn reply) {
        std::string vid = (a.is_array() && a.size() > 0 && a[0].is_string()) ? a[0].get<std::string>() : "";
        std::string mc = (a.is_array() && a.size() > 1 && a[1].is_string()) ? a[1].get<std::string>() : "";
        RunBg([=]() { try { EnsureJusticeMod(vid, mc, log); reply(Json{ {"ok", true} }, ""); } catch (const std::exception& e) { reply(Json{ {"ok", false}, {"error", e.what()} }, ""); } });
    });

    b.RegisterAsync("check-java", [](const Json&, Bridge::ReplyFn reply) {
        RunBg([reply]() {
            std::wstring j = FindJava();
            if (j.empty()) { reply(Json{ {"found", false}, {"path", nullptr}, {"compatible", false} }, ""); return; }
            reply(Json{ {"found", true}, {"path", util::WideToUtf8(j)}, {"compatible", true} }, "");
        });
    });

    b.RegisterAsync("install-java", [bp](const Json&, Bridge::ReplyFn reply) {
        RunBg([bp, reply]() {
            try { std::wstring j = InstallJava(21, [&](const std::string& s, double p) { bp->EmitAsync("java-install-progress", Json{ {"step", s}, {"progress", p} }); });
                  reply(Json{ {"success", true}, {"path", util::WideToUtf8(j)} }, ""); }
            catch (const std::exception& e) { reply(Json{ {"error", e.what()} }, ""); }
        });
    });

    b.RegisterAsync("launch-game", [bp](const Json& a, Bridge::ReplyFn reply) {
        Json o = (a.is_array() && !a.empty()) ? a[0] : Json::object();
        RunBg([bp, o, reply]() {
            LaunchGame(o, [bp](const std::string& c, const Json& ar) { bp->EmitAsync(c, ar); });
            reply(Json{ {"success", true} }, "");
        });
    });
}

int GameSelfTest(const std::wstring& mode, const std::wstring& arg1) {
    std::wstring logPath;
    { wchar_t* t = nullptr; size_t n = 0;
      if (_wdupenv_s(&t, &n, L"TEMP") == 0 && t) { logPath = std::wstring(t) + L"\\jl_selftest.log"; free(t); } }
    auto write = [&](const std::string& s) {
        std::ofstream f(logPath, std::ios::app | std::ios::binary); f << s;
    };
    { std::ofstream f(logPath, std::ios::trunc | std::ios::binary); }
    auto log = [&](const std::string& m) { write(m); };
    auto prog = [&](const std::string& s, double p) { write("[" + std::to_string((int)(p * 100)) + "%] " + s + "\n"); };

    std::string a1 = util::WideToUtf8(arg1);
    try {
        if (mode == L"install-fabric") {
            std::string mc = a1.empty() ? "1.21.1" : a1;
            write("=== install-fabric " + mc + " ===\n");
            Json loaders = HttpJson("GET", "https://meta.fabricmc.net/v2/versions/loader/" + mc);
            std::string loader = (loaders.is_array() && !loaders.empty() && loaders[0].contains("loader"))
                                 ? loaders[0]["loader"].value("version", "") : "";
            write("loader = " + loader + "\n");
            std::string id = InstallFabric(mc, loader, "SelfTest", false, prog, log);
            write("RESULT: OK profileId=" + id + "\n");
            return 0;
        }
        if (mode == L"install-forge") {
            std::string spec = a1; size_t slash = spec.find('/');
            std::string mc = slash == std::string::npos ? spec : spec.substr(0, slash);
            std::string fv = slash == std::string::npos ? "" : spec.substr(slash + 1);
            write("=== install-forge " + mc + " / " + fv + " ===\n");
            std::string id = InstallForge(mc, fv, "SelfTestForge", prog, log);
            write("RESULT: OK profileId=" + id + "\n");
            return 0;
        }
        if (mode == L"check-update") {
            std::string cur = a1.empty() ? "2.1.6" : a1;
            write("=== check-update (pretending current=" + cur + ") ===\n");
            HttpResponse r = HttpSend("GET", "https://justiceclient.org/downloads/version.txt", { {"User-Agent", "JusticeLauncher/" + cur} });
            std::string latest = (r.ok && r.status == 200) ? r.body : std::string("");
            while (!latest.empty() && (latest.back() == '\n' || latest.back() == '\r' || latest.back() == ' ')) latest.pop_back();
            bool has = !latest.empty() && latest != cur;
            write("http status=" + std::to_string(r.status) + " ok=" + (r.ok ? "1" : "0") + "\n");
            write("server latest=[" + latest + "]  hasUpdate=" + (has ? "TRUE" : "FALSE") + "\n");
            return 0;
        }
        if (mode == L"modpack") {
            write("=== modpack " + a1 + " ===\n");
            Json p = { {"url", a1}, {"fileName", "selftest.mrpack"}, {"packName", "SelfTest Pack"} };
            Json r = InstallModrinthPack(p, prog, log);
            write("RESULT: " + r.dump() + "\n");
            return 0;
        }
        if (mode == L"launch") {
            write("=== launch (offline) " + a1 + " ===\n");
            Json params = { {"versionId", a1}, {"offlineMode", true}, {"username", "SelfTester"}, {"ram", "2048"} };
            LaunchGame(params, [&](const std::string& ch, const Json& ar) {
                if (ch == "game-log" && ar.is_array() && !ar.empty()) write(ar[0].get<std::string>());
                else write("[" + ch + "] " + ar.dump() + "\n");
            });
            std::this_thread::sleep_for(std::chrono::seconds(50));
            write("\nRESULT: launch attempted (see log above)\n");
            return 0;
        }
        write("unknown selftest mode\n");
        return 2;
    } catch (const std::exception& e) {
        write(std::string("RESULT: ERROR ") + e.what() + "\n");
        return 1;
    }
}
