// Tests for the tool registry + built-in tools (shell/file/web).
#include "agent/mode.hpp"
#include "agent/tools.hpp"
#include <cstdio>
#include <cstring>

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

int main() {
  agent::ToolRegistry reg;
  registerBuiltinTools(reg);

  // registry basics
  CHECK(reg.find("shell_exec") != nullptr);
  CHECK(reg.find("file_read") != nullptr);
  CHECK(reg.find("file_write") != nullptr);
  CHECK(reg.find("file_list") != nullptr);
  CHECK(reg.find("web_fetch") != nullptr);
  CHECK(reg.find("verify") != nullptr);
  CHECK(reg.find("remember") != nullptr);
  CHECK(reg.find("recall") != nullptr);
  CHECK(reg.find("subagent_parallel") != nullptr);
  CHECK(reg.find("web_search") != nullptr);
  CHECK(reg.find("nope") == nullptr);

#ifdef _WIN32
  // desktop accessibility (UI Automation) tools are registered; on other
  // platforms they are deliberately absent (no UIA backend, no CDP client).
  CHECK(reg.find("desktop_windows") != nullptr);
  CHECK(reg.find("desktop_tree") != nullptr);
  CHECK(reg.find("desktop_click") != nullptr);
  CHECK(reg.find("desktop_type") != nullptr);
  CHECK(reg.find("desktop_key") != nullptr);
  CHECK(reg.find("desktop_scroll") != nullptr);
  CHECK(reg.find("desktop_wait") != nullptr);
#else
  CHECK(reg.find("desktop_windows") == nullptr);
  CHECK(reg.find("browser_open") == nullptr);
#endif

  // HTML stripping used by web_fetch: scripts/styles gone, tags gone, text kept
  {
    std::string html =
        "<html><head><style>.x{color:red}</style><script>var a=1;</script></head>"
        "<body><h1>Hello</h1><script>var b=2;</script><p>World &amp; more</p>"
        "<style>p{}</style></body></html>";
    std::string txt = agent::htmlBodyToText(html);
    CHECK(txt.find("Hello") != std::string::npos);
    CHECK(txt.find("World & more") != std::string::npos);
    CHECK(txt.find("script") == std::string::npos);
    CHECK(txt.find("style") == std::string::npos);
    CHECK(txt.find("color") == std::string::npos);
    CHECK(txt.find("var a") == std::string::npos);
  }

  std::string tj = reg.toolsJson();
  CHECK(!tj.empty());
  CHECK(tj.find("shell_exec") != std::string::npos);
  CHECK(tj.find("web_fetch") != std::string::npos);

  // subset restriction
  std::string sub = reg.toolsJson({"file_read"});
  CHECK(sub.find("file_read") != std::string::npos);
  CHECK(sub.find("shell_exec") == std::string::npos);

  // disable
  reg.setEnabled("shell_exec", false);
  CHECK(reg.toolsJson().find("shell_exec") == std::string::npos);
  CHECK(reg.run("shell_exec", "{}").find("disabled") != std::string::npos);
  reg.setEnabled("shell_exec", true);

  // unknown tool -> JSON error
  CHECK(reg.run("does_not_exist", "{}").find("unknown") != std::string::npos);

  // file_write + file_read round-trip
  std::string out = reg.run("file_write", "{\"path\":\"agent_test_tmp.txt\",\"content\":\"hello M4\"}");
  CHECK(out.find("\"ok\":true") != std::string::npos);
  out = reg.run("file_read", "{\"path\":\"agent_test_tmp.txt\"}");
  CHECK(out.find("hello M4") != std::string::npos);
  CHECK(out.find("\"bytes\":8") != std::string::npos);

  // file_list finds the temp file
  out = reg.run("file_list", "{\"path\":\".\"}");
  CHECK(out.find("agent_test_tmp.txt") != std::string::npos);

  // file errors
  CHECK(reg.run("file_read", "{\"path\":\"no_such_file_xyz\"}").find("error") != std::string::npos);
  CHECK(reg.run("file_write", "{}").find("error") != std::string::npos);

  // shell_exec runs a command
  out = reg.run("shell_exec", "{\"command\":\"echo m4-shell-ok\"}");
  CHECK(out.find("m4-shell-ok") != std::string::npos);
  CHECK(out.find("\"exit_code\":0") != std::string::npos);

  // shell_exec error path
  out = reg.run("shell_exec", "{}");
  CHECK(out.find("error") != std::string::npos);

  // web_fetch bad URL -> error (no server running in tests)
  out = reg.run("web_fetch", "{\"url\":\"http://127.0.0.1:1/nope\"}");
  CHECK(out.find("error") != std::string::npos);

  // truncation
  std::string big;
  for (int i = 0; i < 100; i++) big += "0123456789";
  std::string wj = "{\"path\":\"agent_test_tmp_big.txt\",\"content\":\"" + big + "\"}";
  reg.run("file_write", wj);
  out = reg.run("file_read", "{\"path\":\"agent_test_tmp_big.txt\",\"max_chars\":50}");
  CHECK(out.find("truncated") != std::string::npos);

  // verify: explicit command echo returns ok
  out = reg.run("verify", "{\"command\":\"echo m4-verify-ok\"}");
  CHECK(out.find("\"exit_code\":0") != std::string::npos);
  CHECK(out.find("m4-verify-ok") != std::string::npos);
  // verify: failing command reports ok:false
  out = reg.run("verify", "{\"command\":\"exit 3\"}");
  CHECK(out.find("\"ok\":false") != std::string::npos);
  CHECK(out.find("\"exit_code\":3") != std::string::npos);

  // remember / recall lesson memory (out/lessons.md in cwd)
  std::remove("out/lessons.md");
  out = reg.run("remember", "{\"lesson\":\"Always verify after editing.\"}");
  CHECK(out.find("\"ok\":true") != std::string::npos);
  out = reg.run("recall", "{}");
  CHECK(out.find("Always verify after editing.") != std::string::npos);
  CHECK(out.find("\"found\":true") != std::string::npos);
  // remember errors
  out = reg.run("remember", "{}");
  CHECK(out.find("error") != std::string::npos);

  // mode helpers
  CHECK(agent::modeFromString("standard") == agent::Mode::Standard);
  CHECK(agent::modeFromString("minimal") == agent::Mode::Minimal);
  CHECK(agent::modeFromString("ptc") == agent::Mode::Ptc);
  CHECK(agent::modeFromString("creator") == agent::Mode::Creator);
  CHECK(agent::modeFromString("bogus") == agent::Mode::Standard);
  CHECK(agent::modeToString(agent::Mode::Ptc) == "ptc");
  CHECK(agent::modeAllowsTool(agent::Mode::Minimal, "shell_exec"));
  CHECK(agent::modeAllowsTool(agent::Mode::Minimal, "edit"));
  CHECK(!agent::modeAllowsTool(agent::Mode::Minimal, "web_fetch"));
  CHECK(agent::modeAllowsTool(agent::Mode::Standard, "web_fetch"));
  {
    auto ex = agent::modeExtraTools(agent::Mode::Ptc);
    CHECK(ex.size() == 1 && ex[0] == "run_code");
    ex = agent::modeExtraTools(agent::Mode::Creator);
    CHECK(ex.size() == 2 && ex[0] == "list_tools" && ex[1] == "get_config");
    CHECK(agent::modeExtraTools(agent::Mode::Standard).empty());
    CHECK(!agent::modeInstructions(agent::Mode::Minimal).empty());
    CHECK(!agent::modeInstructions(agent::Mode::Ptc).empty());
    CHECK(agent::modeInstructions(agent::Mode::Standard).empty());
  }

  // run_code: sequential program with $var interpolation
  {
    agent::ToolRegistry rc;
    mini::Value none = mini::Value::makeObject();
    rc.add({.name = "echo",
            .description = "echo text",
            .parameters = none,
            .run = [](std::string const& a) {
              mini::Value v;
              mini::tryParse(a, v);
              return v.has("text") ? v.get("text")->asString() : "";
            }});
    rc.add({.name = "shout",
            .description = "uppercase text",
            .parameters = none,
            .run = [](std::string const& a) {
              mini::Value v;
              mini::tryParse(a, v);
              std::string s = v.has("text") ? v.get("text")->asString() : "";
              for (auto& c : s) c = (char)std::toupper((unsigned char)c);
              return s;
            }});

    out = agent::toolRunCode(rc,
                             "{\"steps\":[{\"tool\":\"echo\",\"arguments\":{\"text\":\"hi\"},"
                             "\"var\":\"a\"},{\"tool\":\"shout\",\"arguments\":{\"text\":"
                             "\"{$a}!\"}}]}");
    CHECK(out.find("\"ok\":true") != std::string::npos);
    CHECK(out.find("\"output\":\"HI!\"") != std::string::npos);

    // exact "$var" reference
    out = agent::toolRunCode(rc,
                             "{\"steps\":[{\"tool\":\"echo\",\"arguments\":{\"text\":\"xy\"},"
                             "\"var\":\"v\"},{\"tool\":\"shout\",\"arguments\":{\"text\":\"$v\"}}]}");
    CHECK(out.find("\"output\":\"XY\"") != std::string::npos);

    // parallel batch
    out = agent::toolRunCode(rc,
                             "{\"parallel\":true,\"max_parallel\":2,\"steps\":["
                             "{\"tool\":\"echo\",\"arguments\":{\"text\":\"p1\"}},"
                             "{\"tool\":\"shout\",\"arguments\":{\"text\":\"p2\"}}]}");
    CHECK(out.find("\"ok\":true") != std::string::npos);
    CHECK(out.find("\"steps\":2") != std::string::npos);

    // failing step propagates ok:false
    out = agent::toolRunCode(rc,
                             "{\"steps\":[{\"tool\":\"echo\",\"arguments\":{\"text\":\"x\"},"
                             "\"var\":\"a\"},{\"tool\":\"missing_tool\",\"arguments\":{}}]}");
    CHECK(out.find("\"ok\":false") != std::string::npos);
    CHECK(out.find("unknown") != std::string::npos);

    // malformed program
    out = agent::toolRunCode(rc, "{}");
    CHECK(out.find("error") != std::string::npos);
    out = agent::toolRunCode(rc, "not json");
    CHECK(out.find("error") != std::string::npos);
  }

  std::remove("agent_test_tmp.txt");
  std::remove("agent_test_tmp_big.txt");

  // ---- workspace policy helpers ----
  {
    std::string root = agent::normalizeWorkspacePath("out");
    CHECK(!root.empty());
    CHECK(root.back() != '/');                       // trailing slash trimmed
    CHECK(agent::pathWithinNormalizedRoot(root, root));                    // self
    CHECK(agent::pathWithinNormalizedRoot(root, root + "/a/b.txt"));       // child
    CHECK(!agent::pathWithinNormalizedRoot(root, root + "x"));             // sibling
    CHECK(agent::pathWithinNormalizedRoot("/a", "/a/b"));
    CHECK(!agent::pathWithinNormalizedRoot("/a", "/ab/c"));
    // ".." must collapse and can escape the root
    std::string esc = agent::normalizeWorkspacePath("out/../secret");
    CHECK(!agent::pathWithinNormalizedRoot(root, esc));
    std::string inner = agent::normalizeWorkspacePath("out/./sub/../x");
    CHECK(agent::pathWithinNormalizedRoot(root, inner));
  }
  {
    auto t = agent::toolPathTargets("file_read", "{\"path\":\"out/a.txt\"}");
    CHECK(t.size() == 1 && t[0] == "out/a.txt");
    auto none = agent::toolPathTargets("file_read", "{\"path\":\"\"}");
    CHECK(none.empty());
    // shell: absolute + relative-with-sep tokens become candidates; flags don't
    auto sh = agent::toolPathTargets(
        "shell_exec",
        "{\"command\":\"copy /Y src\\\\a.txt C:\\\\Windows\\\\b.txt && echo hi\"}");
    bool hasSrc = false, hasAbs = false, hasEcho = false;
    for (auto const& s : sh) {
      if (s.find("src") != std::string::npos) hasSrc = true;
      if (s[1] == ':') hasAbs = true;
      if (s == "hi") hasEcho = true;
    }
    CHECK(hasSrc);
    CHECK(hasAbs);
    CHECK(!hasEcho);   // bare word not a path
    // quoted absolute path is unquoted before scanning
    auto q = agent::commandPathCandidates("\"C:\\Program Files\\x\" run");
    bool quoted = false;
    for (auto const& s : q)
      if (s == "C:\\Program Files\\x") quoted = true;
    CHECK(quoted);
    CHECK(agent::toolPathTargets("web_fetch", "{\"url\":\"http://x\"}").empty());
  }
  {
    // registry guard blocks a call and the message reaches the model
    agent::ToolRegistry reg;
    agent::registerBuiltinTools(reg);
    int calls = 0;
    reg.setGuard([&](std::string const& tool, std::string const& args) {
      calls++;
      agent::ToolRegistry::GuardResult r;
      if (tool == "file_write" && args.find("outside") != std::string::npos) {
        r.verdict = agent::ToolRegistry::GuardVerdict::Deny;
        r.message = "denied-outside";
      }
      return r;
    });
    std::string out = reg.run("file_write", "{\"path\":\"/tmp/outside\",\"content\":\"x\"}");
    CHECK(out.find("denied-outside") != std::string::npos);
    CHECK(calls == 1);
    // allowed path: guard consulted, tool actually runs (file created under out)
    std::string out2 = reg.run("file_write", "{\"path\":\"agent_ws_ok.txt\",\"content\":\"x\"}");
    CHECK(calls == 2);
    CHECK(out2.find("denied-outside") == std::string::npos);
    std::remove("agent_ws_ok.txt");
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
