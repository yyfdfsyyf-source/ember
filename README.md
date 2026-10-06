# Ember — Terminal Software-Engineering Agent

A text-first, C++20 TUI agent for software engineering inside the terminal.
The Windows binary is standalone (statically linked); the Linux binary needs
only the system `libcurl.so.4` and glibc 2.30+. Browser tools additionally
drive a headless Chrome/Edge binary over CDP.

## Current Version

0.10.0. `VERSION` is the single source of truth: every build path (CMake,
`build.ps1`, `build-linux.sh`) injects it into the binary, `agent --version`
prints it, and the TUI shows `v<version> <cols>x<rows>` in the frame foot.
Windows, macOS, and Linux are all supported.

## Quick Start

```powershell
# PowerShell (Windows)
.\build.ps1

# Or with CMake
cmake -B build -G Ninja
cmake --build build --config Release
```

The binary is `out/agent.exe` (Windows) or `out/agent`.

## Configuration

`settings.json` (created on first run or edited via the built-in settings UI):

```json
{
  "base_url": "https://api.openai.com/v1",
  "api_key_env": "OPENAI_API_KEY",
  "model": "gpt-4o-mini",
  "system_prompt": "You are Ember, a software-engineering agent.",
  "budget": 6000,
  "json_mode": false,
  "shell_enabled": true,
  "mode": "standard",
  "mcp_servers": [
    {
      "name": "filesystem",
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "C:\\src"],
      "enabled": true
    }
  ]
}
```

### API key handling

- `"api_key_env": "<ENV_VAR_NAME>"` — load the key from an environment variable at startup. The secret is never written to disk.
- `"api_key": "<raw key>"` — still supported for backward compatibility, but discouraged because `settings.json` is plain text.
- If both are present, `api_key_env` wins.

### Providers & model groups

For multiple endpoints, define named `providers` and bundle models into
`groups`; each group binds to one provider. `/model` lists them grouped, and
`/model <N|NAME>` switches the model **and** the provider (base URL + key)
automatically. Models not in any group use the top-level `base_url`/`api_key`
(the "default" connection). A group may also set `"provider": "default"` or
omit it to explicitly use that same default connection.

```json
{
  "base_url": "https://api.openai.com/v1",
  "api_key": "…",
  "model": "gpt-4o-mini",
  "thinking": "medium",
  "providers": [
    { "name": "deepseek", "base_url": "https://api.deepseek.com/v1", "api_key_env": "DEEPSEEK_KEY" },
    { "name": "ollama",   "base_url": "http://localhost:11434/v1" },
    { "name": "glm",      "base_url": "https://open.bigmodel.cn/api/paas/v4", "thinking_style": "thinking" }
  ],
  "groups": [
    { "name": "gpt",  "provider": "default",  "models": ["gpt-4o", "gpt-4o-mini"] },
    { "name": "ds",   "provider": "deepseek", "models": ["deepseek-v4-pro", "deepseek-v4-flash"] },
    { "name": "local","provider": "ollama",   "models": ["qwen2.5", "llama3.2"] },
    { "name": "glm",  "provider": "glm",      "models": ["glm-4.5"] }
  ]
}
```

- `api_key_env` on a provider resolves at startup; the secret stays out of the file.
- The top-level `base_url`/`api_key` remain the default connection for ungrouped models; switching to a grouped model never overwrites them on disk.
- `/model <prefix>` + Tab completes across all grouped + ungrouped models.

### Thinking strength

`thinking` (top level) sets how hard the model reasons: `auto` (default: send
nothing) | `none` | `minimal` | `low` | `medium` | `high` | `max`. Change it live with
`/think <level>` (shown on the frame's top edge as `[think:high]`), or at
startup with `--thinking NAME` / `AGENT_THINKING`.

Endpoints spell the same switch differently, so each provider picks one
`thinking_style` (the top-level `thinking_style` covers the default
connection):

| `thinking_style` | field sent | spoken by |
| --- | --- | --- |
| `effort` (default) | `"reasoning_effort": "high"` | OpenAI, DeepSeek, OpenRouter, most proxies |
| `thinking` | `"thinking": {"type": "enabled" \| "disabled"}` | Zhipu GLM, Moonshot Kimi |
| `enable_thinking` | `"enable_thinking": true \| false` | Qwen / DashScope / vLLM |
| `chat_template_kwargs` | `"chat_template_kwargs": {"enable_thinking": true \| false}` | Agnes AI, some vLLM deployments |
| `none` | nothing | endpoint cannot control it |

`thinking`, `enable_thinking` and `chat_template_kwargs` are binary: `/think low` on such a
provider still just turns thinking on, and Ember says so in the status line.

DeepSeek documents `reasoning_effort` as `low` / `high` / `max`; Ember sends the
level you pick verbatim, so use `/think max` for its strongest setting.

### MCP servers

`mcp_servers` is a list of stdio JSON-RPC 2.0 servers. Each entry exposes tools
under a `<name>_` prefix. Type `/mcp` inside Ember to inspect the current
connections.

## Features

- Chat with any OpenAI-compatible endpoint (OpenAI, DeepSeek, OpenRouter, custom proxies).
- Built-in tools: shell_exec, file_read, file_write, edit, patch, grep, glob, web_fetch, web_search.
- Headless browser automation (Chrome/Edge over CDP): open, click, type, screenshot, eval.
- Windows desktop automation via UI Automation (desktop_tree, desktop_click, desktop_type...).
- Reasoning / thinking models: `reasoning_content` is rendered inline; strength is chosen with `/think` (see [Thinking strength](#thinking-strength)), and models plus their providers switch through `/model`.
- Sub-agents: `subagent` (single delegation) and `subagents_parallel` (fan-out).
- PTC batch execution: feed a JSON program of tool steps to `run_code`.
- Trajectory log: append-only `traj.jsonl` with rolling archive (10 MiB by default).
- Plugin system: spawn local JSON-RPC plugin executables and register their tools.

## Releases

Grab the packages from the
[latest GitHub Release](https://github.com/yyfdfsyyf-source/ember/releases/latest);
the same bytes are also committed in-tree under
[`releases/`](releases/) with the SHA256 listed in the release notes.

- `Ember-<ver>-win64.zip` — unzip and run `agent.exe`
- `ember-<ver>-linux-x86_64.tar.gz` — `tar xzf` it, `chmod +x ember verify.sh`,
  then run `./ember` (needs the system `libcurl.so.4` and glibc 2.30+)

The Linux package also ships `verify.sh`, `LICENSE` and the offline test
binaries, so `sh verify.sh` tells you on the spot whether that machine can run
Ember — it needs no network and no API key.

Or build from source: `tools/package/pack.ps1` (Windows) and
`STAGE=1 BUILD_TESTS=1 ./build-linux.sh` (Linux).

## License

Copyright (c) 2026 YYFDFS

Ember is licensed under the **GNU AGPL-3.0-only** (see `LICENSE`). In plain
terms: you may run, study, modify and redistribute Ember, but if you distribute
a modified version — or let people use Ember over a network, including through
its `--serve` protocol or the desktop app — you must release the corresponding
source under the same license. That is deliberate: Ember is meant to stay open,
including as a hosted service.

Everything under `third_party/` is written for this project and carries no
upstream license restrictions.
