#pragma once
#include "minijson.hpp"
#include <string>

namespace agent {

// Headless browser session backed by a real Chrome/Edge binary driven over the
// Chrome DevTools Protocol (WebSocket via WinHTTP on Windows, raw sockets
// elsewhere). Lets the agent do what a human does in a browser: navigate,
// click, type, keyboard, scroll, forms, uploads, screenshots, arbitrary JS.
//
// A single process-wide session is shared by all browser_* tools.
class BrowserSession {
 public:
  BrowserSession();
  ~BrowserSession();
  BrowserSession(BrowserSession const&) = delete;
  BrowserSession& operator=(BrowserSession const&) = delete;

  bool running() const;
  // True if the last CDP exchange failed because the renderer died; the next
  // ensureStarted() restarts cleanly and restores lastUrl_.
  bool crashed() const;
  // Launch headless Chrome/Edge if needed. Returns "" on success, otherwise a
  // human-readable reason.
  std::string ensureStarted();
  // Send a CDP command. Returns the command's `result` object, or an object
  // with a single "error" string key on transport/DevTools failure.
  mini::Value call(std::string const& method, mini::Value params);
  // Runtime.evaluate convenience; same error convention as call().
  mini::Value evaluate(std::string const& expression, bool awaitPromise = true);
  // Navigate and wait (poll document.readyState == "complete", up to waitMs).
  // Returns "" on success or an error description.
  std::string navigateAndWait(std::string const& url, int waitMs);
  // Poll document.readyState == "complete" on the current page (no
  // navigation). Returns "" when the page settled or an error description.
  std::string waitReady(int waitMs = 15000);
  std::string currentUrl();
  std::string currentTitle();
  // Compact accessibility-style outline of the current page. Returns an
  // object {"error": ...} or {"lines": N, "snapshot": "...", "truncated": b}.
  mini::Value snapshotOutline(int maxChars = 12000, int maxLines = 400);
  // Detach local files from file inputs. Chromium's renderer can crash when a
  // page whose <input type=file> still holds uploaded files is reloaded or
  // navigated; clear them before any navigation.
  void clearFileInputs();
  // True if any file input on the page currently holds an uploaded file. Such
  // a page may crash the Chromium renderer on navigation; callers should
  // relaunch the session instead of reloading it.
  bool hasAttachedFiles();
  // Kill the browser process, close handles and remove the profile dir.
  void stop();

 private:
  struct Impl;
  Impl* state_ = nullptr;
  // Last successfully navigated URL. Lives on the session (not Impl) so crash
  // recovery can restore the page after a stop()+relaunch.
  std::string lastUrl_;
};

// Process-wide browser session shared by the browser_* tools.
BrowserSession& browserSession();

}  // namespace agent