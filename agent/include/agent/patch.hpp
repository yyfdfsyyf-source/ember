#pragma once
#include <string>
#include <vector>

namespace agent {

// A hunk from a unified diff. `lines` keep the leading marker byte: ' ' for
// context, '-' for removed, '+' for added (content after the marker).
struct DiffHunk {
  int oldStart = 0;
  int oldCount = 0;
  int newStart = 0;
  int newCount = 0;
  std::vector<std::string> lines;
};

// A parsed single-file unified diff.
struct UnifiedDiff {
  std::string oldFile;
  std::string newFile;
  bool isCreate = false;  // oldFile == /dev/null (pure insertion)
  bool isDelete = false;  // newFile == /dev/null (pure deletion)
  std::vector<DiffHunk> hunks;
};

// Parse a single-file unified diff. Returns false and sets `error` on malformed
// input or when the text spans more than one file.
bool parseUnifiedDiff(std::string const& text, UnifiedDiff& d, std::string& error);

// Split file text into lines (empty file => zero lines; trailing newline does
// not add an extra empty element). Shared by the patch tool and callers.
std::vector<std::string> splitFileLines(std::string const& text);

// Apply `d` to `oldLines`, producing `outLines`. On failure returns false and
// sets `error` to a description of the failing hunk plus a numbered context
// snippet of the file around the expected location.
bool applyDiffLines(std::vector<std::string> const& oldLines, UnifiedDiff const& d,
                    std::vector<std::string>& outLines, std::string& error);

}  // namespace agent
