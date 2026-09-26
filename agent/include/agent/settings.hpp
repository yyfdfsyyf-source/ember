#pragma once
#include "tui/form.hpp"
#include <string>
#include <vector>

namespace agent {

struct AppConfig;
class ToolRegistry;

// Settings persistence: a JSON file (default "settings.json" next to the
// executable's working directory, overridable with --config). The UI edits
// a live copy of AppConfig through tui::Form fields; saveSettingsFile()
// writes it back, loadSettingsFile() reads it with defaults for missing keys.

// Load a settings JSON file into `cfg` (only keys present in the file are
// applied). Returns false if the file cannot be read or parsed.
bool loadSettingsFile(std::string const& path, AppConfig& cfg);

// Write `cfg` as a settings JSON file. Returns false on failure.
bool saveSettingsFile(std::string const& path, AppConfig const& cfg);

// Build the ordered Form field list (including one toggle per enabled tool).
// Field tags map back to config keys:
//   "base_url", "api_key", "model", "system_prompt", "budget",
//   "json_mode", "shell_enabled", then "tool:<name>" per tool.
std::vector<tui::Field> buildSettingsFields(AppConfig const& cfg,
                                            ToolRegistry const& tools);

// Apply the Form fields back onto cfg and tool enable flags.
void applySettingsFields(AppConfig& cfg, std::vector<tui::Field> const& fields,
                         ToolRegistry& tools);

}  // namespace agent
