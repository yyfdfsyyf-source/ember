#pragma once
#include "agent/types.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace agent {

// Local disk cache (see 建议/06-本地缓存实现计划.md, V1).
// One file per key in a single directory; zero dependencies, atomic writes
// (tmp + rename), TTL per entry and an LRU sweep capped by total bytes.
// Two consumers:
//   - LLM response cache (Client::chat): key = hashRequest(messages, opts)
//   - tool result cache (ToolRegistry::run): key = hashTool(name, argsJson)
class DiskCache {
 public:
  // maxBytes ~200MB default; 0 disables the sweep. ttlSec is the default
  // entry TTL (0 = use the 7-day default).
  explicit DiskCache(std::string dir, size_t maxBytes = 200ull * 1024 * 1024,
                     int64_t ttlSec = 7 * 24 * 3600);

  // Hit (fresh) -> true and fills `out`; miss/expired/corrupt -> false
  // (expired files are deleted). Never throws.
  bool get(std::string const& key, std::string& out);
  // Store `value` under `key` with `ttlSeconds` (0 = cache default).
  // Values > 2MB are skipped. Never throws.
  void put(std::string const& key, std::string const& value, int64_t ttlSeconds = 0);

  // FNV-1a 64 over UTF-8 bytes, as 16 lowercase hex chars.
  static std::string fnv1a64(std::string const& data);
  // Stable key for a chat request: model + temperature + maxTokens +
  // jsonMode + systemPrompt + toolsJson + the full message sequence.
  // Changing any of them changes the key.
  static std::string hashRequest(std::vector<Message> const& msgs, ChatOptions const& opts);
  // Stable key for a tool call: name + exact arguments JSON.
  static std::string hashTool(std::string const& name, std::string const& argsJson);

  // Resolve the default cache directory: $AGENT_CACHE_DIR ->
  // $XDG_CACHE_HOME/ember/cache -> %LOCALAPPDATA%\Ember\cache ->
  // %USERPROFILE%\.ember\cache -> ~/.cache/ember -> "cache".
  static std::string resolveDir(std::string const& overrideDir = "");

  std::string const& dir() const { return dir_; }
  int64_t hits() const { return hits_.load(); }
  int64_t stores() const { return stores_.load(); }

 private:
  std::string pathFor(std::string const& key) const;
  void sweepLocked();  // delete oldest files until total size <= maxBytes_

  std::string dir_;
  size_t maxBytes_;
  int64_t ttlSec_;
  mutable std::mutex mu_;
  std::atomic<int64_t> hits_{0};
  std::atomic<int64_t> stores_{0};
};

}  // namespace agent
