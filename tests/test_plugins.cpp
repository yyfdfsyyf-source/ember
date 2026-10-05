// Plugin system: starts examples/plugin_echo (built by the build scripts) as a
// child process, discovers its tools and calls them through ToolRegistry.
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
    std::string e = dead.start("definitely_no_such_plugin.exe");
    CHECK(!e.empty());
    CHECK(!dead.running());
  }

  std::string exe = findBuilt("plugin_echo");
  if (exe.empty()) {
    std::printf("plugin_echo not built; skipping plugin checks\n");
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
  }

  ToolRegistry reg;
  {
    PluginProcess p;
    std::string e = p.start(exe);
    CHECK(e.empty());
    CHECK(p.running());
    e = p.registerTools(reg);
    CHECK(e.empty());
    if (e.empty()) {
      // Both advertised tools are now callable.
      std::string r = reg.run("echo_text", "{\"text\":\"hello\"}");
      CHECK(has(r, "echo: hello"));
      r = reg.run("add_numbers", "{\"a\":20,\"b\":22}");
      CHECK(has(r, "42"));
      // toolsJson advertises plugin tools too.
      CHECK(has(reg.toolsJson(), "add_numbers"));
      // Invalid args are rejected.
      r = reg.run("echo_text", "not json");
      CHECK(has(r, "must be a JSON object"));
      // Unknown tool on the plugin side surfaces as an error.
      r = reg.run("add_numbers", "{\"a\":1,\"b\":2,\"c\":3}");
      CHECK(!has(r, "42"));
    }
    // stop() terminates the child; further calls fail cleanly.
    p.stop();
    CHECK(!p.running());
    reg = ToolRegistry();  // drop stale tools for the next block
  }

  // --- dir loader ------------------------------------------------------
  {
    std::filesystem::create_directories("out/tmp_plugin_dir");
    std::filesystem::copy_file(exe, "out/tmp_plugin_dir/echo2.exe",
                               std::filesystem::copy_options::overwrite_existing);
    // A non-plugin exe must be ignored silently (it fails the handshake).
    std::filesystem::copy_file(exe, "out/tmp_plugin_dir/echo2_copy_evil.exe",
                               std::filesystem::copy_options::overwrite_existing);
    ToolRegistry reg2;
    std::vector<std::unique_ptr<PluginProcess>> plugs;
    std::string e = startPluginsFromDir("out/tmp_plugin_dir", reg2, plugs);
    CHECK(e.empty());
    CHECK(reg2.find("echo_text") != nullptr);
    CHECK(reg2.find("idont_exist") == nullptr);
    std::string r = reg2.run("echo_text", "{\"text\":\"from dir\"}");
    CHECK(has(r, "echo: from dir"));
    plugs.clear();  // stop the children before removing their exes
    std::filesystem::remove_all("out/tmp_plugin_dir");
  }

  std::printf("checks=%d failures=%d\n", checks, failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}