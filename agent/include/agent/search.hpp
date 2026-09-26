#pragma once
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace agent {

// One grep hit (file path is relative to the search root, '/' separators).
struct SearchHit {
  std::string file;
  int64_t line;  // 1-based
  std::string text;
  // For long lines `text` is a window centred on the match (marked with
  // ...[skip N bytes] / ...[+N bytes]) rather than the whole line; this says
  // so, so a caller can tell "no match" apart from "match, text sliced".
  bool textTruncated = false;
  std::vector<std::string> context;  // surrounding lines when requested
};

// What a tree walk left out. Exclusions used to be invisible, which made an
// incomplete result look like a definitive "not found".
struct WalkStats {
  size_t filesScanned = 0;
  size_t skippedBinary = 0;   // NUL in the first 8 KiB
  size_t skippedTooBig = 0;   // over maxBytes
  size_t linesTruncated = 0;  // hit lines whose text was capped
  std::vector<std::string> skippedDirs;  // pruned directory names, unique+sorted
};

// Glob-style path matching against a '/' separated relative path.
// Supports '*', '?' and '**' (any number of path components).
bool globMatch(std::string const& pattern, std::string const& relPath);

// Escape every ECMAScript-regex metacharacter so `s` matches literally.
// Lets a "fixed string" search mode reuse the same engine.
std::string regexEscape(std::string const& s);

// Recursively walk `root`, skipping hidden dirs (.git, .cache, ...),
// build/output dirs and files larger than `maxBytes` (0 = no limit).
// Returns matching files (relative paths) or sets *err.
std::vector<std::string> walkFiles(std::string const& root, size_t maxBytes,
                                   std::string* err);
// Same, but reports what the exclusions left out.
std::vector<std::string> walkFiles(std::string const& root, size_t maxBytes,
                                   WalkStats& stats, std::string* err);

// Content search across every file under `root`. `include` is an optional
// glob filter applied to relative paths. Stops after `maxResults` hits
// (truncated=true). Sets *err on root problems.
struct GrepResult {
  std::vector<SearchHit> hits;
  bool truncated = false;
  WalkStats stats;
};
GrepResult grepFiles(std::string const& root, std::regex const& re,
                     std::string const& include, int contextLines,
                     size_t maxResults, size_t maxBytes, std::string* err);

// Path search: every file under `root` whose relative path matches `pattern`
// (glob with '**' support). Respects the same directory exclusions.
std::vector<std::string> globFiles(std::string const& root,
                                   std::string const& pattern,
                                   size_t maxEntries, std::string* err);
std::vector<std::string> globFiles(std::string const& root,
                                   std::string const& pattern,
                                   size_t maxEntries, WalkStats& stats,
                                   std::string* err);

}  // namespace agent
