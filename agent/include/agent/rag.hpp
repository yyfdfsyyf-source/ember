#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent::rag {

// BM25 keyword search over a directory tree. Pure C++ (no embeddings):
// an inverted index is built locally and can be persisted as JSON.
// Indexing units are "segments": per-file chunks of a few hundred tokens
// split at line boundaries, so hits return a precise code/text window.

struct Segment {
  std::string path;   // path relative to the index root
  int64_t startLine;  // 1-based line of the first character
  std::string text;
};

struct Hit {
  std::string path;
  int64_t line;    // segment start line
  double score;    // normalized BM25 score
  std::string text;
};

// Owns documents + postings for one index root.
class Index {
 public:
  // Scan rootDir, tokenize and index every text file there (same directory
  // exclusions as grep/glob: hidden dirs, node_modules, build outputs,
  // binaries). Returns "" on success or an error description.
  std::string build(std::string const& rootDir);
  // Like build() but first stat-checks the tree against the cached mtimes;
  // skips the full rescan when nothing changed. Returns "" on success; the
  // string "unchanged" when the existing index is still fresh.
  std::string buildIfStale(std::string const& rootDir);

  bool empty() const { return segments_.empty(); }
  size_t segmentCount() const { return segments_.size(); }
  size_t fileCount() const { return files_.size(); }
  std::string const& root() const { return root_; }

  // BM25 top-k segments for a query (tokenized like the index). Results are
  // ordered by descending score, ties broken by path. Returns false if the
  // index is empty.
  bool search(std::string const& query, int topK, std::vector<Hit>& out) const;

  // Persist/restore the whole index as one JSON file; "" on success.
  std::string save(std::string const& path) const;
  std::string load(std::string const& path);

 private:
  struct Posting {
    int64_t seg;
    int32_t tf;
  };

  std::string root_;
  std::vector<Segment> segments_;
  std::vector<std::string> files_;  // indexed file paths (for mtime checks)
  std::vector<int32_t> segTokens_;  // token count per segment (BM25 length)
  std::unordered_map<std::string, std::vector<Posting>> postings_;
  std::vector<int64_t> fileMtimes_;  // matches files_
  double avgdl_ = 1.0;
  int64_t totalTokens_ = 0;
};

// Tokenizer shared by the indexer and the query: [a-z0-9_]+ runs lowercased
// plus single CJK (U+4E00..U+9FFF and common ext-B) characters.
void tokenize(std::string const& text, std::vector<std::string>& out);

}  // namespace agent::rag