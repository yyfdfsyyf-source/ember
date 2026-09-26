// Smoke test for the desktop accessibility (UI Automation) tools.
// These exercise the real Windows UIA stack, so they are inherently
// environment-dependent: the checks are deliberately loose (JSON parses,
// the ok flag is set, windows exist on a real desktop session).
#include "agent/tools.hpp"
#include "agent/uia.hpp"
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

static bool hasKey(std::string const& s, char const* key) {
  std::string needle = "\"" + std::string(key) + "\"";
  return s.find(needle) != std::string::npos;
}

int main() {
  // desktop_windows: JSON with ok:true and a windows array on a real desktop.
  {
    std::string r = agent::toolDesktopWindows("{}");
    CHECK(!r.empty());
    CHECK(hasKey(r, "ok"));
    // Either there are windows, or the tool reports an error (headless).
    CHECK(hasKey(r, "windows") || hasKey(r, "error"));
    std::printf("desktop_windows -> %d bytes\n", (int)r.size());
  }

  // desktop_tree on the focused window: either dumps a tree or errors cleanly.
  {
    std::string r = agent::toolDesktopTree("{\"window\":\"focused\"}");
    CHECK(!r.empty());
    CHECK(hasKey(r, "tree") || hasKey(r, "error"));
    std::printf("desktop_tree focused -> %d bytes\n", (int)r.size());
  }

  // desktop_key with an unknown key must error, not hang.
  {
    std::string r = agent::toolDesktopKey("{\"keys\":\"not_a_key_zzz\"}");
    CHECK(hasKey(r, "error"));
  }

  // desktop_click with no target must error.
  {
    std::string r = agent::toolDesktopClick("{}");
    CHECK(hasKey(r, "error"));
  }

  if (failures == 0) {
    std::printf("ALL DESKTOP-UIA CHECKS PASSED (%d checks)\n", checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", failures, checks);
  return 1;
}