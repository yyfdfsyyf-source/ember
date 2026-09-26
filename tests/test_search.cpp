// Tests for the grep/glob search primitives and tools.
#include "agent/search.hpp"
#include "agent/tools.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

static void write(std::string const& path, std::string const& content) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << content;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::string dir = "out/tmp_search_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  write(dir + "/src/app.cpp", "int main() { /* app */ return 0; }\n// TODO: fix this\n");
  write(dir + "/src/util.cpp", "void util() { /* Files opened */ }\n");
  write(dir + "/include/app.h", "#pragma once\nclass app {};\n");
  write(dir + "/docs/readme.md", "# Title\n\nSee the code for details.\n");
  std::filesystem::create_directories(dir + "/.git");
  write(dir + "/.git/config", "secret hash=abc123\n");
  write(dir + "/out/debug.log", "verbose noise\n");
  std::filesystem::create_directories(dir + "/bin");
  write(dir + "/bin/blob.bin", std::string("app") + char(0) + "bytes\n");

  // --- globMatch unit behaviour ---
  CHECK(agent::globMatch("src/*.cpp", "src/app.cpp"));
  CHECK(!agent::globMatch("src/*.cpp", "include/app.h"));
  CHECK(agent::globMatch("src/**/*.cpp", "src/deep/nested/x.cpp"));
  CHECK(agent::globMatch("**/*.md", "docs/readme.md"));
  CHECK(agent::globMatch("**/*.md", "readme.md"));
  CHECK(agent::globMatch("a?c.txt", "abc.txt"));
  CHECK(!agent::globMatch("a?c.txt", "abbc.txt"));
  CHECK(agent::globMatch("**", "any/depth/file.txt"));

  // --- globFiles: relative path listing with exclusions ---
  {
    std::string err;
    auto files = agent::globFiles(dir, "**/*.cpp", 100, &err);
    CHECK(err.empty());
    CHECK(files.size() == 2);  // src/app.cpp + src/util.cpp
    bool found = false;
    for (auto const& f : files)
      if (f == "src/app.cpp") found = true;
    CHECK(found);
  }
  // Hidden dirs and build outputs are excluded.
  {
    std::string err;
    auto files = agent::globFiles(dir, "**", 1000, &err);
    CHECK(!has(files.size() == 0 ? "" : "x", ".git"));
    for (auto const& f : files) {
      CHECK(!has(f, ".git"));
      CHECK(!has(f, "bin/blob.bin"));
      CHECK(!has(f, "out/debug.log"));
    }
    bool hasLog = false;
    for (auto const& f : files)
      if (f == "out/debug.log") hasLog = true;
    CHECK(!hasLog);
  }

  // --- grepFiles: content search, exclusions, context ---
  {
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("app"), "", 0, 100, 0, &err);
    CHECK(err.empty());
    CHECK(gr.hits.size() == 2);  // src/app.cpp + include/app.h
    // Hits carry 1-based line numbers and relative paths.
    bool line1 = false;
    for (auto const& h : gr.hits)
      if (h.file == "src/app.cpp" && h.line == 1) line1 = true;
    CHECK(line1);
  }
  // include filter narrows to one file.
  {
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("app"), "*.h", 0, 100, 0, &err);
    CHECK(gr.hits.size() == 1);
    CHECK(gr.hits[0].file == "include/app.h");
  }
  // context_lines attaches neighbours.
  {
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("return 0"), "", 1, 100, 0, &err);
    CHECK(gr.hits.size() == 1);
    CHECK(gr.hits[0].context.size() == 1);  // line above only (hit on line 1)
  }
  // Binary files are skipped.
  {
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("app"), "", 0, 100, 0, &err);
    for (auto const& h : gr.hits) CHECK(!has(h.file, ".bin"));
  }

  // --- tool-level JSON ---
  {
    std::string r = agent::toolGrep("{\"path\":\"" + dir + "\",\"pattern\":\"TODO\"}");
    CHECK(has(r, "\"ok\":true"));
    CHECK(has(r, "\"matches\":1"));
    CHECK(has(r, "src/app.cpp"));
    CHECK(has(r, "\"line\":2"));
  }
  {
    std::string r = agent::toolGrep("{\"path\":\"" + dir + "\",\"pattern\":\"(\"}");
    CHECK(has(r, "\"error\""));
  }
  {
    std::string r = agent::toolGlob("{\"path\":\"" + dir + "\",\"pattern\":\"*.md\"}");
    CHECK(has(r, "\"ok\":true"));
    CHECK(has(r, "docs/readme.md"));
  }
  {
    std::string r = agent::toolGlob("{\"path\":\"" + dir + "\",\"pattern\":\"src/**/util.cpp\"}");
    CHECK(has(r, "src/util.cpp"));
  }

  // --- regressions for the silent-loss fixes ------------------------------
  // §1 a >4 KiB line used to be dropped whole; §2 files ending in '\n' used to
  // yield a phantom extra line; §3 regex-only search made literal text lie
  // ("C++" is really the regex "C+"); §4/§6 exclusions used to be unreported.
  write(dir + "/src/one_line.json", "NEEDLE_LONG " + std::string(5000, 'x') + "\n");
  write(dir + "/src/tail_line.json", std::string(6000, 'y') + "NEEDLE_TAIL\n");
  write(dir + "/src/blobby.dat", std::string("NEEDLE_BIN") + char(0) + "bytes\n");
  {
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("NEEDLE_LONG"), "", 0, 100, 0, &err);
    CHECK(gr.hits.size() == 1);
    CHECK(gr.hits[0].textTruncated);
    CHECK(gr.stats.linesTruncated == 1);
    CHECK(has(gr.hits[0].text, "bytes]"));
  }
  {
    // A hit past the cap must still be visible: the reported window is centred
    // on the match, not taken from the head of the line.
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("NEEDLE_TAIL"), "", 0, 100, 0, &err);
    CHECK(gr.hits.size() == 1);
    CHECK(has(gr.hits[0].text, "NEEDLE_TAIL"));
    CHECK(has(gr.hits[0].text, "skip"));
  }
  {
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("."), "", 0, 1000, 0, &err);
    bool phantom = false;
    for (auto const& h : gr.hits) {
      if (h.file == "src/app.cpp" && h.line > 2) phantom = true;
    }
    CHECK(!phantom);
  }
  {
    std::string err;
    auto gr = agent::grepFiles(dir, std::regex("NEEDLE_BIN"), "", 0, 100, 0, &err);
    CHECK(gr.hits.empty());
    CHECK(gr.stats.skippedBinary >= 1);
  }
  {
    CHECK(agent::regexEscape("C++") != "C++");
    std::string r = agent::toolGrep("{\"path\":\"" + dir +
                                    "\",\"pattern\":\"NEEDLE_LONG\",\"fixed\":true}");
    CHECK(has(r, "\"matches\":1"));
    CHECK(has(r, "\"text_truncated\":true"));
    CHECK(has(r, "scanned_files"));
    CHECK(has(r, "\"skipped_dirs\""));
    std::string lit =
        agent::toolGrep("{\"path\":\"" + dir + "\",\"pattern\":\"(parens\",\"fixed\":true}");
    CHECK(!has(lit, "\"error\""));
    std::string bad = agent::toolGrep("{\"path\":\"" + dir + "\",\"pattern\":\"(parens\"}");
    CHECK(has(bad, "fixed"));
  }

  std::filesystem::remove_all(dir);
  std::printf("checks=%d failures=%d\n", checks, failures);
  return 0;
}