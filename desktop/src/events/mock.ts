/**
 * 浏览器预览用的假传输层。
 *
 * 只服务于 ?mock=1 与直接在 vite 里打开的场景：事件与命令面
 * 和 `agent.exe --serve` 完全同 schema（events/schema.ts），所以界面在
 * 预览和真机之间不需要任何分支。这里的所有读数都是演示值，
 * 状态条会显式标出 MOCK，别把它当真实用量看。
 */
import type {
  AgentCommand,
  AgentEvent,
  AgentTransport,
  CommandInfo,
  ModelInfo,
  SessionInfo,
  WorkspaceInfo,
} from './schema';

const READY = {
  type: 'ready',
  version: '0.10.0',
  model: 'agnes-3.0-flash',
  mode: 'standard',
  cwd: '~/work/ember',
  session: '~/.ember/sessions',
  ctxBudget: 6000,
  costBudgetMicroUsd: 10_000_000,
} as const satisfies AgentEvent;

const DAY = 86_400;
const now = Math.floor(Date.now() / 1000);

/** 会话列表：savedAt 铺在近 7 天里，用量页的柱图才有真实形状。 */
const SESSIONS: SessionInfo[] = [
  { id: 'sess-mock-1', title: 'web_fetch 超时可配置', model: 'agnes-3.0-flash', msgCount: 24, savedAt: now - 2 * 60 * 60, current: true },
  { id: 'sess-mock-2', title: '修 test_search 的路径大小写', model: 'agnes-3.0-flash', msgCount: 12, savedAt: now - DAY, current: false },
  { id: 'sess-mock-3', title: '分析 traj.jsonl 里的失败工具', model: 'gx-glm-5.3-flash', msgCount: 41, savedAt: now - 2 * DAY, current: false },
  { id: 'sess-mock-4', title: 'Linux 交叉编译 libcurl 链接失败', model: 'ofree-z-ai/glm-5.3-flash-free', msgCount: 33, savedAt: now - 4 * DAY, current: false },
  { id: 'sess-mock-5', title: '给 RAG 索引加中文分词', model: 'agnes-3.0-flash', msgCount: 8, savedAt: now - 6 * DAY, current: false },
];

const MODELS: ModelInfo[] = [
  { name: 'agnes-3.0-flash', group: 'free', provider: 'agnes', current: true },
  { name: 'agnes-2.5-flash', group: 'free', provider: 'agnes', current: false },
  { name: 'gx-glm-5.3-flash', group: 'zhipu', provider: 'zhipu', current: false },
  { name: 'ofree-z-ai/glm-5.3-flash-free', group: 'free', provider: 'zhipu', current: false },
  { name: 'ofree-deepseek/deepseek-v4-flash-free', group: 'free', provider: 'zhipu', current: false },
  { name: 'deepseek-chat', group: 'ds', provider: 'deepseek', current: false },
];

const WORKSPACES: WorkspaceInfo[] = [
  { index: 0, path: '~/work/ember', current: true },
  { index: 1, path: '~/work/demo-app', current: false },
];

/** 预览用副本：真机的命令面板读后端 commands 事件（C++ 的 kCommands 表）。 */
const COMMANDS: CommandInfo[] = (
  [
    ['help', '本指引(命令与快捷键一览)', '', false],
    ['clear', '清空当前对话', '', false],
    ['new', '开始新会话', '', false],
    ['compact', '压缩上下文(减少 token 用量)', '', false],
    ['model', '切换模型(服务商随分组自动切换) NAME|编号|add|rm|fetch', '', true],
    ['theme', '切换主题 dark|light|terminal|nord|gruvbox|dracula|solarized', '', true],
    ['mode', '切换运行模式 standard|minimal|ptc|creator', '', true],
    ['think', '模型思考强度 auto|none|minimal|low|medium|high|max', '', true],
    ['mcp', 'MCP 服务器连接状态', '', false],
    ['ws', '工作区: /ws list|use N|add PATH|rm N|on|off', '', true],
    ['json', '切换 JSON 输出模式', '', false],
    ['provider', '弹窗配置服务商 base_url / key / 模型', '', false],
    ['settings', '设置(价格/预算/工具开关)', 'Ctrl+S', false],
    ['usage', '用量与成本环', 'Ctrl+U', false],
    ['stats', '一行用量统计(写入对话)', '', false],
    ['trajectory', '事件轨迹查看', 'Ctrl+T', false],
    ['sessions', '会话列表', 'Ctrl+O', false],
    ['save', '立即保存会话到文件', '', false],
    ['copy', '复制最近回答到剪贴板(/copy all 复制全场)', '', false],
  ] as const
).map(([name, desc, key, needsArg]) => ({ name: `/${name}`, desc, key, needsArg }));

const THINK = '超时值可能在 http.cpp 的请求层与 tools.cpp 的工具层各有一份；先定位再统一到配置项，避免只改一处。';

const PROMPT = '把 web_fetch 的超时改成可配置的，默认别变；顺便看看还有哪些地方写死了超时值。';

const ANS1 = '先定位写死的超时，再决定改哪一层。';

const GREP_HITS = `http.cpp:88   // hardcoded request timeout
tools.cpp:2803 int timeoutMs = 30000;
browser.cpp:141 int timeoutMs = 15000;`;

const ANS2 = `超时确实分散在三处。我把工具层与请求层统一读同一个配置项，浏览器层单独保留（CDP 语义不同）。

\`\`\`c++
// 工具层与请求层共用同一超时；0 = 不限
int toolTimeoutMs(Settings const& s) {
  return s.timeoutMs > 0 ? s.timeoutMs : 30000;
}
\`\`\``;

const ANS3 = '改动已落在两处：配置项在 settings.json 的 `tool_timeout_ms`，默认 30000，保持原有行为不变；浏览器层维持 15s。';

const MOCK_USAGE: AgentEvent = {
  type: 'usage',
  turns: 1,
  steps: 2,
  llmMs: 8400,
  toolMs: 320,
  promptTokens: 18400,
  completionTokens: 2100,
  cachedTokens: 11400,
  ctxTokens: 3600,
  ctxBudget: 6000,
  costMicroUsd: 86_700,
  costBudgetMicroUsd: 10_000_000,
};

/** 与真机同形状的历史回放：工具卡与结果也在里面，代码围栏保留换行。 */
const HISTORY: AgentEvent = {
  type: 'history',
  list: [
    { kind: 'user', text: '把 web_fetch 的超时改成可配置的，默认别变' },
    { kind: 'assistant', text: '好，先看现在写死在哪几处。' },
    { kind: 'tool', id: 'call-h1', name: 'grep', args: '{"pattern":"timeoutMs","path":"agent/src"}' },
    { kind: 'tool_result', id: 'call-h1', result: GREP_HITS },
    { kind: 'tool', id: 'call-h2', name: 'file_read', args: '{"path":"agent/src/browser.cpp","from":135,"to":150}' },
    { kind: 'tool_result', id: 'call-h2', result: 'error: 行范围超出文件长度' },
    { kind: 'user', text: '顺便看看还有哪些地方写死了超时值' },
    {
      kind: 'assistant',
      text:
        '三处：请求层、工具层、浏览器层。浏览器层语义不同，单独留。\n\n```c++\nint toolTimeoutMs(Settings const& s) {\n  return s.timeoutMs > 0 ? s.timeoutMs : 30000;\n}\n```',
    },
  ],
};

/** 演示态的可变量：set 命令改它们，state 事件回读，界面立刻跟着变。 */
interface MockState {
  model: string;
  thinking: string;
  mode: string;
  jsonMode: boolean;
  sessionId: string;
  sessionTitle: string;
  workspace: string;
  workspaceGuard: boolean;
  busy: boolean;
}

const STATE: MockState = {
  model: 'agnes-3.0-flash',
  thinking: 'medium',
  mode: 'standard',
  jsonMode: false,
  sessionId: 'sess-mock-1',
  sessionTitle: 'web_fetch 超时可配置',
  workspace: '~/work/ember',
  workspaceGuard: true,
  busy: false,
};

function stateEvent(): AgentEvent {
  return {
    type: 'state',
    version: READY.version,
    model: STATE.model,
    thinking: STATE.thinking,
    thinkingStyle: 'chat_template_kwargs',
    thinkingBinary: true,
    mode: STATE.mode,
    jsonMode: STATE.jsonMode,
    cwd: READY.cwd,
    sessionDir: READY.session,
    sessionId: STATE.sessionId,
    sessionTitle: STATE.sessionTitle,
    ctxBudget: READY.ctxBudget,
    costBudgetMicroUsd: READY.costBudgetMicroUsd,
    workspace: STATE.workspace,
    workspaceGuard: STATE.workspaceGuard,
    busy: STATE.busy,
  };
}

function notice(text: string): AgentEvent {
  return { type: 'notice', text };
}

/** 一个可订阅的假后端：先播历史，再逐字流式最后一句。 */
export function createMockTransport(): AgentTransport {
  const handlers = new Set<(ev: AgentEvent) => void>();
  const timers: number[] = [];
  let streamer = 0;

  const emit = (ev: AgentEvent): void => {
    handlers.forEach((h) => h(ev));
  };
  const later = (ms: number, fn: () => void): void => {
    timers.push(window.setTimeout(fn, ms));
  };

  /** 工作区列表：哪一行是 active 由这里现算，切换后必须重发（与 C++ 同） */
  const emitWorkspaces = (): void => {
    const active = Math.max(0, WORKSPACES.findIndex((w) => w.path === STATE.workspace));
    emit({
      type: 'workspaces',
      list: WORKSPACES.map((w) => ({ ...w, current: w.index === active })),
      active,
      guard: STATE.workspaceGuard,
    });
  };

  /** 与 C++ App::serveSet 同语义：改哪一项、回执什么、要不要重发列表。 */
  const applySet = (key: string, value: string): void => {
    if (key === 'thinking') {
      if (!['auto', 'none', 'minimal', 'low', 'medium', 'high', 'max'].includes(value)) {
        emit({ type: 'error', message: `thinking: auto | none | minimal | low | medium | high | max（收到 ${value}）` });
        return;
      }
      STATE.thinking = value;
      emit(notice(`thinking: ${value}  (mock，不落盘)  注意: chat_template_kwargs 只分开关，minimal/low/high 等效`));
    } else if (key === 'model') {
      const m = MODELS.find((x) => x.name === value);
      if (!m) {
        emit({ type: 'error', message: `未配置的模型: ${value}` });
        return;
      }
      STATE.model = value;
      emit(notice(`model: ${value}  · 服务商: ${m.provider}  (mock)`));
    } else if (key === 'mode') {
      if (!['standard', 'minimal', 'ptc', 'creator'].includes(value)) {
        emit({ type: 'error', message: `mode: standard | minimal | ptc | creator（收到 ${value}）` });
        return;
      }
      STATE.mode = value;
      emit(notice(`mode: ${value}  (mock)`));
    } else if (key === 'workspace') {
      const w = WORKSPACES[Number(value)];
      if (!w) {
        emit({ type: 'error', message: `无工作区 #${value}` });
        return;
      }
      STATE.workspace = w.path;
      emit(notice(`workspace → ${w.path}  (mock)`));
      emitWorkspaces();
    } else if (key === 'json') {
      STATE.jsonMode = value === 'on' || value === 'true';
      emit(notice(`JSON mode ${STATE.jsonMode ? 'ON' : 'OFF'}  (mock)`));
    } else {
      emit({ type: 'error', message: `unknown set key: ${key}` });
      return;
    }
    emit(stateEvent());
  };

  /** 与 C++ runServe 相同的应答面：每种命令回什么，这里对齐什么。 */
  const answer = (cmd: AgentCommand): boolean => {
    switch (cmd.type) {
      case 'state':
        emit(stateEvent());
        return true;
      case 'sessions':
        emit({ type: 'sessions', dir: READY.session, list: SESSIONS });
        return true;
      case 'models':
        emit({ type: 'model_list', list: MODELS.map((m) => ({ ...m, current: m.name === STATE.model })), source: 'settings' });
        return true;
      case 'workspaces':
        emitWorkspaces();
        return true;
      case 'commands':
        emit({ type: 'commands', list: COMMANDS });
        return true;
      case 'history':
        emit(HISTORY);
        return true;
      case 'usage':
        emit(MOCK_USAGE);
        return true;
      case 'set':
        applySet(cmd.key, cmd.value);
        return true;
      case 'command': {
        const [word, ...rest] = cmd.text.slice(1).split(' ');
        const arg = rest.join(' ').trim();
        if (word === 'model' || word === 'models') {
          if (arg) applySet('model', arg);
          else emit({ type: 'model_list', list: MODELS.map((m) => ({ ...m, current: m.name === STATE.model })), source: 'settings' });
        } else if (word === 'sessions' || word === 'ls') {
          emit({ type: 'sessions', dir: READY.session, list: SESSIONS });
        } else if (word === 'usage' || word === 'cost') {
          emit(MOCK_USAGE);
        } else if (word === 'think') {
          applySet('thinking', arg);
        } else if (word === 'mode') {
          applySet('mode', arg);
        } else if (word === 'json') {
          STATE.jsonMode = !STATE.jsonMode;  // /json 是切换，与 C++ 同
          emit(notice(`JSON mode ${STATE.jsonMode ? 'ON' : 'OFF'}  (mock)`));
          emit(stateEvent());
        } else if (word === 'ws' || word === 'workspace') {
          if (arg === 'on' || arg === 'off') {
            STATE.workspaceGuard = arg === 'on';
            emit(notice(`工作区审批: ${STATE.workspaceGuard ? 'ON' : 'OFF'}  (mock)`));
            emitWorkspaces();
          } else if (arg.startsWith('use ')) {
            applySet('workspace', arg.slice(4).trim());
          } else {
            emitWorkspaces();
          }
        } else if (word === 'clear') {
          emit({ type: 'clear' });
        } else if (word === 'new') {
          STATE.sessionId = 'sess-mock-new';
          STATE.sessionTitle = '';
          emit({ type: 'sessions', dir: READY.session, list: SESSIONS.map((s) => ({ ...s, current: false })) });
          emit(stateEvent());
          emit({ type: 'clear' });
        } else {
          emit(notice(`（mock）已解析命令 ${cmd.text}`));
        }
        // C++ 在任何斜杠命令后都重发一次 state，这里对齐，预览里也不会出现"改了但界面不动"
        emit(stateEvent());
        return true;
      }
      case 'new_session': {
        const fresh: SessionInfo = {
          id: `sess-mock-${Date.now() % 10000}`,
          title: '',
          model: STATE.model,
          msgCount: 0,
          savedAt: Math.floor(Date.now() / 1000),
          current: true,
        };
        STATE.sessionId = fresh.id;
        STATE.sessionTitle = '';
        emit(notice('new session started  (mock)'));
        SESSIONS.unshift(fresh);
        emit({ type: 'sessions', dir: READY.session, list: SESSIONS.map((s) => ({ ...s, current: s.id === fresh.id })) });
        emit(stateEvent());
        emit({ type: 'clear' });
        return true;
      }
      case 'session': {
        const s = SESSIONS.find((x) => x.id === cmd.id);
        if (!s) {
          emit({ type: 'error', message: `no such session: ${cmd.id}` });
          break;
        }
        STATE.sessionId = s.id;
        STATE.sessionTitle = s.title;
        emit({ type: 'sessions', dir: READY.session, list: SESSIONS.map((x) => ({ ...x, current: x.id === s.id })) });
        emit(stateEvent());
        emit(HISTORY);
        return true;
      }
      case 'clear':
        emit({ type: 'clear' });
        return true;
      case 'compact':
        emit({ type: 'tool_call', name: 'context', args: '压缩 12 条较早消息' });
        emit({ type: 'tool_result', name: 'context', result: '已压缩为 1 条摘要（mock）' });
        return true;
      default:
        return false;
    }
    return false;
  };

  return {
    subscribe(handler: (ev: AgentEvent) => void): () => void {
      handlers.add(handler);
      later(0, () => emit(READY));
      later(10, () => emit(stateEvent()));
      later(20, () => emit(HISTORY));
      later(30, () => emit({ type: 'user', text: PROMPT }));
      later(160, () => emit({ type: 'reasoning', text: THINK }));
      later(300, () => emit({ type: 'delta', text: ANS1 }));
      later(620, () => emit({ type: 'tool_call', name: 'file_read', args: 'agent/src/tools.cpp · 检索 timeout' }));
      later(980, () => emit({ type: 'tool_result', name: 'file_read', result: GREP_HITS }));
      later(1180, () => emit({ type: 'delta', text: ANS2 }));
      later(1520, () => emit({ type: 'tool_call', name: 'shell_exec', args: 'g++ -std=c++20 -O2 构建 + 跑 test_tools …' }));
      // 越界写入等确认：检查器「待批准」面板会亮出允许/拒绝
      later(1560, () => emit({ type: 'ask', question: '越界写入 ../shared/ 需要你确认' }));

      // 16ms/字符：与真实流式同节奏（设计系统 §7「流式文本」）
      later(1800, () => {
        let i = 0;
        STATE.busy = true;
        streamer = window.setInterval(() => {
          i += 1;
          emit({ type: 'delta', text: ANS3.slice(i - 1, i) });
          if (i >= ANS3.length) {
            window.clearInterval(streamer);
            STATE.busy = false;
            emit({ type: 'turn_end', rc: 0 });
            emit(MOCK_USAGE);
          }
        }, 16);
      });

      return () => {
        handlers.delete(handler);
        timers.forEach((t) => window.clearTimeout(t));
        if (streamer) window.clearInterval(streamer);
      };
    },

    send(cmd: AgentCommand): void {
      if (cmd.type === 'prompt') {
        later(120, () => emit({ type: 'delta', text: '（mock 传输层）' }));
        later(360, () =>
          emit({ type: 'delta', text: `收到「${cmd.text}」。接上 agent.exe --serve 后，这里会是真实回复。` }),
        );
        later(700, () => emit({ type: 'tool_call', name: 'file_read', args: 'agent/src/app.cpp' }));
        later(1100, () => emit({ type: 'tool_result', name: 'file_read', result: '4231 行 · 读取完成' }));
        later(1400, () => {
          emit({ type: 'turn_end', rc: 0 });
          emit(MOCK_USAGE);
        });
        return;
      }
      later(60, () => {
        if (!answer(cmd)) emit({ type: 'error', message: `mock 未实现命令: ${cmd.type}` });
      });
    },
  };
}
