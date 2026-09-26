# Ember 插件系统 — 设计方案与实施计划

> 2026-08-20 · 目标：让 Ember（C++20，零依赖单二进制 TUI agent）拥有 **dsh 式插件能力**：
> 插件可覆盖内置工具、挂生命周期钩子、提供 skill/提示词包、可安装管理，同时通过协议接入
> DeepSeek Harness（dsh）与 MCP 两大外部插件生态。

---

## 1. 背景与目标

用户原需求："我想搞个 dsh 一样的插件系统"。经研究澄清：**真正的 DeepSeek Harness 是独立产品**（`deepseek-ai/deepseek-harness`），本仓库 `reference/deepseek-harness` 目录副本实为 `windless/deepseek-harness`——一个 **opencode 插件**（汉化 opencode 内置工具的），非官方产品。

最终目标拆解为三轨：

| 轨道 | 能力 | 说明 |
|---|---|---|
| A | 原生插件协议扩展 | override / hooks / skills / 插件管理，零新依赖 |
| B | dsh 客户端 | spawn 超级精简 dsh，委托其整棵 Cordis 插件树 |
| C | MCP 客户端 | stdio 接入全世界 MCP 服务器生态 |

**已确认的决策**：
- MCP 客户端：**要**
- dsh 接入：**spawn 超级精简版本**（headless + 自定义最小 profile）
- 轨道 A：**四步全做**
- opencode 插件直读翻译器：**放弃**（需 Bun 旁路桥仿真 `@opencode-ai/plugin`，API 漂移，不划算）

---

## 2. 生态研究结论

### 2.1 官方 DeepSeek Harness（dsh）
- 2026-08-13 发布 v0.1 developer preview，MIT，`deepseek-ai/deepseek-harness`，npm 包 `@deepseek-ai/dsh`
- 基于 **Cordis** 插件框架，"一切皆插件"（模型/工具/技能/会话/沙箱/存储/**Agent 循环**/UI 全部可换），无特权核心
- 运行：`npx @deepseek-ai/dsh web` → Web UI `127.0.0.1:3080`；`dsh --profile headless "任务"` 一次性任务；要求 Node `^22.19 || >=24`
- 四种模式：标准 / PTC（模型写 TS 程序编排多轮工具）/ 极简（仅 bash+str_replace_editor）/ 创造（运行时改插件组合）
- 会话数据面：append-only 事件溯源日志，"模型可见 ⟺ 已记录"，Trajectory/恢复/分叉/回放同源
- 生态：插件市场社区站（dshmarketplace.dev、dsharness.io 等），GitHub topic `dsh-plugin`，1600+ 插件
- 已知问题：minimal 模式持久 bash 硬编码 `/bin/bash`，**Windows 不可用**（社区有 `dsh-preset-minimal-windows` workaround）；fast mode 假定 Linux；v0.1 会破坏性变更

### 2.2 opencode 插件
- 进程内 TS/JS 模块（`.opencode/plugin/*.ts` 或 npm 包），跑在 opencode 的 **Bun** 运行时
- 钩子：`tool.definition`（改工具定义）、`tool.execute.after`、`chat.*`、`experimental.session.compacting` + 25+ 事件
- 外部接口：HTTP server API、`@opencode-ai/sdk`、`opencode-mcp`（桥成 MCP 服务器）、ACP 支持
- **结论**：插件文件无法被 C++ 直读；直读需 Bun 旁路桥仿真其 SDK，已放弃

### 2.3 dsh 的对外协议（轨道 B 的核心依据）
来自 `packages/sdk/protocol/README.md`：
- 传输：`JsonRpcLineTransport` = **换行分隔 JSON-RPC 2.0，每行一个紧凑 JSON**（与 Ember 现有 `PluginProcess` 同形态）
- 帧规则：`id`+`method`=请求；`id`=响应；`method`=通知；畸形行忽略
- 服务端：`dsh-sdk-jsonrpc-server` 插件；客户端：`dsh-sdk-client`（TS）、Python SDK
- 方法表：
  - 客户端→服务端：`initialize`、`session/prompt`（持久 enqueue 回执）、`shutdown`
  - 服务端→客户端：`session.event`（全会话事件流，不筛选）、`session.status`（agent running/idle）、`subagent.started` / `subagent.finished`
- 特性：无协议版本协商（`serverInfo.version`=0.0.1 未校验）；无 cancel/会话关闭方法（放弃=杀进程）；服务端不发客户端请求
- **本质**：session 型协议（发任务、收事件流），非工具列表型 → dsh 作为**委托子 agent**，不并入 ToolRegistry

### 2.4 MCP（轨道 C 的依据）
- stdio 传输 = 换行分隔 JSON-RPC 2.0（与 Ember 同形态）
- 方法：`initialize` → `notifications/initialized` → `tools/list` → `tools/call`
- 配置形状（对齐 opencode / dsh-mcp-client）：`{name: {command, args, env}}`
- MCP 工具会占上下文 → 每服务器可启停

---

## 3. 三轨架构总览

```
Ember (C++)
├─ A 原生插件   扩展现有 JSON-RPC 协议(override/hooks/skills/管理)   ← 自建插件
├─ B dsh 客户端 NDJSON JSON-RPC 2.0 → spawn 超级精简 dsh            ← dsh 插件树(委托 agent)
└─ C MCP 客户端 NDJSON JSON-RPC 2.0 → stdio MCP 服务器              ← opencode/dsh 同族/全世界 MCP
```

三个传输全是"spawn 子进程 + 换行分隔 JSON-RPC 2.0" → **抽公共 `RpcProcess` 基类，三轨复用**。

---

## 4. 轨道 A — 原生插件协议扩展（四步全做）

现有协议（`agent/include/agent/plugins.hpp`、`agent/src/plugins.cpp`）：
`initialize → tools/list → tools/call`，一行一个 JSON，工具 `run` 同步转发到子进程。

### 4.1 工具覆盖 override
- `tools/list` 每个 entry 增加可选字段：`target`（要覆盖的内置工具名）、`replace`（true=实现也换到本插件，false=只改元数据）
- `ToolRegistry::override()`（`agent/include/agent/tools.hpp`）；`toolsJson()` 用覆盖后元数据；`makeToolSubset` 不变

### 4.2 生命周期钩子
- 协议新增：`hooks/list`（广告 `tool.before`/`tool.after`）、`events/emit`（宿主→插件通知）
- `App::execTool`（`agent/src/app.cpp:791`）在 `tools_.run` 前后发通知：`{tool, arguments, ok, duration_ms, result}`
- 效果：插件可落 `tool-call-fail-日期.jsonl` 失败统计、缓存、审计（对齐 dsh）

### 4.3 Skills / 提示词包
- 插件通过 `capabilities` 广告 markdown skill bundle（纯文本）
- 回合开始注入系统提示词（复用 `rulesPath` / AGENTS.md 注入点）

### 4.4 插件管理
- `AppConfig.plugins` 列表（本地目录 / `git:url` / url）；插件目录可带 `plugin.json` manifest（入口 exe + skills + 版本）
- 新增 `/plugin add|remove|list|reload` 命令（git 用 `git clone`，零新依赖）；settings UI 加字段
- 兼容 `--plugins DIR`（`agent/src/main.cpp:129`）

---

## 5. 轨道 B — dsh 客户端

- **超级精简 dsh**：headless profile 本无 Web/HTTP/browser（`dsh-base`+`dsh-headless`）；叠自定义最小 profile（加 `dsh-sdk-jsonrpc-server` 插件 + 极简工具集）；`--profile <mini>` 首次自动初始化，`--patch` 可覆盖
- 启动：Ember spawn `npx @deepseek-ai/dsh --profile <mini>`（Windows 注意 `npx.cmd`），stdio 直连
- 协议：`initialize` → `session/prompt`（发任务）→ 流式收 `session.event` / `session.status` 通知，映射为 Ember 的 `StreamEvent`（`agent/include/agent/types.hpp`）
- **定位**：dsh 作为委托子 agent（发任务、流式收结果），不并入 ToolRegistry——让 dsh 自己调它的插件树
- 生命周期：配置启用才 spawn；退出 kill（`PluginProcess::stop` 已有 TerminateProcess/SIGTERM）
- Windows 注意：minimal 模式 bash 工具 Windows 不可用 → 自定义 profile 用 Windows 友好工具

---

## 6. 轨道 C — MCP 客户端

- 新增 `agent/src/mcp.cpp` / `agent/include/agent/mcp.hpp`：MCP stdio 客户端，复用 `RpcProcess`
- 方法：`initialize`、`notifications/initialized`、`tools/list`、`tools/call`
- `AppConfig.mcpServers` = `[{name, command, args, env}]`
- 工具并入 `ToolRegistry`（前缀防冲突）；每服务器可启停防 token 膨胀

---

## 7. 跨轨共享改造

- **抽公共传输基类 `RpcProcess`**：把 `PluginProcess` 的 spawn + NDJSON + 请求/通知/超时抽出来（原生插件 / dsh / MCP 三用）
- `ToolRegistry::override()`；`makeToolSubset`（`agent/src/app.cpp:801`）扩展：插件/MCP 工具进**初始子集**（现状只含 `kCore` + 本会话用过的）
- `App::execTool` 钩子 + 会话事件转发
- `AppConfig`（`agent/include/agent/app.hpp:32`）+ settings UI + `/plugin`、`/mcp`、`/dsh` 命令

---

## 8. 测试

- `tests/test_plugins.cpp` 扩展：override / hooks / skills
- `tests/test_mcp.cpp`：fake stdio MCP server
- `tests/test_dsh.cpp`：协议级 mock（initialize / session.prompt / session.event）
- `tests/test_tools.cpp` 补 override 用例

---

## 9. 里程碑顺序

1. **抽 `RpcProcess` 基类 + 协议扩展（override/hooks/skills）+ 测试** → 轨道 A 1-3
2. **插件管理**（`/plugin`、config、settings UI）→ 轨道 A 4
3. **MCP 客户端 + fake server 测试** → 轨道 C
4. **dsh 客户端**（spawn 精简 profile + 事件流映射）+ mock 测试 → 轨道 B

每步独立可交付、可验证（build.ps1 + 对应测试 exe）。

---

## 10. 实现时需核实的点

- `packages/sdk/server/README.md`：jsonrpc-server 插件的确切 profile 组合与启动方式
- Windows 下 spawn `npx`（`npx.cmd`）的处理
- minimal 模式 Windows bash 限制 → 自定义 profile 工具取舍
- MCP stdio 消息边界（是否严格每行一帧）

---

## 11. 明确不做

- opencode 插件直读翻译器（Bun 旁路桥，已放弃）
- 嵌入式脚本引擎（LuaJIT/QuickJS）、进程内热装卸、PTC/模型自改 harness
- dsh 插件包直读（需重写 Cordis 运行时；走 dsh 自带 JSON-RPC 服务器即可）

---

## 12. 参考资料

- 官方 dsh 仓库：https://github.com/deepseek-ai/deepseek-harness
- dsh SDK 线协议：`packages/sdk/protocol/README.md`
- dsh CLI：https://deepseekdocs.com/en/docs/user-guide/cli
- dsh 插件生态：https://github.com/topics/dsh-plugin 、dsharness.io、dshmarketplace.dev、dshplugin.dev
- 极简模式 Windows 替代：https://github.com/zeroa234/dsh-preset-minimal-windows
- opencode 插件文档：https://opencode.ai/docs/plugins 、MCP: https://opencode.ai/docs/mcp-servers
- Ember 现有插件实现：`agent/src/plugins.cpp`、`agent/include/agent/plugins.hpp`
- 本仓库目录副本（非官方，仅参考）：`reference/deepseek-harness` = `windless/deepseek-harness`