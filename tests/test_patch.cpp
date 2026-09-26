// Tests for the unified-diff engine and the editing tools (edit / patch).
#include "agent/patch.hpp"
#include "agent/tools.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
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

// True when `hay` contains `needle`.
static bool has(std::string const& hay, std::string const& needle) {
  return hay.find(needle) != std::string::npos;
}

static void testParse() {
  // Well-formed single conflict-free diff.
  std::string diff = R"(--- a/src/a.cpp
+++ b/src/a.cpp
@@ -1,3 +1,3 @@
 int x = 1;
-x = 2;
+x = 3;
 int y = 2;
)";
  agent::UnifiedDiff d;
  std::string err;
  CHECK(agent::parseUnifiedDiff(diff, d, err));
  CHECK(d.oldFile == "src/a.cpp");
  CHECK(d.newFile == "src/a.cpp");
  CHECK(!d.isCreate && !d.isDelete);
  CHECK(d.hunks.size() == 1);
  CHECK(d.hunks[0].oldStart == 1 && d.hunks[0].oldCount == 3);
  CHECK(d.hunks[0].newStart == 1 && d.hunks[0].newCount == 3);
  CHECK(d.hunks[0].lines.size() == 4);

  // No hunk -> error.
  agent::UnifiedDiff d2;
  CHECK(!agent::parseUnifiedDiff("no diff here", d2, err));

  // Multi-file diff -> error.
  std::string multi = R"(--- a/x
+++ b/x
@@ -1 +1 @@
-a
+a
--- a/y
+++ b/y
@@ -1 +1 @@
-b
+b
)";
  agent::UnifiedDiff d3;
  CHECK(!agent::parseUnifiedDiff(multi, d3, err));
}

static void testApply() {
  std::vector<std::string> file = {"line1", "line2", "line3", "line4"};
  std::string diff = R"(--- a/f
+++ b/f
@@ -2,2 +2,2 @@
 line2
-line3
+LINE3
 line4
)";
  agent::UnifiedDiff d;
  std::string err;
  CHECK(agent::parseUnifiedDiff(diff, d, err));
  std::vector<std::string> out;
  CHECK(agent::applyDiffLines(file, d, out, err));
  CHECK(out.size() == 4);
  CHECK(out[0] == "line1");
  CHECK(out[1] == "line2");
  CHECK(out[2] == "LINE3");
  CHECK(out[3] == "line4");

  // Multi-hunk application with offset adjustment.
  std::string multi = R"(--- a/f
+++ b/f
@@ -1,2 +1,2 @@
 line1
-line2
+line2x
@@ -4,1 +4,1 @@
-line4
+line4x
)";
  agent::UnifiedDiff d2;
  CHECK(agent::parseUnifiedDiff(multi, d2, err));
  std::vector<std::string> out2;
  CHECK(agent::applyDiffLines(file, d2, out2, err));
  CHECK(out2.size() == 4);
  CHECK(out2[0] == "line1");
  CHECK(out2[1] == "line2x");
  CHECK(out2[2] == "line3");
  CHECK(out2[3] == "line4x");

  // Failing hunk reports a diagnostic with a snippet.
  std::string bad = R"(--- a/f
+++ b/f
@@ -1,2 +1,2 @@
 no-such-line
-anything
+whatever
)";
  agent::UnifiedDiff d3;
  CHECK(agent::parseUnifiedDiff(bad, d3, err));
  std::vector<std::string> out3;
  CHECK(!agent::applyDiffLines(file, d3, out3, err));
  CHECK(has(err, "hunk failed to match"));
  CHECK(has(err, "line1"));
}

static void testToolEdit() {
  std::string w = agent::toolFileWrite("{\"path\":\"pt_test_edit.txt\",\"content\":\"aaa bbb ccc bbb\"}");
  CHECK(has(w, "\"ok\":true"));
  // single replace
  std::string r = agent::toolEdit("{\"path\":\"pt_test_edit.txt\",\"old_string\":\"bbb\",\"new_string\":\"XXX\"}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "\"replacements\":1"));
  std::string rd = agent::toolFileRead("{\"path\":\"pt_test_edit.txt\"}");
  CHECK(has(rd, "aaa XXX ccc bbb"));
  // replace all
  r = agent::toolEdit("{\"path\":\"pt_test_edit.txt\",\"old_string\":\"bbb\",\"new_string\":\"Y\",\"replace_all\":true}");
  CHECK(has(r, "\"replacements\":1"));
  rd = agent::toolFileRead("{\"path\":\"pt_test_edit.txt\"}");
  CHECK(has(rd, "aaa XXX ccc Y"));
  // not found
  r = agent::toolEdit("{\"path\":\"pt_test_edit.txt\",\"old_string\":\"zzz\",\"new_string\":\"x\"}");
  CHECK(has(r, "error"));
  // missing args
  CHECK(has(agent::toolEdit("{}"), "error"));
  std::remove("pt_test_edit.txt");
}

static void testToolPatchFile() {
  std::string w = agent::toolFileWrite("{\"path\":\"pt_test_patch.txt\",\"content\":\"one\\ntwo\\nthree\\nfour\\n\"}");
  CHECK(has(w, "\"ok\":true"));
  // Replace two lines and change a later line (multi-hunk).
  std::string patchJson =
      "{\"path\":\"pt_test_patch.txt\",\"patch\":" +
      std::string("\"--- a/pt_test_patch.txt\\n+++ b/pt_test_patch.txt\\n"
                  "@@ -1,2 +1,2 @@\\n one\\n-two\\n+TWO\\n"
                  "@@ -4,1 +4,1 @@\\n-four\\n+FOUR\\n\"") +
      "}";
  std::string r = agent::toolPatch(patchJson);
  CHECK(has(r, "\"ok\":true"));
  std::string rd = agent::toolFileRead("{\"path\":\"pt_test_patch.txt\"}");
  CHECK(has(rd, "one\\nTWO\\nthree\\nFOUR"));

  // Failure path: mismatched context yields rich error.
  std::string badJson =
      "{\"path\":\"pt_test_patch.txt\",\"patch\":" +
      std::string("\"--- a/pt_test_patch.txt\\n+++ b/pt_test_patch.txt\\n"
                  "@@ -1,2 +1,2 @@\\n nope\\n-x\\n+y\\n\"") +
      "}";
  std::string r2 = agent::toolPatch(badJson);
  CHECK(has(r2, "error"));
  CHECK(has(r2, "hunk failed to match"));

  // Create via /dev/null.
  std::string createJson =
      "{\"patch\":" +
      std::string("\"--- /dev/null\\n+++ b/created/new.txt\\n"
                  "@@ -0,0 +1,2 @@\\n+hello\\n+world\\n\"") +
      "}";
  std::string r3 = agent::toolPatch(createJson);
  CHECK(has(r3, "\"created\":true"));
  std::string rd3 = agent::toolFileRead("{\"path\":\"created/new.txt\"}");
  CHECK(has(rd3, "hello"));
  CHECK(has(rd3, "world"));

  // Delete via /dev/null new header.
  std::string delJson =
      "{\"path\":\"pt_test_patch.txt\",\"patch\":" +
      std::string("\"--- a/pt_test_patch.txt\\n+++ /dev/null\\n"
                  "@@ -1,4 +0,0 @@\\n-one\\n-TWO\\n-three\\n-FOUR\\n\"") +
      "}";
  std::string r4 = agent::toolPatch(delJson);
  CHECK(has(r4, "\"deleted\":true"));
  CHECK(has(agent::toolFileRead("{\"path\":\"pt_test_patch.txt\"}"), "error"));

  std::remove("created/new.txt");
  std::filesystem::remove("created");  // best-effort
}

static void testToolReadWindow() {
  std::string w = agent::toolFileWrite("{\"path\":\"pt_test_win.txt\",\"content\":\"l1\\nl2\\nl3\\nl4\\nl5\\n\"}");
  CHECK(has(w, "\"ok\":true"));
  std::string r = agent::toolFileRead("{\"path\":\"pt_test_win.txt\",\"offset\":2,\"limit\":2}");
  CHECK(has(r, "l2"));
  CHECK(has(r, "l3"));
  CHECK(has(r, "\"start_line\":2"));
  CHECK(has(r, "\"end_line\":3"));
  CHECK(has(r, "\"total_lines\":5"));
  CHECK(r.find("l1") == std::string::npos);
  CHECK(r.find("l4") == std::string::npos);
  std::remove("pt_test_win.txt");
}

int main() {
  testParse();
  testApply();
  testToolEdit();
  testToolPatchFile();
  testToolReadWindow();
  std::printf("checks=%d failures=%d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}