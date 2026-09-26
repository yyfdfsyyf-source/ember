#include "agent/cache.hpp"
#include "minijson.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace agent {

namespace {

constexpr int64_t kMaxValueBytes = 2 * 1024 * 1024;  // skip very large values

int64_t nowSec() {
  return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

DiskCache::DiskCache(std::string dir, size_t maxBytes, int64_t ttlSec)
    : dir_(std::move(dir)),
      maxBytes_(maxBytes),
      ttlSec_(ttlSec > 0 ? ttlSec : 7 * 24 * 3600) {
  std::error_code ec;
  fs::create_directories(dir_, ec);  // best effort; get/put fail silently
}

std::string DiskCache::pathFor(std::string const& key) const {
  return dir_ + "/" + key + ".json";
}

bool DiskCache::get(std::string const& key, std::string& out) {
  std::lock_guard<std::mutex> lk(mu_);
  std::ifstream in(pathFor(key), std::ios::binary);
  if (!in) return false;
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  mini::Value v;
  if (!mini::tryParse(raw, v) || v.type != mini::Value::Object) {
    std::error_code ec;
    fs::remove(pathFor(key), ec);  // corrupt: drop it
    return false;
  }
  int64_t stored = v.get("stored") ? v.get("stored")->asInt() : 0;
  int64_t ttl = v.get("ttl") ? v.get("ttl")->asInt() : ttlSec_;
  if (ttl <= 0 || nowSec() - stored >= ttl) {
    std::error_code ec;
    fs::remove(pathFor(key), ec);  // expired
    return false;
  }
  mini::Value const* val = v.get("value");
  if (!val || val->type != mini::Value::String) return false;
  out = val->s;
  hits_++;
  return true;
}

void DiskCache::put(std::string const& key, std::string const& value, int64_t ttlSeconds) {
  if (value.size() > (size_t)kMaxValueBytes) return;
  std::lock_guard<std::mutex> lk(mu_);

  mini::Value v = mini::Value::makeObject();
  v.set("key", mini::Value::makeString(key));
  v.set("stored", mini::Value::makeInt(nowSec()));
  v.set("ttl", mini::Value::makeInt(ttlSeconds > 0 ? ttlSeconds : ttlSec_));
  v.set("value", mini::Value::makeString(value));
  std::string raw = mini::dump(v);

  std::string path = pathFor(key);
  std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << raw;
    out.flush();
    if (!out) return;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    // Windows cannot rename over an existing target; fall back to remove+rename.
    fs::remove(path, ec);
    ec.clear();
    fs::rename(tmp, path, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return;
    }
  }
  stores_++;
  sweepLocked();
}

void DiskCache::sweepLocked() {
  if (maxBytes_ == 0) return;
  std::error_code ec;
  uint64_t total = 0;
  std::vector<std::pair<fs::file_time_type, fs::path>> entries;
  for (auto const& de : fs::directory_iterator(dir_, ec)) {
    if (!de.is_regular_file()) continue;
    if (de.path().extension() != ".json") continue;
    total += de.file_size(ec);
    entries.emplace_back(fs::last_write_time(de.path(), ec), de.path());
  }
  if (total <= maxBytes_) return;
  std::sort(entries.begin(), entries.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
  for (auto const& [t, p] : entries) {
    if (total <= maxBytes_) break;
    uint64_t sz = fs::file_size(p, ec);
    fs::remove(p, ec);
    if (!ec && sz <= total) total -= sz;
  }
}

std::string DiskCache::fnv1a64(std::string const& data) {
  uint64_t h = 14695981039346656037ull;  // offset basis
  for (unsigned char c : data) {
    h ^= c;
    h *= 1099511628211ull;  // FNV prime
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
  return buf;
}

std::string DiskCache::hashRequest(std::vector<Message> const& msgs, ChatOptions const& opts) {
  std::string s;
  s.reserve(1024 + msgs.size() * 256);
  s += "v1\x01";
  s += opts.model;
  s += '\x01';
  char tbuf[32];
  std::snprintf(tbuf, sizeof tbuf, "%.6g", opts.temperature);
  s += tbuf;
  s += '\x01';
  s += std::to_string(opts.maxTokens);
  s += '\x01';
  s += opts.jsonMode ? "json1" : "json0";
  s += '\x01';
  s += opts.systemPrompt;
  s += '\x01';
  s += opts.toolsJson;
  for (auto const& m : msgs) {
    s += '\x02';
    s += m.role;
    s += '\x01';
    s += m.content;
    s += '\x01';
    s += m.toolCallId;
    for (auto const& tc : m.toolCalls) {
      s += '\x03';
      s += tc.id;
      s += '\x01';
      s += tc.name;
      s += '\x01';
      s += tc.arguments;
    }
    for (auto const& img : m.images) {
      // Full data URIs are megabytes; size + head is identity enough (they are
      // generated screenshots, never hand-edited in place).
      s += '\x04';
      s += std::to_string(img.size());
      s += ':';
      s += img.substr(0, 64);
    }
  }
  return "llm-" + fnv1a64(s);
}

std::string DiskCache::hashTool(std::string const& name, std::string const& argsJson) {
  return "tool-" + fnv1a64(name + '\x01' + argsJson);
}

std::string DiskCache::resolveDir(std::string const& overrideDir) {
  if (!overrideDir.empty()) return overrideDir;
  if (char const* e = std::getenv("AGENT_CACHE_DIR"); e && *e) return e;
  if (char const* x = std::getenv("XDG_CACHE_HOME"); x && *x)
    return std::string(x) + "/ember/cache";
  if (char const* l = std::getenv("LOCALAPPDATA"); l && *l)
    return std::string(l) + "\\Ember\\cache";
  if (char const* u = std::getenv("USERPROFILE"); u && *u)
    return std::string(u) + "\\.ember\\cache";
  if (char const* h = std::getenv("HOME"); h && *h)
    return std::string(h) + "/.cache/ember";
  return "cache";
}

}  // namespace agent
