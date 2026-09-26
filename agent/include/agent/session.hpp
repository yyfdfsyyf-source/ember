#pragma once
#include "agent/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace agent {

// Persist/restore a conversation to a JSON file (used by --session).
// Returns false on I/O or parse failure; never throws. `title` (optional,
// e.g. the truncated first user message) is stored in the file header and
// shown by the session browser.
bool saveSession(std::string const& path, std::vector<Message> const& messages,
                 std::string const& model, std::string const& title = "");
// Restore a conversation from a JSON file. Returns 1 on success; 0 if the
// file cannot be opened; -1 on JSON parse failure; -2 if the "messages"
// field is missing. Individual malformed messages are skipped, not fatal.
int loadSession(std::string const& path, std::vector<Message>& messages, std::string& model);

// A conversation archive: <sessionDir>/<id>.json. `listSessions` scans a
// directory for such files without loading their message bodies.
struct SessionInfo {
  std::string id;       // file stem (stable name)
  std::string path;     // full file path
  std::string title;    // header title, may be empty
  std::string model;
  int64_t savedAt = 0;
  int msgCount = 0;
  bool current = false;  // caller sets this for the active session
};

// Scan `dir` for *.json session files, newest first. Returns false if the
// directory is missing or unreadable (never throws).
bool listSessions(std::string const& dir, std::vector<SessionInfo>& out);

// Generate a fresh session id string (e.g. "sess-20260815-153000").
std::string makeSessionId();

}  // namespace agent
