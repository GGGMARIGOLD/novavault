// Vespera - a game store server in C++17.
// Accounts, wallet, store, library, APK/game uploads, owner-only downloads.
#include "httplib.h"
#include "json.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
#include <regex>
#include <sstream>
using json = nlohmann::json;
namespace fs = std::filesystem;

static json db = {{"users", json::object()}, {"games", json::object()}, {"next", 1}};
static std::map<std::string, std::string> sessions; // token -> username
struct Upload { std::string owner, filename; size_t size = 0, got = 0; long at = 0; };
static std::map<std::string, Upload> uploads; // chunked uploads in progress
static const size_t MAX_UPLOAD = 4ULL << 30; // 4 GB per game file
static std::mutex mu;

static std::string hex(const unsigned char* b, size_t n) {
  static const char* d = "0123456789abcdef"; std::string s;
  for (size_t i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
  return s;
}
static std::string randHex(size_t n) { std::string b(n, 0); RAND_bytes((unsigned char*)&b[0], (int)n); return hex((unsigned char*)b.data(), n); }
static std::string hashPw(const std::string& pw, const std::string& salt) {
  unsigned char out[32];
  PKCS5_PBKDF2_HMAC(pw.c_str(), (int)pw.size(), (const unsigned char*)salt.data(), (int)salt.size(), 200000, EVP_sha256(), 32, out);
  return hex(out, 32);
}
static void save() { { std::ofstream("data/db.tmp") << db.dump(); } fs::rename("data/db.tmp", "data/db.json"); }
static void logAct(const std::string& who, const std::string& what, const std::string& detail) {
  if (!db.contains("log")) db["log"] = json::array();
  db["log"].push_back({{"at", (long)time(nullptr)}, {"who", who}, {"what", what}, {"detail", detail}});
  if (db["log"].size() > 1000) db["log"].erase(db["log"].begin());
}
static json& settings() { if (!db.contains("settings")) db["settings"] = {{"announcement", ""}}; return db["settings"]; }
static std::string extOf(const std::string& f) { auto p = f.find_last_of('.'); if (p == std::string::npos) return ""; std::string e = f.substr(p + 1); for (auto& c : e) c = (char)tolower(c); return e; }
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower(c); return s; }
static void reply(httplib::Response& r, int code, const json& j) { r.status = code; r.set_content(j.dump(), "application/json"); }
static json err(const std::string& m) { return {{"error", m}}; }
static long eff(const json& g) { long p = g.value("price", 0L), s = g.value("sale", 0L); return (s > 0 && s < p) ? s : p; }
static json pub(const json& g) { json o = g; o.erase("file"); o.erase("cover"); o.erase("revenue"); o.erase("update"); o["status"] = g.value("status", "approved"); o["hasCover"] = g.contains("cover") && g["cover"] != ""; return o; }

// Returns the logged-in username or "" (caller holds mu).
static std::string whoami(const httplib::Request& q) {
  std::string c = q.get_header_value("Cookie"); auto p = c.find("sid=");
  if (p == std::string::npos) return "";
  auto e = c.find(';', p); auto t = c.substr(p + 4, e == std::string::npos ? e : e - p - 4);
  auto it = sessions.find(t); if (it == sessions.end()) return ""; if (db["users"][it->second].value("banned", false)) return ""; return it->second;
}
static std::string roleOf(const std::string& n) { return n.empty() ? "" : db["users"][n].value("role", "user"); }
static bool isMod(const std::string& n) { auto r = roleOf(n); return r == "admin" || r == "mod"; }
static bool isAdmin(const std::string& n) { return roleOf(n) == "admin"; }
static json earned(const std::string& n) {
  json b = json::array(); auto& u = db["users"][n]; std::string r = u.value("role", "user");
  if (r == "admin") b.push_back("Admin"); if (r == "mod") b.push_back("Moderator");
  json gr = u.value("granted", json::array()); for (auto& x : gr) b.push_back(x);
  bool dev = false; for (auto& [k, g] : db["games"].items()) if (g.value("owner", "") == n && g.value("status", "approved") == "approved") dev = true;
  if (dev) b.push_back("Developer"); if (u["library"].size() >= 10) b.push_back("Collector");
  return b;
}
static json shownBadges(const std::string& n) {
  auto e = earned(n); json out = json::array(); json sh = db["users"][n].value("shown", json::array());
  for (auto& x : sh) if (std::find(e.begin(), e.end(), x) != e.end() && out.size() < 12) out.push_back(x);
  if (out.empty()) for (auto& x : e) if (out.size() < 12) out.push_back(x);
  return out;
}
static std::string displayOf(const json& u) { std::string d = u.value("display", ""); return d.empty() ? u["name"].get<std::string>() : d; }
static std::string cleanDisplay(std::string t) {
  std::string o; for (unsigned char c : t) if (c >= 32 && c != 127) o += (char)c;
  auto a = o.find_first_not_of(' '); if (a == std::string::npos) return ""; o = o.substr(a, o.find_last_not_of(' ') - a + 1);
  if (o.size() > 32) { o.resize(32); while (!o.empty() && ((unsigned char)o.back() & 0xC0) == 0x80) o.pop_back(); if (!o.empty() && (unsigned char)o.back() >= 0xC0) o.pop_back(); }
  return o;
}
static bool inList(const json& u, const char* k, const std::string& v) { for (auto& x : u.value(k, json::array())) if (x == v) return true; return false; }
static void addTo(json& u, const char* k, const std::string& v) { if (!u.contains(k)) u[k] = json::array(); if (!inList(u, k, v)) u[k].push_back(v); }
static void dropFrom(json& u, const char* k, const std::string& v) { json o = json::array(); for (auto& x : u.value(k, json::array())) if (x != v) o.push_back(x); u[k] = o; }
static std::string mimeOf(const std::string& p) { auto e = p.substr(p.find_last_of('.') + 1); return e == "png" ? "image/png" : e == "webp" ? "image/webp" : "image/jpeg"; }
static json shownBadges(const std::string& n);
static json summary(const std::string& k) {
  auto& u = db["users"][k]; std::string pr = u.value("presence", "online"); json eq = u.value("equipped", json::object());
  return {{"key", k}, {"name", u["name"]}, {"display", displayOf(u)}, {"hasAvatar", u.value("avatar", "") != ""}, {"presence", pr == "invisible" ? "offline" : pr},
    {"statusText", u.value("statusText", "")}, {"frame", eq.value("frame", "")}, {"nameStyle", eq.value("name", "")}, {"badges", shownBadges(k)}, {"banned", u.value("banned", false)}};
}
static bool owns(const json& u, int id) { for (auto& x : u["library"]) if (x == id) return true; return false; }
static std::string safeName(std::string n) { for (auto& c : n) if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') c = '_'; return n.empty() ? "game.bin" : n; }
static long credits(const std::string& s) { try { return std::clamp(std::lround(std::stod(s)), 0L, 1000000L); } catch (...) { return 0; } }
static void rmFile(const std::string& name) { if (name.empty()) return; std::error_code ec; fs::remove("data/files/" + name, ec); }
// Profile store catalog: everything is bought with credits.
static const json CATALOG = json::parse(R"([
  {"id":"frame_gold","slot":"frame","name":"Gilded Ring","price":400},
  {"id":"frame_neon","slot":"frame","name":"Neon Pulse","price":600},
  {"id":"frame_ember","slot":"frame","name":"Ember Glow","price":800},
  {"id":"frame_frost","slot":"frame","name":"Frostbite","price":800},
  {"id":"frame_aurora","slot":"frame","name":"Aurora Halo","price":1500},
  {"id":"frame_void","slot":"frame","name":"Void Rift","price":2000},
  {"id":"theme_dusk","slot":"theme","name":"Vesper Dusk","price":500},
  {"id":"theme_ocean","slot":"theme","name":"Deep Ocean","price":500},
  {"id":"theme_forest","slot":"theme","name":"Moss Forest","price":500},
  {"id":"theme_rose","slot":"theme","name":"Rose Quartz","price":700},
  {"id":"theme_crimson","slot":"theme","name":"Crimson Night","price":900},
  {"id":"theme_nebula","slot":"theme","name":"Living Nebula","price":1800},
  {"id":"name_gold","slot":"name","name":"Gold Leaf","price":300},
  {"id":"name_ice","slot":"name","name":"Glacier","price":300},
  {"id":"name_fire","slot":"name","name":"Wildfire","price":600},
  {"id":"name_prism","slot":"name","name":"Prism Shift","price":1200},
  {"id":"effect_stars","slot":"effect","name":"Starfall","price":900},
  {"id":"effect_snow","slot":"effect","name":"Snowdrift","price":900},
  {"id":"effect_embers","slot":"effect","name":"Rising Embers","price":1100},
  {"id":"frame_pixel","slot":"frame","name":"Pixel Border","price":700},
  {"id":"frame_radar","slot":"frame","name":"Radar Pulse","price":900},
  {"id":"frame_chrome","slot":"frame","name":"Liquid Chrome","price":1000},
  {"id":"frame_glitch","slot":"frame","name":"Glitch Ring","price":1200},
  {"id":"frame_thorns","slot":"frame","name":"Thorn Crown","price":1300},
  {"id":"frame_holo","slot":"frame","name":"Hologram","price":1400},
  {"id":"theme_ink","slot":"theme","name":"Ink Smoke","price":900},
  {"id":"theme_gold","slot":"theme","name":"Midnight Gold","price":1000},
  {"id":"theme_aurora","slot":"theme","name":"Aurora Sky","price":1200},
  {"id":"theme_lava","slot":"theme","name":"Lava Flow","price":1400},
  {"id":"theme_rain","slot":"theme","name":"Data Rain","price":1500},
  {"id":"theme_synth","slot":"theme","name":"Synthwave Grid","price":1600},
  {"id":"name_retro","slot":"name","name":"Retro 3D","price":700},
  {"id":"name_chrome","slot":"name","name":"Chrome","price":800},
  {"id":"name_neon","slot":"name","name":"Neon Sign","price":900},
  {"id":"name_glitch","slot":"name","name":"Glitch","price":1000},
  {"id":"name_holo","slot":"name","name":"Holographic","price":1100},
  {"id":"effect_crt","slot":"effect","name":"CRT Scanlines","price":700},
  {"id":"effect_rain","slot":"effect","name":"Night Rain","price":800},
  {"id":"effect_bubbles","slot":"effect","name":"Bubbles","price":800},
  {"id":"effect_fireflies","slot":"effect","name":"Fireflies","price":1000},
  {"id":"effect_shooting","slot":"effect","name":"Shooting Stars","price":1200},
  {"id":"effect_lightning","slot":"effect","name":"Thunderstrike","price":1300},
  {"id":"frame_sovereign","slot":"frame","name":"Sovereign Halo","price":0,"owner":true},
  {"id":"frame_horizon","slot":"frame","name":"Event Horizon","price":0,"owner":true},
  {"id":"frame_hellfire","slot":"frame","name":"Hellfire","price":0,"owner":true},
  {"id":"theme_throne","slot":"theme","name":"Throne Room","price":0,"owner":true},
  {"id":"theme_galaxy","slot":"theme","name":"Galaxy Core","price":0,"owner":true},
  {"id":"theme_abyss","slot":"theme","name":"The Abyss","price":0,"owner":true},
  {"id":"name_liquidgold","slot":"name","name":"Liquid Gold","price":0,"owner":true},
  {"id":"name_overlord","slot":"name","name":"Overlord","price":0,"owner":true},
  {"id":"name_starlight","slot":"name","name":"Starlight","price":0,"owner":true},
  {"id":"effect_meteor","slot":"effect","name":"Meteor Storm","price":0,"owner":true},
  {"id":"effect_goldrain","slot":"effect","name":"Gold Rain","price":0,"owner":true},
  {"id":"effect_warp","slot":"effect","name":"Warp Speed","price":0,"owner":true}
])");
static const json* item(const std::string& id) { for (auto& i : CATALOG) if (i["id"] == id) return &i; return nullptr; }

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  fs::create_directories("data/files");
  if (fs::exists("data/db.json")) { std::ifstream f("data/db.json"); db = json::parse(f, nullptr, false); if (db.is_discarded()) { fprintf(stderr, "db.json is corrupt\n"); return 1; } }
  if (const char* a = getenv("ADMIN_USER")) { auto k = lower(a); if (db["users"].contains(k)) { db["users"][k]["role"] = "admin"; save(); printf("%s is an admin\n", a); } }
  fs::remove_all("data/tmp"); fs::create_directories("data/tmp");
  httplib::Server svr;
  svr.set_payload_max_length(64ULL << 20); // single requests are small: big game files arrive in chunks

  svr.Post("/api/register", [&](const httplib::Request& q, httplib::Response& r) {
    auto b = json::parse(q.body, nullptr, false); std::lock_guard<std::mutex> l(mu);
    if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string n = b.value("username", ""), pw = b.value("password", "");
    if (!std::regex_match(n, std::regex("[A-Za-z0-9_]{3,24}"))) return reply(r, 400, err("Username: 3-24 letters, numbers or _"));
    if (pw.size() < 8) return reply(r, 400, err("Password must be at least 8 characters"));
    if (db["users"].contains(lower(n))) return reply(r, 409, err("That username is taken"));
    std::string salt = randHex(16); std::string role = db["users"].empty() ? "admin" : "user";
    db["users"][lower(n)] = {{"role", role}, {"joined", (long)time(nullptr)}, {"name", n}, {"salt", salt}, {"hash", hashPw(pw, salt)}, {"balance", 0}, {"library", json::array()}};
    logAct(n, "register", "New account"); save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post("/api/login", [&](const httplib::Request& q, httplib::Response& r) {
    auto b = json::parse(q.body, nullptr, false); std::lock_guard<std::mutex> l(mu);
    if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string n = lower(b.value("username", ""));
    if (!db["users"].contains(n)) return reply(r, 401, err("Wrong username or password"));
    auto& u = db["users"][n];
    if (hashPw(b.value("password", ""), u["salt"]) != u["hash"]) return reply(r, 401, err("Wrong username or password"));
    if (u.value("banned", false)) { std::string why = u.value("banReason", ""); return reply(r, 403, err("This account is banned" + (why.empty() ? std::string(".") : ": " + why))); }
    std::string t = randHex(24); sessions[t] = n;
    r.set_header("Set-Cookie", "sid=" + t + "; HttpOnly; SameSite=Lax; Path=/; Max-Age=2592000");
    reply(r, 200, {{"ok", true}});
  });
  svr.Post("/api/logout", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); std::string c = q.get_header_value("Cookie"); auto p = c.find("sid=");
    if (p != std::string::npos) sessions.erase(c.substr(p + 4, 48));
    r.set_header("Set-Cookie", "sid=; Path=/; Max-Age=0"); reply(r, 200, {{"ok", true}});
  });
  svr.Get("/api/me", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q);
    if (n.empty()) return reply(r, 200, {{"user", nullptr}, {"announcement", settings().value("announcement", "")}});
    auto& u = db["users"][n]; reply(r, 200, {{"user", {{"name", u["name"]}, {"display", displayOf(u)}, {"balance", u["balance"]}, {"library", u["library"]}, {"role", roleOf(n)}, {"equipped", u.value("equipped", json::object())}, {"friendRequests", u.value("friendReq", json::array()).size()}}}, {"announcement", settings().value("announcement", "")}});
  });
  svr.Get(R"(/api/profile/(\w+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); std::string n = lower(q.matches[1]), me = whoami(q);
    if (!db["users"].contains(n)) return reply(r, 404, err("No such user"));
    auto& u = db["users"][n]; std::string pr = u.value("presence", "online"); json games = json::array();
    for (auto& [k, g] : db["games"].items()) if (g.value("owner", "") == n && g.value("status", "approved") == "approved") games.push_back(pub(g));
    json p = {{"name", u["name"]}, {"display", displayOf(u)}, {"key", n}, {"role", u.value("role", "user")}, {"bio", u.value("bio", "")}, {"statusText", u.value("statusText", "")},
      {"presence", (pr == "invisible" && me != n) ? "offline" : pr}, {"badges", shownBadges(n)}, {"hasAvatar", u.value("avatar", "") != ""}, {"hasBanner", u.value("banner", "") != ""},
      {"joined", u.value("joined", 0L)}, {"games", games}, {"owned", u["library"].size()}, {"equipped", u.value("equipped", json::object())}};
    if (me == n) { p["earned"] = earned(n); p["shown"] = shownBadges(n); }
    long followers = 0; for (auto& [k, x] : db["users"].items()) if (inList(x, "following", n)) followers++;
    p["followers"] = followers; p["followingCount"] = u.value("following", json::array()).size(); p["friendsCount"] = u.value("friends", json::array()).size();
    if (!me.empty() && me != n) { auto& v = db["users"][me]; p["isFollowing"] = inList(v, "following", n);
      p["friendState"] = inList(v, "friends", n) ? "friends" : inList(u, "friendReq", me) ? "sent" : inList(v, "friendReq", n) ? "received" : "none"; }
    reply(r, 200, p);
  });
  svr.Post("/api/profile", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (n.empty()) return reply(r, 401, err("Log in first"));
    auto& u = db["users"][n]; auto F = [&](const char* k) { return q.get_file_value(k).content; };
    if (q.has_file("display")) u["display"] = cleanDisplay(F("display"));
    if (q.has_file("bio")) { auto t = F("bio"); if (t.size() > 500) t.resize(500); u["bio"] = t; }
    if (q.has_file("statusText")) { auto t = F("statusText"); if (t.size() > 80) t.resize(80); u["statusText"] = t; }
    if (q.has_file("presence")) { auto p = F("presence"); if (p == "online" || p == "away" || p == "busy" || p == "invisible") u["presence"] = p; }
    if (q.has_file("shown")) { json sh = json::array(); std::stringstream ss(F("shown")); std::string t; auto e = earned(n);
      while (std::getline(ss, t, '|')) if (std::find(e.begin(), e.end(), t) != e.end() && sh.size() < 12) sh.push_back(t); u["shown"] = sh; }
    std::string bad;
    auto img = [&](const char* ff, const char* key, const char* rf, size_t lim) {
      auto drop = [&]() { if (u.value(key, "") != "") fs::remove("data/files/" + u[key].get<std::string>()); };
      if (F(rf) == "1") { drop(); u[key] = ""; }
      if (!q.has_file(ff)) return;
      auto c = q.get_file_value(ff); std::string ext = c.content_type == "image/png" ? ".png" : c.content_type == "image/jpeg" ? ".jpg" : c.content_type == "image/webp" ? ".webp" : "";
      if (ext.empty()) { bad = "Images must be PNG, JPG or WebP"; return; }
      if (c.content.size() > lim) { bad = "Image is too large"; return; }
      drop(); std::string nm = randHex(12) + ext; std::ofstream("data/files/" + nm, std::ios::binary).write(c.content.data(), (std::streamsize)c.content.size()); u[key] = nm;
    };
    img("avatar", "avatar", "removeAvatar", 2u << 20); img("banner", "banner", "removeBanner", 5u << 20);
    save(); if (!bad.empty()) return reply(r, 400, err(bad)); reply(r, 200, {{"ok", true}});
  });
  auto serveImg = [&](const httplib::Request& q, httplib::Response& r, const char* key) {
    std::string path; { std::lock_guard<std::mutex> l(mu); std::string k = lower(q.matches[1]);
      if (!db["users"].contains(k) || db["users"][k].value(key, "") == "") { r.status = 404; return; } path = "data/files/" + db["users"][k][key].get<std::string>(); }
    std::ifstream f(path, std::ios::binary); std::string d((std::istreambuf_iterator<char>(f)), {}); r.set_header("Cache-Control", "no-cache"); r.set_content(d, mimeOf(path));
  };
  svr.Get(R"(/avatar/(\w+))", [&](const httplib::Request& q, httplib::Response& r) { serveImg(q, r, "avatar"); });
  svr.Get(R"(/banner/(\w+))", [&](const httplib::Request& q, httplib::Response& r) { serveImg(q, r, "banner"); });
  svr.Post("/api/admin/badge", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string t = lower(b.value("username", "")), bd = b.value("badge", ""); bool give = b.value("grant", true);
    if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (bd != "Partner" && bd != "Owner" && bd != "Founder" && bd != "Beta Tester" && bd != "Verified Dev" && bd != "Supporter") return reply(r, 400, err("Unknown badge"));
    json out = json::array(); json cur = db["users"][t].value("granted", json::array()); for (auto& x : cur) if (x != bd) out.push_back(x);
    if (give) out.push_back(bd); db["users"][t]["granted"] = out; logAct(whoami(q), give ? "badge+" : "badge-", db["users"][t]["name"].get<std::string>() + ": " + bd); save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post("/api/uploads", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (n.empty()) return reply(r, 401, err("Log in to upload"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    size_t size = b.value("size", (size_t)0); std::string name = b.value("filename", "");
    if (size == 0) return reply(r, 400, err("That file is empty"));
    if (size > MAX_UPLOAD) return reply(r, 413, err("Files can be at most 4 GB"));
    int mine = 0; for (auto& [k, u] : uploads) if (u.owner == n) mine++;
    if (mine >= 3) return reply(r, 429, err("Finish your other uploads first"));
    std::string id = randHex(16); uploads[id] = {n, safeName(name), size, 0, (long)time(nullptr)};
    std::ofstream("data/tmp/" + id, std::ios::binary); reply(r, 200, {{"uploadId", id}});
  });
  svr.Put(R"(/api/uploads/([0-9a-f]{32}))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    auto it = uploads.find(id); if (it == uploads.end() || it->second.owner != n) return reply(r, 404, err("Upload not found. Start again."));
    auto& u = it->second; size_t off = 0; try { off = std::stoull(q.get_param_value("offset")); } catch (...) { return reply(r, 400, err("Bad offset")); }
    if (off != u.got) return reply(r, 409, {{"error", "Out of order"}, {"got", u.got}});
    if (u.got + q.body.size() > u.size) return reply(r, 400, err("More data than the file size"));
    { std::ofstream o("data/tmp/" + id, std::ios::binary | std::ios::app); o.write(q.body.data(), (std::streamsize)q.body.size()); if (!o) return reply(r, 500, err("The server ran out of disk space")); }
    u.got += q.body.size(); reply(r, 200, {{"got", u.got}});
  });
  svr.Get("/api/games", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto me = whoami(q); bool mod = isMod(me); json a = json::array();
    bool adm = isAdmin(me);
    for (auto& [k, g] : db["games"].items()) if (g.value("status", "approved") == "approved" || mod || g.value("owner", "") == me) { a.push_back(pub(g)); if (adm) a.back()["revenue"] = g.value("revenue", 0L);
      if (g.contains("update") && (mod || g.value("owner", "") == me)) { json u = g["update"]; u.erase("file"); u["hasCover"] = u.value("cover", "") != ""; u.erase("cover"); a.back()["update"] = u; } }
    std::sort(a.begin(), a.end(), [](const json& x, const json& y) { return x.value("at", 0L) > y.value("at", 0L); });
    reply(r, 200, a);
  });
  // Takes the game file from a finished chunked upload (uploadId) or a plain multipart "file". Returns an error or "".
  auto takeFile = [&](const httplib::Request& q, const std::string& n, std::string& stored, std::string& fname, size_t& size) -> std::string {
    std::string up = q.get_file_value("uploadId").content;
    if (!up.empty()) {
      auto it = uploads.find(up); if (it == uploads.end() || it->second.owner != n) return "Upload not found. Choose the file again.";
      if (it->second.got != it->second.size) return "The file hasn't finished uploading";
      stored = randHex(12); std::error_code ec; fs::rename("data/tmp/" + up, "data/files/" + stored, ec); if (ec) return "Couldn't store the file";
      fname = it->second.filename; size = it->second.size; uploads.erase(it); return "";
    }
    if (q.has_file("file")) { auto f = q.get_file_value("file"); if (f.content.empty()) return "That file is empty";
      stored = randHex(12); std::ofstream("data/files/" + stored, std::ios::binary).write(f.content.data(), (std::streamsize)f.content.size()); fname = safeName(f.filename); size = f.content.size(); }
    return "";
  };
  auto takeCover = [&](const httplib::Request& q) -> std::string {
    if (!q.has_file("cover")) return "";
    auto c = q.get_file_value("cover"); std::string ext = c.content_type == "image/png" ? ".png" : c.content_type == "image/jpeg" ? ".jpg" : c.content_type == "image/webp" ? ".webp" : "";
    if (ext.empty() || c.content.empty() || c.content.size() >= (8u << 20)) return "";
    std::string name = randHex(12) + ext; std::ofstream("data/files/" + name, std::ios::binary).write(c.content.data(), (std::streamsize)c.content.size()); return name;
  };
  static const char* FIELDS[] = {"title", "desc", "genre", "platform", "version", "os", "ram", "storage"};
  // Moves an approved update (or direct edit) onto the game, replacing the old file/cover.
  auto applyUpdate = [&](json& g, const json& u) {
    json fl = u.value("fields", json::object()); for (auto& [k, v] : fl.items()) g[k] = v;
    if (u.value("file", "") != "") { rmFile(g.value("file", "")); g["file"] = u["file"]; g["filename"] = u["filename"]; g["size"] = u["size"]; g["filetype"] = extOf(u["filename"]); }
    if (u.value("cover", "") != "") { rmFile(g.value("cover", "")); g["cover"] = u["cover"]; }
    g["updatedAt"] = (long)time(nullptr);
  };
  svr.Post("/api/games", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q);
    if (n.empty()) return reply(r, 401, err("Log in to publish"));
    auto F = [&](const char* k) { return q.get_file_value(k).content; };
    std::string title = F("title"); if (title.empty()) return reply(r, 400, err("Enter a title"));
    std::string stored, filename; size_t size = 0;
    auto e = takeFile(q, n, stored, filename, size); if (!e.empty()) return reply(r, 400, err(e));
    if (stored.empty()) return reply(r, 400, err("Choose a game file (.exe, .apk, .zip and more)"));
    std::string cover = takeCover(q);
    int id = db["next"]; db["next"] = id + 1; bool admin = isAdmin(n);
    db["games"][std::to_string(id)] = {{"id", id}, {"title", title}, {"dev", db["users"][n]["name"]}, {"owner", n}, {"desc", F("desc")}, {"genre", F("genre")},
      {"price", credits(F("price"))}, {"sale", credits(F("sale"))}, {"platform", F("platform")}, {"version", F("version")}, {"os", F("os")}, {"ram", F("ram")}, {"storage", F("storage")},
      {"size", size}, {"filename", filename}, {"filetype", extOf(filename)}, {"file", stored}, {"cover", cover}, {"at", (long)time(nullptr)},
      {"status", admin ? "approved" : "pending"}, {"featured", false}, {"sales", 0}, {"revenue", 0}};
    if (admin) { db["games"][std::to_string(id)]["reviewedBy"] = db["users"][n]["name"]; db["games"][std::to_string(id)]["reviewedAt"] = (long)time(nullptr); }
    logAct(n, "submit", title + " (" + filename + ")"); save(); reply(r, 200, {{"id", id}, {"status", admin ? "approved" : "pending"}});
  });
  // Developers update their game. Before acceptance edits apply directly (and go back to review);
  // after acceptance they become a pending update that an admin reviews, and the price is locked.
  svr.Post(R"(/api/games/(\d+)/update)", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    if (n.empty()) return reply(r, 401, err("Log in first"));
    if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto& g = db["games"][id]; bool admin = isAdmin(n);
    if (g.value("owner", "") != n && !admin) return reply(r, 403, err("Only the developer can update this game"));
    if (q.has_file("title") && q.get_file_value("title").content.empty()) return reply(r, 400, err("The title can't be empty"));
    json fields = json::object();
    for (auto k : FIELDS) if (q.has_file(k)) { std::string v = q.get_file_value(k).content; if (v != g.value(k, "")) fields[k] = v; }
    std::string stored, filename; size_t size = 0;
    auto e = takeFile(q, n, stored, filename, size); if (!e.empty()) return reply(r, 400, err(e));
    std::string cover = takeCover(q), st = g.value("status", "approved");
    bool live = st == "approved" || st == "hidden";
    json upd = {{"fields", fields}, {"file", stored}, {"filename", filename}, {"size", size}, {"filetype", extOf(filename)}, {"cover", cover}, {"at", (long)time(nullptr)},
      {"note", q.get_file_value("note").content.substr(0, 500)}};
    bool priceChange = (q.has_file("price") && credits(q.get_file_value("price").content) != g.value("price", 0L)) || (q.has_file("sale") && credits(q.get_file_value("sale").content) != g.value("sale", 0L));
    if (live && !admin && priceChange) { rmFile(stored); rmFile(cover); return reply(r, 403, err("The price of an accepted game is locked. Ask an admin to change it.")); }
    if (fields.empty() && stored.empty() && cover.empty() && !priceChange) return reply(r, 400, err("Nothing changed"));
    std::string t = g["title"];
    if (admin || !live) {
      applyUpdate(g, upd);
      if (q.has_file("price")) g["price"] = credits(q.get_file_value("price").content);
      if (q.has_file("sale")) g["sale"] = credits(q.get_file_value("sale").content);
      if (!admin) { g["status"] = "pending"; g["reason"] = ""; g["at"] = (long)time(nullptr); }
      logAct(n, "edit", t); save(); return reply(r, 200, {{"applied", true}, {"status", g["status"]}});
    }
    if (g.contains("update")) { rmFile(g["update"].value("file", "")); rmFile(g["update"].value("cover", "")); }
    upd["status"] = "pending"; g["update"] = upd;
    logAct(n, "update", t + (filename.empty() ? "" : " (new file " + filename + ")")); save(); reply(r, 200, {{"applied", false}, {"status", "update-pending"}});
  });
  svr.Post(R"(/api/mod/games/(\d+)/update/(approve|reject))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (!isMod(n)) return reply(r, 403, err("Moderators only"));
    std::string id = q.matches[1], act = q.matches[2]; if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto& g = db["games"][id]; if (!g.contains("update") || g["update"].value("status", "") != "pending") return reply(r, 400, err("No update waiting for review"));
    std::string t = g["title"];
    if (act == "approve") { applyUpdate(g, g["update"]); g.erase("update"); g["reviewedBy"] = db["users"][n]["name"]; g["reviewedAt"] = (long)time(nullptr); logAct(n, "approve update", t); }
    else { auto b = json::parse(q.body, nullptr, false); std::string why = b.is_object() ? b.value("reason", "") : "";
      rmFile(g["update"].value("file", "")); rmFile(g["update"].value("cover", ""));
      g["update"] = {{"status", "rejected"}, {"reason", why}, {"at", (long)time(nullptr)}}; logAct(n, "reject update", t + (why.empty() ? "" : " (" + why + ")")); }
    save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post(R"(/api/games/(\d+)/resubmit)", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto& g = db["games"][id]; if (g.value("owner", "") != n) return reply(r, 403, err("Only the developer can resubmit"));
    if (g.value("status", "") != "rejected") return reply(r, 400, err("Only rejected games can be resubmitted"));
    g["status"] = "pending"; g["reason"] = ""; g["at"] = (long)time(nullptr); logAct(n, "resubmit", g["title"].get<std::string>()); save(); reply(r, 200, {{"ok", true}});
  });
  svr.Delete(R"(/api/games/(\d+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto g = db["games"][id]; if (g["owner"] != n && !isAdmin(n)) return reply(r, 403, err("Only the developer or an admin can remove this game"));
    rmFile(g.value("file", "")); rmFile(g.value("cover", "")); if (g.contains("update")) { rmFile(g["update"].value("file", "")); rmFile(g["update"].value("cover", "")); }
    if (db.contains("comments")) db["comments"].erase(id); logAct(n, "delete", g["title"].get<std::string>()); db["games"].erase(id); save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post(R"(/api/games/(\d+)/buy)", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    if (n.empty()) return reply(r, 401, err("Log in to get games"));
    if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto& g = db["games"][id]; auto& u = db["users"][n]; int gid = g["id"];
    if (g.value("status", "approved") != "approved") return reply(r, 403, err("This game is not approved yet"));
    if (owns(u, gid)) return reply(r, 200, {{"ok", true}});
    long p = eff(g);
    if (u["balance"].get<long>() < p) return reply(r, 402, err("Not enough credits. You need " + std::to_string(p) + " and have " + std::to_string(u["balance"].get<long>()) + "."));
    u["balance"] = u["balance"].get<long>() - p;
    if (p > 0 && g["owner"] != n) { auto& d = db["users"][g["owner"].get<std::string>()]; d["balance"] = d["balance"].get<long>() + p; } // developer payout
    g["sales"] = g.value("sales", 0) + 1; g["revenue"] = g.value("revenue", 0L) + p; logAct(n, "purchase", g["title"].get<std::string>() + (p ? " for " + std::to_string(p) + " credits" : " (free)"));
    u["library"].push_back(gid); save(); reply(r, 200, {{"ok", true}, {"balance", u["balance"]}});
  });
  svr.Get(R"(/download/(\d+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::string path, name; size_t size = 0;
    { std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
      if (n.empty()) return reply(r, 401, err("Log in first"));
      if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
      auto& g = db["games"][id]; if (!owns(db["users"][n], g["id"]) && g["owner"] != n && !isMod(n)) return reply(r, 403, err("Add this game to your library first"));
      path = "data/files/" + g["file"].get<std::string>(); name = g["filename"]; size = g["size"];
      if (q.has_param("update")) { if (!g.contains("update") || g["update"].value("file", "") == "" || (g["owner"] != n && !isMod(n))) return reply(r, 404, err("No new file in this update"));
        path = "data/files/" + g["update"]["file"].get<std::string>(); name = g["update"]["filename"]; size = g["update"]["size"]; } }
    r.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
    r.set_content_provider(size, "application/octet-stream", [path](size_t off, size_t len, httplib::DataSink& s) {
      std::ifstream f(path, std::ios::binary); f.seekg((std::streamoff)off); std::vector<char> b(std::min(len, (size_t)1 << 20)); f.read(b.data(), (std::streamsize)b.size());
      return s.write(b.data(), (size_t)f.gcount());
    });
  });
  svr.Get(R"(/cover/(\d+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::string path; { std::lock_guard<std::mutex> l(mu); std::string id = q.matches[1];
      if (!db["games"].contains(id)) { r.status = 404; return; } auto& g = db["games"][id]; std::string c = g.value("cover", "");
      if (q.has_param("update")) { auto me = whoami(q); c = (g.contains("update") && (g["owner"] == me || isMod(me))) ? g["update"].value("cover", "") : ""; }
      if (c.empty()) { r.status = 404; return; } path = "data/files/" + c; }
    std::ifstream f(path, std::ios::binary); std::string d((std::istreambuf_iterator<char>(f)), {});
    r.set_content(d, path.size() > 4 && path.substr(path.size() - 4) == ".png" ? "image/png" : path.back() == 'p' ? "image/webp" : "image/jpeg");
  });
  auto setStatus = [&](const httplib::Request& q, httplib::Response& r, const char* st) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q);
    if (!isMod(n)) return reply(r, 403, err("Moderators only"));
    std::string id = q.matches[1]; if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto& g = db["games"][id]; g["status"] = st; auto b = json::parse(q.body, nullptr, false);
    g["reason"] = (b.is_object() && std::string(st) == "rejected") ? b.value("reason", "") : "";
    g["reviewedBy"] = db["users"][n]["name"]; g["reviewedAt"] = (long)time(nullptr);
    logAct(n, std::string(st) == "approved" ? "approve" : "reject", g["title"].get<std::string>() + (g["reason"] != "" ? " (" + g["reason"].get<std::string>() + ")" : ""));
    save(); reply(r, 200, {{"ok", true}});
  };
  svr.Post(R"(/api/mod/games/(\d+)/approve)", [&](const httplib::Request& q, httplib::Response& r) { setStatus(q, r, "approved"); });
  svr.Post(R"(/api/mod/games/(\d+)/reject)", [&](const httplib::Request& q, httplib::Response& r) { setStatus(q, r, "rejected"); });
  svr.Get("/api/admin/users", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    json a = json::array(); for (auto& [k, u] : db["users"].items()) { int made = 0; for (auto& [gk, g] : db["games"].items()) if (g.value("owner", "") == k) made++;
      a.push_back({{"name", u["name"]}, {"display", displayOf(u)}, {"key", k}, {"role", u.value("role", "user")}, {"balance", u["balance"]}, {"joined", u.value("joined", 0L)}, {"owned", u["library"].size()},
        {"made", made}, {"banned", u.value("banned", false)}, {"banReason", u.value("banReason", "")}, {"hasAvatar", u.value("avatar", "") != ""}, {"granted", u.value("granted", json::array())}}); }
    reply(r, 200, a);
  });
  svr.Post("/api/admin/credits", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string t = lower(b.value("username", "")); double raw = b.value("amount", 0.0);
    if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (!(std::abs(raw) <= 10000000)) return reply(r, 400, err("Amount must be at most 10,000,000 credits"));
    long amt = std::lround(raw); auto& u = db["users"][t]; u["balance"] = std::max(0L, u["balance"].get<long>() + amt);
    logAct(whoami(q), "credits", u["name"].get<std::string>() + " " + (amt > 0 ? "+" : "") + std::to_string(amt) + " credits"); save(); reply(r, 200, {{"balance", u["balance"]}});
  });
  svr.Post("/api/admin/role", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string t = lower(b.value("username", "")), role = b.value("role", "");
    if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (role != "user" && role != "mod") return reply(r, 400, err("Role must be user or mod"));
    if (isAdmin(t)) return reply(r, 400, err("Admin role can't be changed here"));
    db["users"][t]["role"] = role; logAct(whoami(q), "role", db["users"][t]["name"].get<std::string>() + " is now " + role); save(); reply(r, 200, {{"ok", true}});
  });
  svr.Get("/api/admin/overview", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    long updates = 0, users = 0, banned = 0, mods = 0, wallets = 0, sales = 0, revenue = 0, live = 0, pending = 0, rejected = 0, hidden = 0, bytes = 0;
    for (auto& [k, u] : db["users"].items()) { users++; if (u.value("banned", false)) banned++; if (u.value("role", "user") != "user") mods++; wallets += u["balance"].get<long>(); }
    json top = json::array();
    for (auto& [k, g] : db["games"].items()) { auto st = g.value("status", "approved"); if (st == "approved") live++; else if (st == "pending") pending++; else if (st == "rejected") rejected++; else hidden++;
      if (g.contains("update") && g["update"].value("status", "") == "pending") updates++; sales += g.value("sales", 0L); revenue += g.value("revenue", 0L); bytes += g.value("size", 0L);
      top.push_back({{"id", g["id"]}, {"title", g["title"]}, {"sales", g.value("sales", 0)}, {"revenue", g.value("revenue", 0L)}}); }
    std::sort(top.begin(), top.end(), [](const json& a, const json& b) { return a["revenue"].get<long>() != b["revenue"].get<long>() ? a["revenue"].get<long>() > b["revenue"].get<long>() : a["sales"].get<long>() > b["sales"].get<long>(); });
    if (top.size() > 5) top.erase(top.begin() + 5, top.end());
    json recent = json::array(); json lg = db.value("log", json::array()); for (size_t i = lg.size(); i-- > 0 && recent.size() < 8;) recent.push_back(lg[i]);
    reply(r, 200, {{"users", users}, {"banned", banned}, {"staff", mods}, {"wallets", wallets}, {"sales", sales}, {"revenue", revenue}, {"live", live}, {"pending", pending},
      {"rejected", rejected}, {"hidden", hidden}, {"storage", bytes}, {"shopRevenue", settings().value("shopRevenue", 0L)}, {"updates", updates}, {"top", top}, {"recent", recent}});
  });
  svr.Get("/api/admin/log", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    json out = json::array(); json lg = db.value("log", json::array()); for (size_t i = lg.size(); i-- > 0 && out.size() < 1200;) out.push_back(lg[i]); reply(r, 200, out);
  });
  svr.Post(R"(/api/admin/games/(\d+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (!isAdmin(n)) return reply(r, 403, err("Admins only"));
    std::string id = q.matches[1]; if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    auto& g = db["games"][id]; std::string t = g["title"];
    if (b.contains("featured")) { bool f = b.value("featured", false); if (f) for (auto& [k, x] : db["games"].items()) x["featured"] = false; g["featured"] = f; logAct(n, f ? "feature" : "unfeature", t); }
    if (b.contains("status")) { std::string st = b.value("status", ""); if (st != "approved" && st != "hidden") return reply(r, 400, err("Bad status"));
      g["status"] = st; if (st == "hidden") g["featured"] = false; logAct(n, st == "hidden" ? "unpublish" : "republish", t); }
    if (b.contains("price")) { g["price"] = std::clamp(std::lround(b.value("price", 0.0)), 0L, 1000000L); g["sale"] = std::clamp(std::lround(b.value("sale", 0.0)), 0L, 1000000L); logAct(n, "price", t); }
    save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post("/api/admin/username", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (!isAdmin(n)) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string ok = lower(b.value("username", "")), nn = b.value("newName", ""), nk = lower(nn);
    if (!db["users"].contains(ok)) return reply(r, 404, err("No such user"));
    if (!std::regex_match(nn, std::regex("[A-Za-z0-9_]{3,24}"))) return reply(r, 400, err("Usernames are 3-24 letters, numbers or _"));
    if (nk != ok && db["users"].contains(nk)) return reply(r, 409, err("That username is taken"));
    std::string old = db["users"][ok]["name"]; json u = db["users"][ok]; u["name"] = nn;
    db["users"].erase(ok); db["users"][nk] = u;
    for (auto& [k, g] : db["games"].items()) if (g.value("owner", "") == ok) { g["owner"] = nk; g["dev"] = nn; }
    for (auto& [t, who] : sessions) if (who == ok) who = nk;
    for (auto& [id, up] : uploads) if (up.owner == ok) up.owner = nk;
    for (auto& [k, x] : db["users"].items()) for (auto lst : {"following", "friends", "friendReq"}) if (inList(x, lst, ok)) { dropFrom(x, lst, ok); addTo(x, lst, nk); }
    if (db.contains("comments")) for (auto& [gid, list] : db["comments"].items()) for (auto& c : list) if (c.value("user", "") == ok) c["user"] = nk;
    logAct(n == ok ? nk : n, "rename", "@" + old + " is now @" + nn); save(); reply(r, 200, {{"ok", true}, {"key", nk}});
  });
  svr.Post("/api/admin/ban", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (!isAdmin(n)) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string t = lower(b.value("username", "")); bool ban = b.value("ban", true);
    if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (isAdmin(t)) return reply(r, 400, err("Admins can't be banned"));
    auto& u = db["users"][t]; u["banned"] = ban; u["banReason"] = ban ? b.value("reason", "") : "";
    if (ban) { for (auto it = sessions.begin(); it != sessions.end();) it = it->second == t ? sessions.erase(it) : std::next(it);
      for (auto& [k, g] : db["games"].items()) if (g.value("owner", "") == t && g.value("status", "") == "approved") { g["status"] = "hidden"; g["featured"] = false; } }
    logAct(n, ban ? "ban" : "unban", u["name"].get<std::string>() + (ban && u["banReason"] != "" ? " (" + u["banReason"].get<std::string>() + ")" : "")); save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post("/api/admin/settings", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (!isAdmin(n)) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    auto& st = settings();
    if (b.contains("announcement")) { std::string a = b.value("announcement", ""); if (a.size() > 300) a.resize(300); st["announcement"] = a; logAct(n, "announcement", a.empty() ? "cleared" : a); }
    save(); reply(r, 200, {{"ok", true}});
  });
  svr.Get("/api/admin/settings", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only")); reply(r, 200, settings());
  });
  svr.Get("/api/shop", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); json owned = json::array(), eq = json::object(); long bal = 0;
    if (!n.empty()) { auto& u = db["users"][n]; owned = u.value("items", json::array()); eq = u.value("equipped", json::object()); bal = u["balance"]; }
    json items = json::array(); bool adm = isAdmin(n); for (auto& i : CATALOG) if (!i.value("owner", false) || adm) items.push_back(i);
    reply(r, 200, {{"items", items}, {"owned", owned}, {"equipped", eq}, {"balance", bal}});
  });
  svr.Post("/api/shop/buy", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (n.empty()) return reply(r, 401, err("Log in to shop"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    auto it = item(b.value("item", "")); if (!it) return reply(r, 404, err("No such item"));
    if ((*it).value("owner", false)) return reply(r, 403, err("Owner items can't be bought"));
    auto& u = db["users"][n]; if (!u.contains("items")) u["items"] = json::array();
    for (auto& x : u["items"]) if (x == (*it)["id"]) return reply(r, 400, err("You already own this"));
    long p = (*it)["price"]; if (u["balance"].get<long>() < p) return reply(r, 402, err("Not enough credits. You need " + std::to_string(p) + " and have " + std::to_string(u["balance"].get<long>()) + "."));
    u["balance"] = u["balance"].get<long>() - p; u["items"].push_back((*it)["id"]);
    if (!u.contains("equipped")) u["equipped"] = json::object(); u["equipped"][(*it)["slot"].get<std::string>()] = (*it)["id"];
    settings()["shopRevenue"] = settings().value("shopRevenue", 0L) + p;
    logAct(n, "shop", (*it)["name"].get<std::string>() + " for " + std::to_string(p) + " credits"); save(); reply(r, 200, {{"ok", true}, {"balance", u["balance"]}});
  });
  svr.Post("/api/shop/equip", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (n.empty()) return reply(r, 401, err("Log in first"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string slot = b.value("slot", ""), id = b.value("item", ""); auto& u = db["users"][n];
    if (slot != "frame" && slot != "theme" && slot != "name" && slot != "effect") return reply(r, 400, err("Bad slot"));
    if (!u.contains("equipped")) u["equipped"] = json::object();
    if (id.empty()) { u["equipped"].erase(slot); save(); return reply(r, 200, {{"ok", true}}); }
    auto it = item(id); if (!it || (*it)["slot"] != slot) return reply(r, 400, err("That item doesn't go there"));
    bool own = false; for (auto& x : u.value("items", json::array())) if (x == id) own = true;
    if ((*it).value("owner", false)) { if (!isAdmin(n)) return reply(r, 403, err("Only the owner can wear this")); }
    else if (!own) return reply(r, 403, err("Buy this item first"));
    u["equipped"][slot] = id; save(); reply(r, 200, {{"ok", true}});
  });
  // ---- follow
  svr.Post(R"(/api/users/(\w+)/(follow|unfollow))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string t = lower(q.matches[1]);
    if (n.empty()) return reply(r, 401, err("Log in first")); if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (t == n) return reply(r, 400, err("You can't follow yourself"));
    auto& u = db["users"][n]; if (q.matches[2] == "follow") addTo(u, "following", t); else dropFrom(u, "following", t);
    save(); reply(r, 200, {{"ok", true}});
  });
  svr.Get(R"(/api/users/(\w+)/(followers|following|friends))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); std::string t = lower(q.matches[1]), kind = q.matches[2];
    if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    json out = json::array();
    if (kind == "followers") { for (auto& [k, x] : db["users"].items()) if (inList(x, "following", t)) out.push_back(summary(k)); }
    else for (auto& k : db["users"][t].value(kind == "following" ? "following" : "friends", json::array())) if (db["users"].contains(k.get<std::string>())) out.push_back(summary(k));
    reply(r, 200, out);
  });
  // ---- friends (requests must be accepted)
  svr.Get("/api/friends", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (n.empty()) return reply(r, 401, err("Log in first"));
    auto& u = db["users"][n]; json fr = json::array(), in = json::array(), outq = json::array();
    for (auto& k : u.value("friends", json::array())) if (db["users"].contains(k.get<std::string>())) fr.push_back(summary(k));
    for (auto& k : u.value("friendReq", json::array())) if (db["users"].contains(k.get<std::string>())) in.push_back(summary(k));
    for (auto& [k, x] : db["users"].items()) if (inList(x, "friendReq", n)) outq.push_back(summary(k));
    reply(r, 200, {{"friends", fr}, {"incoming", in}, {"outgoing", outq}});
  });
  svr.Post(R"(/api/friends/(\w+)/(request|accept|decline|remove))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string t = lower(q.matches[1]), act = q.matches[2];
    if (n.empty()) return reply(r, 401, err("Log in first")); if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (t == n) return reply(r, 400, err("You can't friend yourself"));
    auto& me = db["users"][n]; auto& them = db["users"][t]; std::string state;
    if (act == "request") {
      if (inList(me, "friends", t)) return reply(r, 200, {{"state", "friends"}});
      if (inList(me, "friendReq", t)) act = "accept"; // they already asked you: accept
      else { if (them.value("friendReq", json::array()).size() >= 200) return reply(r, 429, err("They have too many pending requests"));
        addTo(them, "friendReq", n); state = "sent"; }
    }
    if (act == "accept") { if (!inList(me, "friendReq", t)) return reply(r, 400, err("No friend request from them"));
      dropFrom(me, "friendReq", t); addTo(me, "friends", t); addTo(them, "friends", n); dropFrom(them, "friendReq", n); state = "friends"; }
    if (act == "decline") { dropFrom(me, "friendReq", t); state = "none"; }
    if (act == "remove") { dropFrom(me, "friends", t); dropFrom(them, "friends", n); dropFrom(them, "friendReq", n); state = "none"; }
    save(); reply(r, 200, {{"state", state}});
  });
  // ---- comments on games
  static std::map<std::string, long> lastComment;
  svr.Get(R"(/api/games/(\d+)/comments)", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); std::string id = q.matches[1]; json out = json::array();
    if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    std::string owner = db["games"][id].value("owner", "");
    for (auto& c : db.value("comments", json::object()).value(id, json::array())) { std::string k = c.value("user", "");
      if (!db["users"].contains(k) || db["users"][k].value("banned", false)) continue;
      json o = c; o["author"] = summary(k); o["isDev"] = k == owner; o["owns"] = owns(db["users"][k], std::stoi(id)); out.push_back(o); }
    std::reverse(out.begin(), out.end()); reply(r, 200, out);
  });
  svr.Post(R"(/api/games/(\d+)/comments)", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    if (n.empty()) return reply(r, 401, err("Log in to comment")); if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    if (db["games"][id].value("status", "approved") != "approved") return reply(r, 400, err("You can comment once the game is in the store"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string t; for (unsigned char c : b.value("text", std::string())) if (c >= 32 || c == '\n') t += (char)c;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back(); while (!t.empty() && (t[0] == ' ' || t[0] == '\n')) t.erase(0, 1);
    if (t.empty()) return reply(r, 400, err("Write something first")); if (t.size() > 1000) return reply(r, 400, err("Comments can be at most 1000 characters"));
    long now = (long)time(nullptr); if (now - lastComment[n] < 5) return reply(r, 429, err("Slow down: wait a few seconds between comments")); lastComment[n] = now;
    if (!db.contains("comments")) db["comments"] = json::object(); if (!db["comments"].contains(id)) db["comments"][id] = json::array();
    std::string cid = randHex(8); db["comments"][id].push_back({{"id", cid}, {"user", n}, {"text", t}, {"at", now}});
    save(); reply(r, 200, {{"ok", true}, {"id", cid}});
  });
  svr.Delete(R"(/api/games/(\d+)/comments/([0-9a-f]+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1], cid = q.matches[2];
    if (n.empty()) return reply(r, 401, err("Log in first"));
    if (!db.contains("comments") || !db["comments"].contains(id)) return reply(r, 404, err("Comment not found"));
    auto& list = db["comments"][id];
    for (size_t i = 0; i < list.size(); i++) if (list[i]["id"] == cid) {
      std::string by = list[i]["user"]; if (by != n && !isMod(n)) return reply(r, 403, err("You can only delete your own comments"));
      if (by != n) logAct(n, "delete comment", "by @" + db["users"][by]["name"].get<std::string>() + " on " + db["games"][id].value("title", std::string("a game")));
      list.erase(list.begin() + (long)i); save(); return reply(r, 200, {{"ok", true}}); }
    reply(r, 404, err("Comment not found"));
  });
  // Serve ./index.html if present (GitHub layout), otherwise the ./web folder.
  if (fs::exists("index.html")) svr.Get("/", [&](const httplib::Request&, httplib::Response& r) { std::ifstream f("index.html"); std::string d((std::istreambuf_iterator<char>(f)), {}); r.set_header("Cache-Control", "no-cache"); r.set_content(d, "text/html"); });
  else svr.set_mount_point("/", "./web");
  int port = getenv("PORT") ? atoi(getenv("PORT")) : 8080;
  printf("Vespera running on http://localhost:%d\n", port);
  svr.listen("0.0.0.0", port);
}
