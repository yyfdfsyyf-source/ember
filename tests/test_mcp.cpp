// MCP client: starts examples/mcp_echo (a real MCP stdio server), registers
// its tools under a "<name>_" prefix and calls them through ToolRegistry.
#include "agent/plugins.hpp"
#include "agent/tools.hpp"
#include <cstdio>
#include <filesystem>
#include <memory>
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

// build.ps1 leaves the helper exes in out/, the Linux cross-build stages them
// next to this binary, and neither spelling has an .exe suffix on POSIX.
static std::string findBuilt(std::string const& stem) {
  std::string const cands[] = {"out/" + stem + ".exe", "out/" + stem, stem + ".exe", stem};
  for (std::string const& c : cands) {
    if (std::filesystem::is_regular_file(c)) return c;
  }
  return "";
}

int main() {
  using namespace agent;

  // Missing executable -> clean error, nothing crashes.
  {
    PluginProcess dead;
    McpConfig cfg;
    cfg.name = "nope";
    cfg.command = "definitely_no_such_mcp_server.exe";
    std::string e = dead.startMcp(cfg);
    CHECK(!e.empty());
    CHECK(!dead.running());
  }

  std::string exe = findBuilt("mcp_echo");
  if (exe.empty()) {
    std::printf("mcp_echo not built; skipping MCP checks\n");
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
  }

  ToolRegistry reg;
  {
    PluginProcess m;
    McpConfig cfg;
    cfg.name = "test";
    cfg.command = exe;
    cfg.env = {{"MCP_TEST_ENV", "works"}};
    std::string e = m.startMcp(cfg);
    CHECK(e.empty());
    CHECK(m.running());
    e = m.registerTools(reg, cfg.name + "_");
    CHECK(e.empty());
    if (e.empty()) {
      CHECK(m.toolCount() == 4);
      // Tools are prefixed to avoid collisions with built-ins.
      CHECK(reg.find("test_echo_text") != nullptr);
      CHECK(reg.find("test_add_numbers") != nullptr);
      CHECK(reg.find("test_env_echo") != nullptr);
      CHECK(reg.find("test_always_error") != nullptr);
      CHECK(reg.find("echo_text") == nullptr);  // not exposed bare
      // Advertised (prefixed) in toolsJson.
      CHECK(has(reg.toolsJson(), "test_echo_text"));
      // Calls are forwarded to the MCP server.
      std::string r = reg.run("test_echo_text", "{\"text\":\"hi\"}");
      CHECK(has(r, "echo: hi"));
      r = reg.run("test_add_numbers", "{\"a\":20,\"b\":22}");
      CHECK(has(r, "42"));
      // Extra env reaches the server process.
      r = reg.run("test_env_echo", "{}");
      CHECK(has(r, "works"));
      // isError results come back wrapped as an error.
      r = reg.run("test_always_error", "{}");
      CHECK(has(r, "boom"));
      CHECK(has(r, "error"));
      // Invalid args are rejected.
      r = reg.run("test_echo_text", "not json");
      CHECK(has(r, "must be a JSON object"));
    }
    m.stop();
    CHECK(!m.running());
  }

  std::printf("checks=%d failures=%d\n", checks, failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}