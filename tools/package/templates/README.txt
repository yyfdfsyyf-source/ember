Ember @VERSION@ - 文本优先 CLI Agent(内测版)
============================================

Ember 是一个运行在终端里的命令行 AI 智能体:TUI 界面、OpenAI
兼容 / Ollama 后端、集成了 shell、文件、代码搜索、网页、浏览器、
后台任务、用量与成本统计等功能。本包为内测版,单文件便携,无需安装。

@VERSION@ 更新(相对 0.9.1)
------------------------------------------------------------
- 新增:版本号单一来源。仓库根的 VERSION 是唯一真源,构建时写入二进制:
  agent.exe --version(或 -v)打印版本;欢迎卡片、输入框脚注与状态栏
  显示 "v<版本> <列>x<行>"。Windows 与 Linux 产物统一按版本命名
  (Ember-<版本>-win64.zip / ember-<版本>-linux-x86_64)。
- 新增:MCP 客户端。settings.json 的 mcp_servers 接入 stdio MCP 服务器,
  工具以 "<服务器名>_" 前缀注册;/mcp 查看连接状态与工具数。
- 新增:多服务商与模型分组。providers 定义端点,groups 把模型绑定到
  服务商;/model 分组列出,/model <名称|编号> 会连同 base_url/key 一起切换。
- 新增:思考强度。/think auto|none|minimal|low|medium|high|max;
  各端点的线上写法差异由 provider 的 thinking_style 适配(effort / thinking /
  enable_thinking / chat_template_kwargs)。
- 新增:工作区。/ws list|use N|add PATH|rm N|on|off;开启 workspace_guard
  后,工具越界访问文件会先请求批准。
- 新增:运行模式。/mode standard|minimal|ptc|creator;ptc 模式可用 run_code
  一次提交多步工具调用,creator 模式做运行时自检。
- 新增工具:rag_index / rag_search 本地检索、todowrite 计划清单、
  remember / recall 跨会话记忆、verify 构建+测试闭环、
  git_status / git_diff / git_log / git_commit、
  subagent / subagent_parallel、file_list、ask_user。
- 新增:--serve 无界面 NDJSON 协议(桌面端与脚本用)。stdin/stdout 各一行一个
  JSON。命令:prompt / answer / command / set / state / models / sessions /
  session / history / new_session / workspaces / commands / usage / compact /
  clear / shutdown。事件:ready / state / sessions / model_list / workspaces /
  commands / history / notice / delta / reasoning / tool_call / tool_result /
  ask / models / background / error / turn_end / usage / clear。斜杠命令与终端
  共用同一套实现(不复制第二份逻辑),回执统一走 notice。
- 新增:桌面端 desktop/(Tauri 2 + React 19 + TypeScript)。以 sidecar 方式拉起
  agent --serve,模型 / 思考档 / 会话 / 工作区 / 命令面板一律读后端真实上报;
  几何设计系统见 FRONTEND_DESIGN.md。CLI 与桌面端是两条独立产物线。

0.9.1 更新(相对 0.9.0)
------------------------------------------------------------
- 新增:/model 或 /models 自动从 API 拉取模型列表(/models 端点),
  显示编号列表后用 /model <名称|编号> 直接切换;失败会提示原因。
- 修复:忙碌指示器(状态栏 /-\ )等动画在无输入时不再静止——
  帧循环改为无条件 60fps 渲染(无变化时输出为空,开销可忽略)。
- 修复:滚动(PgUp/PgDn/End/滚轮)时整窗全量重绘,彻底消除
  陈旧行残留 / 字符重叠问题;流式输出收缩也有兜底清行。

0.9.0 更新(相对 0.8.0)
------------------------------------------------------------
- 新增:主题系统。/theme NAME 切换,设置页也可选:
    dark(默认)/ light / terminal / nord / gruvbox / dracula / solarized
- 新增:settings.json 的 "theme_colors" 字段可自定义配色(仅对
  nord/gruvbox/dracula/solarized 生效),改文件保存后重启即生效:
      "theme": "nord",
      "theme_colors": {
        "green": [163,190,140], "gold": [235,203,139],
        "blue":  [129,161,193], "purple": [180,142,173],
        "red":   [191,97,106],  "fg": [216,222,233],
        "fg_dim": [140,160,184], "bg": [46,52,64],
        "head_bg": [59,66,82],  "rule": [67,76,94]
      }
- 修复:滚动回看或生成内容收缩时,上一帧字符残留导致重叠重影。

一、启动
------------------------------------------------------------
建议在 Windows Terminal / ConEmu 等现代终端中运行(需要真实终端,
不支持在资源管理器中双击直接交互):

    agent.exe --trace traj.jsonl

参数说明(均为可选,环境变量同样有效,CLI 优先于环境变量):
    --base-url URL     API 地址,如 https://api.deepseek.com/v1
    --model NAME       模型名,如 deepseek-v4-flash
    --api-key KEY      API 密钥
    --system TEXT      系统提示词
    --session PATH     单一会话文件(JSON,重启同文件可续聊;界面内 /save)
    --sessions DIR     多会话目录(每会话一个文件;/sessions 列表切换)
    --trace PATH       事件轨迹(traj.jsonl,界面内 /trajectory 查看)
    --plugins DIR      插件目录(可执行文件,JSON-RPC over stdio)
    --config PATH      settings 文件(默认 settings.json;/settings 写回)
    --budget N         上下文预算 token 数(0 关闭,默认 6000)
    --tool-cap N       工具结果字节上限(0 = 不限,默认 4096)
    --no-prune         关闭发送前冗余工具输出裁剪
    --rules PATH       AGENTS.md / SKILL.md 规则文件(默认读工作目录)
    --mode NAME        运行模式 standard|minimal|ptc|creator
    --thinking NAME    思考强度 auto|none|minimal|low|medium|high|max
    --json             结构化 JSON 输出模式
    --no-shell         禁用 shell_exec 工具
    --token-summary    退出时打印 token 台账一行(基准用)
    -v, --version      打印版本号并退出

环境变量:AGENT_BASE_URL / AGENT_MODEL / AGENT_API_KEY /
OPENAI_API_KEY / AGENT_SYSTEM_PROMPT / AGENT_SESSION / AGENT_SESSIONS /
AGENT_TRACE / AGENT_PLUGINS / AGENT_MODE / AGENT_THINKING

命令 <-> CLI 参数 对照(界面内 /help 亦有):
    /sessions   <-> --sessions DIR / AGENT_SESSIONS
    /save       <-> --session FILE / AGENT_SESSION
    /model NAME <-> --model          /json <-> --json
    /mode NAME  <-> --mode NAME      /think NAME <-> --thinking NAME
    /trajectory <-> --trace(别名 /trace 即 CLI 名)
    /settings   <-> --config PATH

二、首次启动(重要)
------------------------------------------------------------
第一次运行会自动弹出"first-time setup"配置向导,请填写三项并保存:

    base url   API 端点,形如 https://xxx/v1
    api key    密钥(明文显示为 * )
    model      模型名,如 deepseek-v4-flash

按键:Enter 编辑选中项,Tab 移到下一项,Ctrl+S 保存并继续,Esc 跳过。
保存后写入同目录下的 settings.json,下次启动自动读取;跳过后可随时
用 /settings 补齐。

三、基本用法
------------------------------------------------------------
直接输入文字并按 Enter 即可对话。模型可以使用内置工具,无需手动
调用,交给你想让它完成的任务即可。

常用命令(输入 / 后不带空格):
    /help        完整指引            /clear 清空对话
    /new         新会话              /compact 压缩上下文
    /json        切换 JSON 模式
    /model NAME  切换模型;/model(或 /models)拉取可用模型列表,
                 /model add|rm NAME 维护列表(服务商随分组自动切换)
    /theme NAME  切换主题 dark|light|terminal|nord|gruvbox|dracula|solarized
    /mode NAME   运行模式 standard|minimal|ptc|creator
    /think NAME  思考强度 auto|none|minimal|low|medium|high|max
    /mcp         MCP 服务器连接状态
    /ws          工作区:/ws list|use N|add PATH|rm N|on|off
    /provider    弹窗填写服务商(base_url / key / 模型)
    /settings    设置(价格 / 预算 / 工具开关,写入 settings.json)
    /usage       用量与成本环(快捷键 Ctrl+U)    /stats 一行统计
    /trajectory  事件轨迹(快捷键 Ctrl+T)
    /sessions    会话列表(快捷键 Ctrl+O,--sessions 目录模式)
    /save        立即保存会话到文件
    /copy        复制最近回答   /copy all 复制整场对话

快捷键:
    Ctrl+P  命令面板(输入过滤、↑↓/Tab 选择、Enter 执行)
    / + 前缀自动联想命令,↑↓ 选择,Tab / Enter 补全或执行
    Ctrl+T  轨迹       Ctrl+U  用量      Ctrl+O  会话
    Ctrl+G  下一个会话                  Ctrl+S  设置页保存
    Esc     关闭覆盖页 / 清空输入
    PgUp/PgDn 或滚轮   滚动
    Ctrl+C  退出(请求进行中再按一次强制)

四、模型可用工具
------------------------------------------------------------
shell_exec、verify(构建+测试闭环)、file_read / file_write / edit /
patch / file_list / grep / glob、web_fetch / web_search、
rag_index / rag_search、git_status / git_diff / git_log / git_commit、
background_start / background_status / background_kill / background_list、
subagent / subagent_parallel、ask_user、todowrite、remember / recall、
run_code(一次提交多步工具调用)、browser_*(headless Chrome/Edge over CDP)、
desktop_*(Windows UI Automation)。

五、后台任务
------------------------------------------------------------
耗时命令交给后台执行,不阻塞对话,完成后会收到模型通知:
    "用 background_start 跑 <命令>,完成后在后台任务通知里报结果"
也可手动轮询:background_status <id> / background_kill <id> / background_list。

六、MCP 与本地插件
------------------------------------------------------------
MCP 服务器在 settings.json 里配置(stdio JSON-RPC 2.0,一行一帧):

    "mcp_servers": [
      { "name": "filesystem", "command": "npx",
        "args": ["-y", "@modelcontextprotocol/server-filesystem", "C:\\src"],
        "enabled": true }
    ]

它们的工具以 "<服务器名>_" 前缀注册,用 /mcp 查看连接状态与工具数。
本地插件(可执行文件,同样是 stdio JSON-RPC)用 --plugins DIR 加载,
插件通过 tools/list 自报工具。

七、用量与成本
------------------------------------------------------------
/usage 页面有成本 / 预算圆环和缓存命中率圆环,底部状态栏显示迷你
统计。成本按 tokens x /settings 中配置的 $/M 单价估算,预算封顶值
也在 /settings 中设置。

八、常见问题
------------------------------------------------------------
- 想确认手上是哪个版本:agent.exe --version,或看欢迎卡片 / 输入框
  脚注右下角的 "v<版本> <列>x<行>"。
- 提示 no credentials / client invalid:检查 base url 与 api key,
  在 /settings 里修改后 Ctrl+S 保存(会被写入 settings.json)。
- 中文显示为乱码:结果字符串为 UTF-8 而 Windows 控制台为 GBK 时,
  仅显示受影响,不影响功能(Windows 11 新版终端一般自动处理)。
- 工具超时:默认 120 秒强制终止并向上报错,可让模型重试或拆分。
- 程序闪退或异常退出时,请把同目录的 agent_crash.log 一并反馈。
- 想重置"首次向导":删除运行目录下 .agent_first_run 后重启。

版本历史交由内测反馈集中收集。反馈请附 traj.jsonl 与复现步骤。

许可:AGPL-3.0-only(全文见同目录 LICENSE)。你可以自由运行、研究、修改和
分发本程序;但只要你分发改动后的版本,或者让他人通过网络使用它(包括
--serve 协议和桌面端),就必须按同一许可公开对应源码。