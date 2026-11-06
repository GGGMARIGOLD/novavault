// NovaVault - a Steam-style game store server in C++17.
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
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower(c); return s; }
static void reply(httplib::Response& r, int code, const json& j) { r.status = code; r.set_content(j.dump(), "application/json"); }
static json err(const std::string& m) { return {{"error", m}}; }
static long eff(const json& g) { long p = g.value("price", 0L), s = g.value("sale", 0L); return (s > 0 && s < p) ? s : p; }
static json pub(const json& g) { json o = g; o.erase("file"); o.erase("cover"); o["status"] = g.value("status", "approved"); o["hasCover"] = g.contains("cover") && g["cover"] != ""; return o; }

// Returns the logged-in username or "" (caller holds mu).
static std::string whoami(const httplib::Request& q) {
  std::string c = q.get_header_value("Cookie"); auto p = c.find("sid=");
  if (p == std::string::npos) return "";
  auto e = c.find(';', p); auto t = c.substr(p + 4, e == std::string::npos ? e : e - p - 4);
  auto it = sessions.find(t); return it == sessions.end() ? "" : it->second;
}
static std::string roleOf(const std::string& n) { return n.empty() ? "" : db["users"][n].value("role", "user"); }
static bool isMod(const std::string& n) { auto r = roleOf(n); return r == "admin" || r == "mod"; }
static bool isAdmin(const std::string& n) { return roleOf(n) == "admin"; }
static json earned(const std::string& n) {
  json b = json::array(); auto& u = db["users"][n]; std::string r = u.value("role", "user");
  if (r == "admin") b.push_back("Admin"); if (r == "mod") b.push_back("Moderator");
  json gr = u.value("granted", json::array()); for (auto& x : gr) b.push_back(x);
  bool dev = false; for (auto& [k, g] : db["games"].items()) if (g["owner"] == n && g.value("status", "approved") == "approved") dev = true;
  if (dev) b.push_back("Developer"); if (u["library"].size() >= 10) b.push_back("Collector");
  return b;
}
static json shownBadges(const std::string& n) {
  auto e = earned(n); json out = json::array(); json sh = db["users"][n].value("shown", json::array());
  for (auto& x : sh) if (std::find(e.begin(), e.end(), x) != e.end() && out.size() < 3) out.push_back(x);
  if (out.empty()) for (auto& x : e) if (out.size() < 3) out.push_back(x);
  return out;
}
static std::string mimeOf(const std::string& p) { auto e = p.substr(p.find_last_of('.') + 1); return e == "png" ? "image/png" : e == "webp" ? "image/webp" : "image/jpeg"; }
static bool owns(const json& u, int id) { for (auto& x : u["library"]) if (x == id) return true; return false; }
static std::string safeName(std::string n) { for (auto& c : n) if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') c = '_'; return n.empty() ? "game.bin" : n; }
static long cents(const std::string& s) { try { return std::max(0L, std::lround(std::stod(s) * 100)); } catch (...) { return 0; } }

int main() {
  fs::create_directories("data/files");
  if (fs::exists("data/db.json")) { std::ifstream f("data/db.json"); db = json::parse(f, nullptr, false); if (db.is_discarded()) { fprintf(stderr, "db.json is corrupt\n"); return 1; } }
  if (const char* a = getenv("ADMIN_USER")) { auto k = lower(a); if (db["users"].contains(k)) { db["users"][k]["role"] = "admin"; save(); printf("%s is an admin\n", a); } }
  const bool demoFunds = !getenv("NO_DEMO_FUNDS"); // test money; disable once a real payment provider is wired in
  httplib::Server svr;
  svr.set_payload_max_length(2ULL << 30); // 2 GB uploads (held in memory while uploading)

  svr.Post("/api/register", [&](const httplib::Request& q, httplib::Response& r) {
    auto b = json::parse(q.body, nullptr, false); std::lock_guard<std::mutex> l(mu);
    if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string n = b.value("username", ""), pw = b.value("password", "");
    if (!std::regex_match(n, std::regex("[A-Za-z0-9_]{3,24}"))) return reply(r, 400, err("Username: 3-24 letters, numbers or _"));
    if (pw.size() < 8) return reply(r, 400, err("Password must be at least 8 characters"));
    if (db["users"].contains(lower(n))) return reply(r, 409, err("That username is taken"));
    std::string salt = randHex(16); std::string role = db["users"].empty() ? "admin" : "user";
    db["users"][lower(n)] = {{"role", role}, {"joined", (long)time(nullptr)}, {"name", n}, {"salt", salt}, {"hash", hashPw(pw, salt)}, {"balance", 0}, {"library", json::array()}};
    save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post("/api/login", [&](const httplib::Request& q, httplib::Response& r) {
    auto b = json::parse(q.body, nullptr, false); std::lock_guard<std::mutex> l(mu);
    if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string n = lower(b.value("username", ""));
    if (!db["users"].contains(n)) return reply(r, 401, err("Wrong username or password"));
    auto& u = db["users"][n];
    if (hashPw(b.value("password", ""), u["salt"]) != u["hash"]) return reply(r, 401, err("Wrong username or password"));
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
    if (n.empty()) return reply(r, 200, {{"user", nullptr}, {"demoFunds", demoFunds}});
    auto& u = db["users"][n]; reply(r, 200, {{"user", {{"name", u["name"]}, {"balance", u["balance"]}, {"library", u["library"]}, {"role", roleOf(n)}}}, {"demoFunds", demoFunds}});
  });
  svr.Post("/api/wallet/add", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q);
    if (n.empty()) return reply(r, 401, err("Log in first"));
    if (!demoFunds) return reply(r, 403, err("Test funds are disabled"));
    auto& u = db["users"][n]; if (u["balance"].get<long>() >= 10000) return reply(r, 400, err("Wallet is already full of test funds"));
    u["balance"] = u["balance"].get<long>() + 2000; save(); reply(r, 200, {{"balance", u["balance"]}});
  });
  svr.Get(R"(/api/profile/(\w+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); std::string n = lower(q.matches[1]), me = whoami(q);
    if (!db["users"].contains(n)) return reply(r, 404, err("No such user"));
    auto& u = db["users"][n]; std::string pr = u.value("presence", "online"); json games = json::array();
    for (auto& [k, g] : db["games"].items()) if (g["owner"] == n && g.value("status", "approved") == "approved") games.push_back(pub(g));
    json p = {{"name", u["name"]}, {"key", n}, {"role", u.value("role", "user")}, {"bio", u.value("bio", "")}, {"statusText", u.value("statusText", "")},
      {"presence", (pr == "invisible" && me != n) ? "offline" : pr}, {"badges", shownBadges(n)}, {"hasAvatar", u.value("avatar", "") != ""}, {"hasBanner", u.value("banner", "") != ""},
      {"joined", u.value("joined", 0L)}, {"games", games}, {"owned", u["library"].size()}};
    if (me == n) { p["earned"] = earned(n); p["shown"] = shownBadges(n); }
    reply(r, 200, p);
  });
  svr.Post("/api/profile", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); if (n.empty()) return reply(r, 401, err("Log in first"));
    auto& u = db["users"][n]; auto F = [&](const char* k) { return q.get_file_value(k).content; };
    if (q.has_file("bio")) { auto t = F("bio"); if (t.size() > 500) t.resize(500); u["bio"] = t; }
    if (q.has_file("statusText")) { auto t = F("statusText"); if (t.size() > 80) t.resize(80); u["statusText"] = t; }
    if (q.has_file("presence")) { auto p = F("presence"); if (p == "online" || p == "away" || p == "busy" || p == "invisible") u["presence"] = p; }
    if (q.has_file("shown")) { json sh = json::array(); std::stringstream ss(F("shown")); std::string t; auto e = earned(n);
      while (std::getline(ss, t, '|')) if (std::find(e.begin(), e.end(), t) != e.end() && sh.size() < 3) sh.push_back(t); u["shown"] = sh; }
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
    if (bd != "Founder" && bd != "Beta Tester" && bd != "Verified Dev" && bd != "Supporter") return reply(r, 400, err("Unknown badge"));
    json out = json::array(); json cur = db["users"][t].value("granted", json::array()); for (auto& x : cur) if (x != bd) out.push_back(x);
    if (give) out.push_back(bd); db["users"][t]["granted"] = out; save(); reply(r, 200, {{"ok", true}});
  });
  svr.Get("/api/games", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto me = whoami(q); bool mod = isMod(me); json a = json::array();
    for (auto& [k, g] : db["games"].items()) if (g.value("status", "approved") == "approved" || mod || g["owner"] == me) a.push_back(pub(g));
    std::sort(a.begin(), a.end(), [](const json& x, const json& y) { return x["at"].get<long>() > y["at"].get<long>(); });
    reply(r, 200, a);
  });
  svr.Post("/api/games", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q);
    if (n.empty()) return reply(r, 401, err("Log in to publish"));
    if (!q.has_file("file")) return reply(r, 400, err("Choose a game or APK file"));
    auto f = q.get_file_value("file"); std::string title = q.get_file_value("title").content;
    if (title.empty() || f.content.empty()) return reply(r, 400, err("Title and file are required"));
    int id = db["next"]; db["next"] = id + 1;
    std::string stored = randHex(12); { std::ofstream o("data/files/" + stored, std::ios::binary); o.write(f.content.data(), (std::streamsize)f.content.size()); }
    std::string cover;
    if (q.has_file("cover")) {
      auto c = q.get_file_value("cover"); std::string ext = c.content_type == "image/png" ? ".png" : c.content_type == "image/jpeg" ? ".jpg" : c.content_type == "image/webp" ? ".webp" : "";
      if (!ext.empty() && c.content.size() < (8u << 20)) { cover = randHex(12) + ext; std::ofstream("data/files/" + cover, std::ios::binary).write(c.content.data(), (std::streamsize)c.content.size()); }
    }
    auto F = [&](const char* k) { return q.get_file_value(k).content; };
    db["games"][std::to_string(id)] = {{"id", id}, {"title", title}, {"dev", db["users"][n]["name"]}, {"owner", n}, {"desc", F("desc")}, {"genre", F("genre")},
      {"price", cents(F("price"))}, {"sale", cents(F("sale"))}, {"platform", F("platform")}, {"version", F("version")}, {"os", F("os")}, {"ram", F("ram")}, {"storage", F("storage")},
      {"size", f.content.size()}, {"filename", safeName(f.filename)}, {"file", stored}, {"cover", cover}, {"at", (long)time(nullptr)}, {"status", isMod(n) ? "approved" : "pending"}};
    save(); reply(r, 200, {{"id", id}});
  });
  svr.Delete(R"(/api/games/(\d+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto g = db["games"][id]; if (g["owner"] != n) return reply(r, 403, err("Only the developer can remove this game"));
    fs::remove("data/files/" + g["file"].get<std::string>()); if (g["cover"] != "") fs::remove("data/files/" + g["cover"].get<std::string>());
    db["games"].erase(id); save(); reply(r, 200, {{"ok", true}});
  });
  svr.Post(R"(/api/games/(\d+)/buy)", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
    if (n.empty()) return reply(r, 401, err("Log in to get games"));
    if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto& g = db["games"][id]; auto& u = db["users"][n]; int gid = g["id"];
    if (g.value("status", "approved") != "approved") return reply(r, 403, err("This game is not approved yet"));
    if (owns(u, gid)) return reply(r, 200, {{"ok", true}});
    long p = eff(g);
    if (u["balance"].get<long>() < p) return reply(r, 402, err("Not enough wallet funds"));
    u["balance"] = u["balance"].get<long>() - p;
    if (p > 0 && g["owner"] != n) { auto& d = db["users"][g["owner"].get<std::string>()]; d["balance"] = d["balance"].get<long>() + p; } // developer payout
    u["library"].push_back(gid); save(); reply(r, 200, {{"ok", true}, {"balance", u["balance"]}});
  });
  svr.Get(R"(/download/(\d+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::string path, name; size_t size = 0;
    { std::lock_guard<std::mutex> l(mu); auto n = whoami(q); std::string id = q.matches[1];
      if (n.empty()) return reply(r, 401, err("Log in first"));
      if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
      auto& g = db["games"][id]; if (!owns(db["users"][n], g["id"]) && g["owner"] != n && !isMod(n)) return reply(r, 403, err("Add this game to your library first"));
      path = "data/files/" + g["file"].get<std::string>(); name = g["filename"]; size = g["size"]; }
    r.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
    r.set_content_provider(size, "application/octet-stream", [path](size_t off, size_t len, httplib::DataSink& s) {
      std::ifstream f(path, std::ios::binary); f.seekg((std::streamoff)off); std::vector<char> b(std::min(len, (size_t)1 << 20)); f.read(b.data(), (std::streamsize)b.size());
      return s.write(b.data(), (size_t)f.gcount());
    });
  });
  svr.Get(R"(/cover/(\d+))", [&](const httplib::Request& q, httplib::Response& r) {
    std::string path; { std::lock_guard<std::mutex> l(mu); std::string id = q.matches[1];
      if (!db["games"].contains(id) || db["games"][id]["cover"] == "") { r.status = 404; return; } path = "data/files/" + db["games"][id]["cover"].get<std::string>(); }
    std::ifstream f(path, std::ios::binary); std::string d((std::istreambuf_iterator<char>(f)), {});
    r.set_content(d, path.size() > 4 && path.substr(path.size() - 4) == ".png" ? "image/png" : path.back() == 'p' ? "image/webp" : "image/jpeg");
  });
  auto setStatus = [&](const httplib::Request& q, httplib::Response& r, const char* st) {
    std::lock_guard<std::mutex> l(mu); auto n = whoami(q);
    if (!isMod(n)) return reply(r, 403, err("Moderators only"));
    std::string id = q.matches[1]; if (!db["games"].contains(id)) return reply(r, 404, err("Not found"));
    auto& g = db["games"][id]; g["status"] = st; auto b = json::parse(q.body, nullptr, false);
    g["reason"] = (b.is_object() && std::string(st) == "rejected") ? b.value("reason", "") : "";
    save(); reply(r, 200, {{"ok", true}});
  };
  svr.Post(R"(/api/mod/games/(\d+)/approve)", [&](const httplib::Request& q, httplib::Response& r) { setStatus(q, r, "approved"); });
  svr.Post(R"(/api/mod/games/(\d+)/reject)", [&](const httplib::Request& q, httplib::Response& r) { setStatus(q, r, "rejected"); });
  svr.Get("/api/admin/users", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    json a = json::array(); for (auto& [k, u] : db["users"].items()) a.push_back({{"name", u["name"]}, {"role", u.value("role", "user")}, {"balance", u["balance"]}});
    reply(r, 200, a);
  });
  svr.Post("/api/admin/credits", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string t = lower(b.value("username", "")); double amt = b.value("amount", 0.0);
    if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (!(std::abs(amt) <= 100000)) return reply(r, 400, err("Amount must be between -100000 and 100000"));
    auto& u = db["users"][t]; u["balance"] = std::max(0L, u["balance"].get<long>() + std::lround(amt * 100)); save(); reply(r, 200, {{"balance", u["balance"]}});
  });
  svr.Post("/api/admin/role", [&](const httplib::Request& q, httplib::Response& r) {
    std::lock_guard<std::mutex> l(mu); if (!isAdmin(whoami(q))) return reply(r, 403, err("Admins only"));
    auto b = json::parse(q.body, nullptr, false); if (!b.is_object()) return reply(r, 400, err("Bad request"));
    std::string t = lower(b.value("username", "")), role = b.value("role", "");
    if (!db["users"].contains(t)) return reply(r, 404, err("No such user"));
    if (role != "user" && role != "mod") return reply(r, 400, err("Role must be user or mod"));
    if (isAdmin(t)) return reply(r, 400, err("Admin role can't be changed here"));
    db["users"][t]["role"] = role; save(); reply(r, 200, {{"ok", true}});
  });
  // Serve ./index.html if present (GitHub layout), otherwise the ./web folder.
  if (fs::exists("index.html")) svr.Get("/", [&](const httplib::Request&, httplib::Response& r) { std::ifstream f("index.html"); std::string d((std::istreambuf_iterator<char>(f)), {}); r.set_header("Cache-Control", "no-cache"); r.set_content(d, "text/html"); });
  else svr.set_mount_point("/", "./web");
  int port = getenv("PORT") ? atoi(getenv("PORT")) : 8080;
  printf("NovaVault running on http://localhost:%d\n", port);
  svr.listen("0.0.0.0", port);
}
