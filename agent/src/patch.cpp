// Minimal unified-diff parser/applier (single file), used by the `patch` tool.
// Mirrors the behaviour of a classic `patch -pN` apply with rich failure
// reporting: on a mismatch the caller gets the failing hunk header plus a
// numbered snippet of the current file content around the expected position.
#include "agent/patch.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace agent {

namespace {

bool startsWith(std::string const& s, char const* p) {
  return s.compare(0, strlen(p), p) == 0;
}

std::string stripCR(std::string s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
  return s;
}

std::string stripTimestamp(std::string s) {
  auto p = s.find('\t');
  if (p != std::string::npos) s = s.substr(0, p);
  return s;
}

// Strip git's a/ b/ single-component prefixes from a header path.
std::string stripGitPrefix(std::string const& p) {
  if (p.size() >= 3 && p[0] == 'a' && p[1] == '/') return p.substr(2);
  if (p.size() >= 3 && p[0] == 'b' && p[1] == '/') return p.substr(2);
  return p;
}

// Check whether `pat` (context/removed lines) matches `file` at index `pos`.
bool matchesAt(std::vector<std::string> const& file, std::vector<std::pair<bool, std::string>> const& pat, int pos) {
  if (pos < 0 || pos + (long)pat.size() > (long)file.size()) return false;
  for (size_t j = 0; j < pat.size(); j++) {
    if (file[(size_t)pos + j] != pat[j].second) return false;
  }
  return true;
}

}  // namespace

std::vector<std::string> splitFileLines(std::string const& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      out.push_back(stripCR(std::move(cur)));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(stripCR(std::move(cur)));
  return out;
}

bool parseUnifiedDiff(std::string const& text, UnifiedDiff& d, std::string& error) {
  d = UnifiedDiff{};
  auto lines = splitFileLines(text);

  // Skip any preamble ("diff --git", "index ...", ...) up to the first header.
  size_t i = 0;
  while (i < lines.size() && !startsWith(lines[i], "--- ")) i++;
  if (i >= lines.size()) { error = "no '---' file header found in diff"; return false; }
  if (i + 1 >= lines.size() || !startsWith(lines[i + 1], "+++ ")) {
    error = "expected '+++' header after '---'";
    return false;
  }
  d.oldFile = stripGitPrefix(stripTimestamp(lines[i].substr(4)));
  d.newFile = stripGitPrefix(stripTimestamp(lines[i + 1].substr(4)));
  d.isCreate = (d.oldFile == "/dev/null");
  d.isDelete = (d.newFile == "/dev/null");
  i += 2;

  DiffHunk hunk;
  bool haveHunk = false;
  auto pushHunk = [&]() {
    if (haveHunk) d.hunks.push_back(hunk);
  };

  for (; i < lines.size(); i++) {
    std::string const& ln = lines[i];
    if (ln.empty()) continue;
    if (ln.size() >= 2 && ln[0] == '\\') continue;  // "\ No newline at end of file"
    if (ln.size() >= 2 && ln[0] == '@' && ln[1] == '@') {
      pushHunk();
      hunk = DiffHunk{};
      haveHunk = true;
      int o = 0, oc = 0, n = 0, nc = 0;
      // Header format: @@ -o[,oc] +n[,nc] @@ [section ...]
      std::string const h = ln.substr(3);
      int got = sscanf(h.c_str(), "-%d,%d +%d,%d", &o, &oc, &n, &nc);
      if (got != 4) {
        got = sscanf(h.c_str(), "-%d +%d", &o, &n);
        if (got != 2) { error = "malformed hunk header: " + ln; return false; }
        oc = 1;
        nc = 1;
      }
      if (o < 0 || n < 0 || oc < 0 || nc < 0) { error = "negative hunk counts: " + ln; return false; }
      hunk.oldStart = o;
      hunk.oldCount = oc;
      hunk.newStart = n;
      hunk.newCount = nc;
      continue;
    }
    if (haveHunk && (startsWith(ln, "--- ") || startsWith(ln, "+++ "))) {
      error = "diff spans more than one file; patch one file per call";
      return false;
    }
    if (haveHunk && !ln.empty() && (ln[0] == ' ' || ln[0] == '-' || ln[0] == '+')) {
      hunk.lines.push_back(ln);
    }
    // anything else (stray metadata) is ignored
  }
  pushHunk();
  if (d.hunks.empty()) { error = "no @@ hunks found in diff"; return false; }
  return true;
}

bool applyDiffLines(std::vector<std::string> const& oldLines, UnifiedDiff const& d,
                    std::vector<std::string>& outLines, std::string& error) {
  outLines = oldLines;
  long offset = 0;

  for (size_t hk = 0; hk < d.hunks.size(); hk++) {
    DiffHunk const& h = d.hunks[hk];
    // Build the old-side pattern: context + removed lines (added lines are new).
    std::vector<std::pair<bool, std::string>> pat;
    pat.reserve(h.lines.size());
    for (auto const& l : h.lines) {
      if (l[0] == '+') continue;
      pat.push_back({l[0] == '-', l.substr(1)});
    }
    // New-side block: everything except removed lines, in diff order.
    std::vector<std::string> block;
    block.reserve(h.lines.size());
    for (auto const& l : h.lines) {
      if (l[0] == '-') continue;
      block.push_back(l.substr(1));
    }

    long expected = (long)h.oldStart - 1 + offset;
    if (expected < 0) expected = 0;

    // Locate the pattern, preferring the expected offset, then a small window.
    int pos = -1;
    long n = (long)outLines.size();
    if (expected <= n && matchesAt(outLines, pat, (int)expected)) {
      pos = (int)expected;
    } else {
      int lo = (int)std::max(0L, expected - 8);
      int hi = (int)std::min(n - (long)pat.size(), expected + 8);
      for (int k = lo; k <= hi; k++) {
        if (matchesAt(outLines, pat, k)) { pos = k; break; }
      }
    }

    if (pos < 0) {
      // Build the harness-style diagnostic.
      char hdr[128];
      snprintf(hdr, sizeof hdr, "@@ -%d,%d +%d,%d @@", h.oldStart, h.oldCount,
               h.newStart, h.newCount);
      error = "hunk failed to match " + std::string(hdr);
      error += "\ncurrent file lines ";
      int ctxStart = (int)std::max(0L, expected - 2);
      int ctxEnd = (int)std::min(n, expected + std::max((long)h.oldCount, 4L) + 2);
      error += std::to_string(ctxStart + 1) + "-" + std::to_string(ctxEnd);
      error += ":\n";
      for (int li = ctxStart; li < ctxEnd; li++) {
        error += "  " + std::to_string(li + 1) + "|" + outLines[li] + "\n";
      }
      return false;
    }

    // Replace [pos, pos+pat.size()) with the new-side block.
    outLines.erase(outLines.begin() + pos, outLines.begin() + pos + (long)pat.size());
    outLines.insert(outLines.begin() + pos, block.begin(), block.end());

    offset += (long)h.newCount - (long)h.oldCount;
  }
  return true;
}

}  // namespace agent
