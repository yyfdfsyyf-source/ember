#include "agent/rag.hpp"
#include "agent/tools.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                     \
  } while (0)

static bool has(std::string const& hay, std::string const& needle) {
  return hay.find(needle) != std::string::npos;
}

static void wr(std::string const& p, std::string const& c) {
  std::filesystem::create_directories(std::filesystem::path(p).parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << c;
}

int main() {
  using namespace agent;
  std::string dir = "out/tmp_rag_test";
  std::filesystem::remove_all(dir);

  // --- tokenizer ---------------------------------------------------------
  {
    std::vector<std::string> toks;
    rag::tokenize("  HTTP fetch 缓存 kids' Apples 中文测试 ", toks);
    bool hasHttp = false, hasFetch = false, hasKids = false;
    for (auto const& t : toks) {
      if (t == "http") hasHttp = true;
      if (t == "fetch") hasFetch = true;
      if (t == "kids") hasKids = true;
    }
    // CJK is indexed per character; two chars produce two tokens.
    bool cjk1 = false, cjk2 = false, cjk3 = false;
    for (auto const& t : toks) {
      if (t == "缓") cjk1 = true;
      if (t == "存") cjk2 = true;
      if (t == "中") cjk3 = true;
    }
    CHECK(hasHttp && hasFetch && hasKids);
    CHECK(cjk1 && cjk2 && cjk3);
  }

  // --- index + BM25 search ----------------------------------------------
  wr(dir + "/src/network/http_utils.cpp",
     "// HTTP client helpers.\nHttpClient::HttpClient() { socket = open_socket(); }\n");
  wr(dir + "/src/network/cache_store.cpp",
     "// 缓存存储实现。\nvoid CacheStore::store(key) { write disk entry; }\n");
  wr(dir + "/src/run/runner.cpp",
     "// Task runner.\nvoid Runner::run() { spawn threads; dispatch jobs; }\n");
  wr(dir + "/bin/junk.bin", std::string("binary\0data", 11));  // skipped

  rag::Index idx;
  CHECK(idx.build(dir).empty());
  CHECK(idx.fileCount() == 3);
  CHECK(idx.segmentCount() >= 3);
  CHECK(idx.root() == std::filesystem::path(dir).lexically_normal().string());

  std::vector<rag::Hit> hits;
  CHECK(idx.search("http client", 5, hits));
  CHECK(!hits.empty());
  CHECK(has(hits[0].path, "http_utils.cpp"));

  CHECK(idx.search("磁盘 缓存", 5, hits));  // CJK: 磁盘 doesn't exist -> 缓+存 match cache file
  bool cacheFirst = false;
  for (auto const& h : hits)
    if (h.path.find("cache_store") != std::string::npos) cacheFirst = true;
  CHECK(cacheFirst && !hits.empty());

  // --- mtime staleness ---------------------------------------------------
  wr(dir + "/src/run/runner.cpp",
     "// Task runner.\nvoid Runner::run() { spawn threads; dispatch jobs; doWork(); }\n");
  std::string s = idx.buildIfStale(dir);
  CHECK(s.empty());  // rebuilt because a file changed

  wr(dir + "/src/run/runner.cpp", "// Task runner.\nvoid Runner::run() {}\n");
  s = idx.buildIfStale(dir);
  CHECK(s.empty());
  s = idx.buildIfStale(dir);
  CHECK(s == "unchanged");  // second scan is a no-op

  // --- save/load round-trip ---------------------------------------------
  wr(dir + "/src/net2/extra.cpp", "// Extra.\nint extra() { return 42; }\n");
  s = idx.buildIfStale(dir);
  CHECK(s.empty());
  // The index file itself lives outside the tree (out/ is excluded when the
  // tree is the project root), so it is never scanned as a source file.
  std::string idxFile = dir + ".idx.json";
  CHECK(idx.save(idxFile).empty());
  rag::Index idx2;
  CHECK(idx2.load(idxFile).empty());
  CHECK(idx2.fileCount() == idx.fileCount());
  std::vector<rag::Hit> h2;
  CHECK(idx2.search("extra", 3, h2));
  CHECK(!h2.empty());
  CHECK(h2[0].path == "src/net2/extra.cpp");
  // Loading the saved index, then scanning the tree, reports no change.
  CHECK(idx2.buildIfStale(dir) == "unchanged");

  // --- tools -------------------------------------------------------------
  std::string r;
  r = toolRagSearch("{\"query\":\"\"}");
  CHECK(has(r, "query is required"));
  r = toolRagSearch("{\"query\":\"http\",\"path\":\"" + dir + "\"}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "http_utils.cpp"));
  r = toolRagIndex("{\"path\":\"" + dir + "\"}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "\"segments\""));
  r = toolRagSearch("{\"query\":\"runner\",\"path\":\"" + dir + "\"}");
  CHECK(has(r, "run/runner.cpp"));

  std::filesystem::remove_all(dir);
  std::printf("checks=%d failures=%d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}