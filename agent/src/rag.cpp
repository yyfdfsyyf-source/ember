#include "agent/rag.hpp"
#include "minijson.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agent::rag {

namespace fs = std::filesystem;

namespace {

// Precise file mtime. MinGW's std::filesystem::last_write_time resolves to
// whole seconds on Windows, which misses edits inside the same second; the
// Win32 FILETIME (100ns) does not.
int64_t fileMtime(fs::path const& p) {
#ifdef _WIN32
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;
  FILETIME ft{};
  BOOL ok = GetFileTime(h, nullptr, nullptr, &ft);
  CloseHandle(h);
  if (!ok) return -1;
  return ((int64_t)ft.dwHighDateTime << 32) | (int64_t)ft.dwLowDateTime;
#else
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  return ec ? -1 : (int64_t)t.time_since_epoch().count();
#endif
}


// Directories never searched (same policy as grep/glob).
bool excludedDir(std::string const& name) {
  static const char* kDirs[] = {"node_modules", "out", "build", "dist", "target",
                                "bin", "obj", "browser_screenshots"};
  if (!name.empty() && name[0] == '.') return true;
  for (char const* d : kDirs)
    if (name == d) return true;
  return false;
}

// Segments of a file: consecutive lines up to ~1200 bytes, with one line of
// overlap so a hit keeps its surrounding context.
void splitSegments(std::string const& text, std::string const& path,
                   std::vector<Segment>& out) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    lines.push_back(line);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (lines.size() >= 2 && lines.back().empty()) lines.pop_back();  // trailing \n
  if (lines.empty()) return;

  size_t start = 0;
  size_t bytes = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    bytes += lines[i].size() + 1;
    bool last = (i + 1 == lines.size());
    if (last || bytes >= 1200) {
      size_t end = last ? lines.size() : i + 1;  // last segment runs to the end
      std::string seg;
      for (size_t k = start; k < end; k++) {
        seg += lines[k];
        seg += '\n';
      }
      Segment s;
      s.path = path;
      s.startLine = (int64_t)start + 1;
      s.text = seg;
      out.push_back(std::move(s));
      if (last) break;
      start = i;          // one line of overlap
      bytes = lines[i].size() + 1;
    }
  }
}

bool isCjk(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}

bool isWordChar(uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
         cp == '_';
}

}  // namespace

void tokenize(std::string const& text, std::vector<std::string>& out) {
  std::string word;
  auto flush = [&] {
    if (!word.empty()) {
      out.push_back(word);
      word.clear();
    }
  };
  size_t i = 0;
  while (i < text.size()) {
    unsigned char c = (unsigned char)text[i];
    if (c < 0x80) {
      uint32_t cp = c;
      if (isWordChar(cp)) {
        word += (char)(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp);
      } else {
        flush();
      }
      i++;
      continue;
    }
    // UTF-8 decode
    uint32_t cp = 0;
    int len = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 3; }
    for (int k = 1; k <= len && i + k < text.size(); k++) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
    flush();
    if (isCjk(cp)) {
      char buf[5];
      int n = cp < 0x80 ? 1 : (cp < 0x800 ? 2 : (cp < 0x10000 ? 3 : 4));
      if (n == 1) buf[0] = (char)cp;
      else if (n == 2) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); }
      else if (n == 3) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); }
      else { buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); }
      buf[n] = 0;
      out.push_back(std::string(buf, (size_t)n));
    }
    i += (size_t)len + 1;
  }
  flush();
}

std::string Index::build(std::string const& rootDir) {
  fs::path root(rootDir);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return "not a directory: " + rootDir;
  std::string rootStr = root.lexically_normal().string();

  segments_.clear();
  files_.clear();
  fileMtimes_.clear();
  postings_.clear();
  root_ = rootStr;

  std::vector<Segment> segs;
  std::vector<std::string> files;
  std::vector<int64_t> mtimes;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied),
       end; it != end; it.increment(ec)) {
    if (ec) { ec.clear(); continue; }
    std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (excludedDir(name)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    if (ec) { ec.clear(); continue; }
    int64_t mtime = fileMtime(it->path());
    if (mtime < 0) continue;
    std::string rel = it->path().lexically_relative(root).lexically_normal().generic_string();
    std::ifstream f(it->path(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty()) continue;
    if (text.find('\0') != std::string::npos) continue;  // binary
    splitSegments(text, rel, segs);
    files.push_back(rel);
    mtimes.push_back(mtime);
  }

  segments_ = std::move(segs);
  files_ = std::move(files);
  fileMtimes_ = std::move(mtimes);

  int64_t total = 0;
  std::vector<int32_t> segToks(segments_.size(), 0);
  for (size_t i = 0; i < segments_.size(); i++) {
    std::vector<std::string> toks;
    tokenize(segments_[i].text, toks);
    segToks[i] = (int32_t)toks.size();
    std::sort(toks.begin(), toks.end());
    size_t k = 0;
    while (k < toks.size()) {
      size_t j = k;
      while (j < toks.size() && toks[j] == toks[k]) j++;
      postings_[toks[k]].push_back({(int64_t)i, (int32_t)(j - k)});
      k = j;
    }
    total += (int64_t)toks.size();
  }
  totalTokens_ = total;
  segTokens_ = std::move(segToks);
  avgdl_ = segments_.empty() ? 1.0 : (double)total / (double)segments_.size();
  return "";
}

std::string Index::buildIfStale(std::string const& rootDir) {
  fs::path root(rootDir);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return "not a directory: " + rootDir;
  // Compare the current tree against the cached file list + mtimes.
  std::vector<std::pair<std::string, int64_t>> now;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied),
       end; it != end; it.increment(ec)) {
    if (ec) { ec.clear(); continue; }
    std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (excludedDir(name)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    if (ec) { ec.clear(); continue; }
    int64_t mtime = fileMtime(it->path());
    if (mtime < 0) continue;
    std::string rel = it->path().lexically_relative(root).lexically_normal().generic_string();
    now.emplace_back(std::move(rel), mtime);
  }
  std::sort(now.begin(), now.end());
  bool same = !segments_.empty() && now.size() == files_.size();
  if (same) {
    std::vector<std::pair<std::string, int64_t>> old;
    old.reserve(files_.size());
    for (size_t i = 0; i < files_.size(); i++)
      old.emplace_back(files_[i], fileMtimes_[i]);
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i < now.size() && same; i++)
      if (now[i] != old[i]) same = false;
  }
  if (same) return "unchanged";
  return build(rootDir);
}

bool Index::search(std::string const& query, int topK, std::vector<Hit>& out) const {
  out.clear();
  if (segments_.empty() || query.empty()) return false;
  if (topK < 1) topK = 1;
  std::vector<std::string> toks;
  tokenize(query, toks);
  std::sort(toks.begin(), toks.end());
  toks.erase(std::unique(toks.begin(), toks.end()), toks.end());

  double N = (double)segments_.size();
  std::unordered_map<int64_t, double> acc;
  bool any = false;
  for (auto const& t : toks) {
    auto it = postings_.find(t);
    if (it == postings_.end()) continue;
    any = true;
    double df = (double)it->second.size();
    double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));
    for (auto const& p : it->second) {
      double dl = (double)(size_t)segTokens_[(size_t)p.seg];
      // BM25 term weight for this segment
      double tf = (double)p.tf;
      double score = idf * (tf * 1.6) / (tf + 1.6 * (1.0 - 0.75 + 0.75 * dl / avgdl_));
      acc[p.seg] += score;
    }
  }
  if (!any) return false;

  std::vector<std::pair<int64_t, double>> scored(acc.begin(), acc.end());
  std::sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });
  int n = (int)std::min((size_t)topK, scored.size());
  for (int i = 0; i < n; i++) {
    Hit h;
    h.path = segments_[(size_t)scored[i].first].path;
    h.line = segments_[(size_t)scored[i].first].startLine;
    h.score = scored[i].second;
    h.text = segments_[(size_t)scored[i].first].text;
    if (h.text.size() > 2000) h.text.resize(2000);
    out.push_back(std::move(h));
  }
  return true;
}

std::string Index::save(std::string const& path) const {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  mini::Value root = mini::Value::makeObject();
  root.set("version", mini::Value::makeInt(1));
  root.set("root", mini::Value::makeString(root_));
  root.set("avgdl", mini::Value::makeDouble(avgdl_));
  mini::Value filesArr = mini::Value::makeArray();
  for (auto const& f : files_) filesArr.arr.push_back(mini::Value::makeString(f));
  root.set("files", std::move(filesArr));
  mini::Value mtArr = mini::Value::makeArray();
  for (auto m : fileMtimes_) mtArr.arr.push_back(mini::Value::makeInt(m));
  root.set("mtimes", std::move(mtArr));
  mini::Value stArr = mini::Value::makeArray();
  for (auto n : segTokens_) stArr.arr.push_back(mini::Value::makeInt(n));
  root.set("segtokens", std::move(stArr));
  mini::Value segArr = mini::Value::makeArray();
  for (auto const& s : segments_) {
    mini::Value o = mini::Value::makeObject();
    o.set("p", mini::Value::makeString(s.path));
    o.set("l", mini::Value::makeInt(s.startLine));
    o.set("t", mini::Value::makeString(s.text));
    segArr.arr.push_back(std::move(o));
  }
  root.set("segments", std::move(segArr));
  mini::Value posArr = mini::Value::makeArray();
  for (auto const& kv : postings_) {
    mini::Value o = mini::Value::makeObject();
    o.set("w", mini::Value::makeString(kv.first));
    mini::Value pArr = mini::Value::makeArray();
    for (auto const& p : kv.second) {
      mini::Value pp = mini::Value::makeArray();
      pp.arr.push_back(mini::Value::makeInt(p.seg));
      pp.arr.push_back(mini::Value::makeInt(p.tf));
      pArr.arr.push_back(std::move(pp));
    }
    o.set("ps", std::move(pArr));
    posArr.arr.push_back(std::move(o));
  }
  root.set("postings", std::move(posArr));
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return "cannot write index file: " + path;
  f << mini::dump(root);
  if (!f.good()) return "write failed: " + path;
  return "";
}

std::string Index::load(std::string const& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "cannot open index file: " + path;
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  mini::Value root;
  if (!mini::tryParse(text, root) || root.type != mini::Value::Object)
    return "index file is not valid JSON: " + path;
  if (!root.get("version") || root.get("version")->asInt(0) != 1)
    return "unsupported index version";
  segments_.clear();
  files_.clear();
  fileMtimes_.clear();
  segTokens_.clear();
  postings_.clear();
  root_ = root.has("root") ? root.get("root")->asString() : "";
  avgdl_ = root.has("avgdl") ? root.get("avgdl")->asDouble(1.0) : 1.0;
  auto strArr = [](mini::Value const* v) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (v && v->type == mini::Value::Array)
      for (auto const& x : v->arr)
        if (x.type == mini::Value::String) out.push_back(x.s);
    return out;
  };
  files_ = strArr(root.get("files"));
  if (mini::Value const* m = root.get("mtimes"))
    if (m->type == mini::Value::Array)
      for (auto const& x : m->arr) fileMtimes_.push_back(x.asInt(0));
  if (mini::Value const* m = root.get("segtokens"))
    if (m->type == mini::Value::Array)
      for (auto const& x : m->arr) segTokens_.push_back((int32_t)x.asInt(0));
  if (mini::Value const* segs = root.get("segments")) {
    if (segs->type != mini::Value::Array) return "corrupt segments array";
    segments_.reserve(segs->arr.size());
    for (auto const& o : segs->arr) {
      Segment s;
      if (o.type != mini::Value::Object) return "corrupt segment entry";
      s.path = o.has("p") ? o.get("p")->asString() : "";
      s.startLine = o.has("l") ? o.get("l")->asInt(1) : 1;
      s.text = o.has("t") ? o.get("t")->asString() : "";
      segments_.push_back(std::move(s));
    }
  }
  if (mini::Value const* ps = root.get("postings")) {
    if (ps->type != mini::Value::Array) return "corrupt postings array";
    for (auto const& o : ps->arr) {
      if (o.type != mini::Value::Object) return "corrupt posting entry";
      std::string w = o.has("w") ? o.get("w")->asString() : "";
      mini::Value const* lst = o.get("ps");
      std::vector<Posting> post;
      if (lst && lst->type == mini::Value::Array)
        for (auto const& pp : lst->arr)
          if (pp.type == mini::Value::Array && pp.arr.size() >= 2)
            post.push_back({pp.arr[0].asInt(0), (int32_t)pp.arr[1].asInt(0)});
      if (!w.empty()) postings_[std::move(w)] = std::move(post);
    }
  }
  return "";
}

}  // namespace agent::rag