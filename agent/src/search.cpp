// Search primitives for the agent: recursive directory walking with sensible
// exclusions, glob path matching ('*', '?', '**') and content search via
// std::regex (ECMAScript syntax). No third-party dependencies.
#include "agent/search.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agent {

namespace {

namespace fs = std::filesystem;

// A line longer than this is searched up to kLineScanCap and reported as a
// window centred on the match: previously any line over 4 KiB was dropped
// whole, which silently hid every hit in single-line JSON, JSONL and minified
// JS.
constexpr size_t kLineTextCap = 4096;
constexpr size_t kLineScanCap = 65536;
constexpr size_t kBinaryProbeBytes = 8192;

bool isExcludedDir(char const* name) {
  if (name[0] == '.') return true;  // .git, .cache, .idea, ...
  static std::string const kExcluded[] = {"node_modules", "out", "build", "dist",
                                          "target",       "bin",  "obj",   "browser_screenshots"};
  for (auto const& d : kExcluded) {
    if (std::strcmp(name, d.c_str()) == 0) return true;
  }
  return false;
}

bool isBinary(std::string const& head) {
  return head.find('\0') != std::string::npos;
}

void noteSkippedDir(WalkStats* st, std::string const& name) {
  if (!st) return;
  for (auto const& d : st->skippedDirs) {
    if (d == name) return;
  }
  st->skippedDirs.push_back(name);
  std::sort(st->skippedDirs.begin(), st->skippedDirs.end());
}

// Cap for display, and say so: a truncated hit is honest, a dropped hit is a
// false "not found".
std::string capText(std::string const& s, bool& truncated) {
  truncated = s.size() > kLineTextCap;
  if (!truncated) return s;
  return s.substr(0, kLineTextCap) + "...[truncated, +" +
         std::to_string(s.size() - kLineTextCap) + " bytes]";
}

std::vector<std::string> split(std::string const& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

bool matchComponent(std::string const& pat, std::string const& s) {
  size_t pi = 0, si = 0;
  while (pi < pat.size()) {
    char c = pat[pi];
    if (c == '*') {
      while (pi < pat.size() && pat[pi] == '*') pi++;
      if (pi == pat.size()) return true;
      for (size_t k = si; k <= s.size(); k++) {
        if (matchComponent(pat.substr(pi), s.substr(k))) return true;
      }
      return false;
    }
    if (si >= s.size()) return false;
    if (c == '?') {
      pi++;
      si++;
      continue;
    }
    if (c != s[si]) return false;
    pi++;
    si++;
  }
  return si == s.size();
}

// Recursive glob: '**' spans any number of path components.
bool globParts(std::vector<std::string> const& pat, size_t pi,
               std::vector<std::string> const& path, size_t si) {
  if (pi == pat.size()) return si == path.size();
  if (pat[pi] == "**") {
    for (size_t k = si; k <= path.size(); k++) {
      if (globParts(pat, pi + 1, path, k)) return true;
    }
    return false;
  }
  if (si >= path.size()) return false;
  return matchComponent(pat[pi], path[si]) && globParts(pat, pi + 1, path, si + 1);
}

std::string relOf(fs::path const& root, fs::path const& p) {
  std::string s = fs::relative(p, root).generic_string();
  if (s.rfind("./", 0) == 0) s = s.substr(2);
  return s;
}

}  // namespace

bool globMatch(std::string const& pattern, std::string const& relPath) {
  if (pattern.empty()) return false;
  return globParts(split(pattern, '/'), 0, split(relPath, '/'), 0);
}

std::string regexEscape(std::string const& s) {
  static std::string const kMeta = "\\^$.|?*+()[]{}";
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (kMeta.find(c) != std::string::npos) out += '\\';
    out += c;
  }
  return out;
}

namespace {

std::vector<std::string> walkFilesImpl(std::string const& root, size_t maxBytes,
                                       WalkStats* stats, std::string* err) {
  std::vector<std::string> out;
  std::error_code ec;
  fs::path rootPath(root.empty() ? "." : root);
  if (!fs::is_directory(rootPath, ec)) {
    if (err) *err = "not a directory: " + root;
    return out;
  }
  fs::recursive_directory_iterator it(
      rootPath, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      it.disable_recursion_pending();
      ec.clear();
      continue;
    }
    std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (isExcludedDir(name.c_str())) {
        noteSkippedDir(stats, name);
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    if (maxBytes > 0) {
      uintmax_t sz = it->file_size(ec);
      if (ec) continue;
      if (sz > maxBytes) {
        if (stats) stats->skippedTooBig++;
        continue;
      }
    }
    if (stats) stats->filesScanned++;
    out.push_back(relOf(rootPath, it->path()));
  }
  if (ec && err) *err = "walk error: " + ec.message();
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::vector<std::string> walkFiles(std::string const& root, size_t maxBytes,
                                   std::string* err) {
  return walkFilesImpl(root, maxBytes, nullptr, err);
}

std::vector<std::string> walkFiles(std::string const& root, size_t maxBytes,
                                   WalkStats& stats, std::string* err) {
  return walkFilesImpl(root, maxBytes, &stats, err);
}

GrepResult grepFiles(std::string const& root, std::regex const& re,
                     std::string const& include, int contextLines,
                     size_t maxResults, size_t maxBytes, std::string* err) {
  GrepResult res;
  // A bare name like "*.cpp" matches at any depth. Hoisted out of the loop:
  // it used to be rebuilt (and reallocated) once per file.
  std::string inc = include;
  if (!inc.empty() && inc.find('/') == std::string::npos) inc = "**/" + inc;
  for (auto const& rel : walkFilesImpl(root, maxBytes, &res.stats, err)) {
    if (res.hits.size() >= maxResults) {
      res.truncated = true;
      break;
    }
    if (!inc.empty() && !globMatch(inc, rel)) continue;
    fs::path full = fs::path(root.empty() ? "." : root) / rel;
    std::ifstream f(full, std::ios::binary);
    if (!f) continue;
    std::string content;
    {
      // Decide "binary" from a head probe. Reading the whole file first made a
      // tree full of archives cost the entire read before being discarded.
      std::string probe(kBinaryProbeBytes, '\0');
      f.read(&probe[0], static_cast<std::streamsize>(kBinaryProbeBytes));
      std::streamsize got = f.gcount();
      probe.resize(got < 0 ? 0 : (size_t)got);
      if (isBinary(probe)) {
        res.stats.skippedBinary++;
        continue;
      }
      content = std::move(probe);
      std::ostringstream rest;
      rest << f.rdbuf();
      content += rest.str();
    }
    if (content.empty()) continue;

    // Exactly the file's lines. The old splitter appended an extra empty line
    // when the file ended in '\n', which grep then reported as a real hit at a
    // line number that does not exist.
    std::vector<std::string> lines;
    {
      size_t start = 0;
      for (;;) {
        size_t nl = content.find('\n', start);
        if (nl == std::string::npos) {
          if (start < content.size()) lines.push_back(content.substr(start));
          break;
        }
        lines.push_back(content.substr(start, nl - start));
        start = nl + 1;
      }
    }
    auto stripCr = [](std::string s) {
      if (!s.empty() && s.back() == '\r') s.pop_back();
      return s;
    };
    for (size_t i = 0; i < lines.size() && res.hits.size() < maxResults; i++) {
      std::string clean = stripCr(lines[i]);
      size_t scanLen = std::min<size_t>(clean.size(), kLineScanCap);
      std::smatch m;
      std::string::const_iterator first = clean.cbegin();
      std::string::const_iterator last = clean.cbegin() + (long)scanLen;
      if (!std::regex_search(first, last, m, re)) continue;
      SearchHit h;
      h.file = rel;
      h.line = (int64_t)i + 1;
      // Report a window centred on the match. Truncating from the head instead
      // would hand back text that does not contain the hit at all, which is
      // exactly what a 15 KB single-line JSON record looks like.
      size_t cap = kLineTextCap;
      size_t pos = (size_t)m.position(0);
      size_t start = pos > cap / 2 ? pos - cap / 2 : 0;
      if (start + cap > clean.size()) start = clean.size() > cap ? clean.size() - cap : 0;
      std::string slice = clean.substr(start, std::min(cap, clean.size() - start));
      size_t left = start, right = clean.size() - start - slice.size();
      h.textTruncated = left > 0 || right > 0;
      h.text = (left ? "...[skip " + std::to_string(left) + " bytes]" : "") + slice +
               (right ? "...[+" + std::to_string(right) + " bytes]" : "");
      if (h.textTruncated) res.stats.linesTruncated++;
      if (contextLines > 0) {
        for (int ci = -contextLines; ci <= contextLines; ci++) {
          if (ci == 0) continue;
          long long idx = (long long)i + ci;
          if (idx < 0) continue;
          if (idx >= (long long)lines.size()) break;
          bool tr = false;
          h.context.push_back(capText(stripCr(lines[(size_t)idx]), tr));
        }
      }
      res.hits.push_back(std::move(h));
    }
  }
  if (res.hits.size() >= maxResults) res.truncated = true;
  return res;
}

std::vector<std::string> globFiles(std::string const& root,
                                   std::string const& pattern,
                                   size_t maxEntries, std::string* err) {
  WalkStats stats;
  return globFiles(root, pattern, maxEntries, stats, err);
}

std::vector<std::string> globFiles(std::string const& root,
                                   std::string const& pattern,
                                   size_t maxEntries, WalkStats& stats,
                                   std::string* err) {
  std::vector<std::string> out;
  for (auto const& rel : walkFilesImpl(root, 0, &stats, err)) {
    if (out.size() >= maxEntries) break;
    if (globMatch(pattern, rel)) out.push_back(rel);
  }
  return out;
}

}  // namespace agent