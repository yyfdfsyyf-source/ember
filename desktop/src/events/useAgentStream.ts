/**
 * UI 状态机：把 AgentEvent 流折叠成可渲染的会话状态。
 *
 * 这里是与后端唯一的耦合点；P2 换成 Tauri sidecar 时只替换 transport，
 * reducer 与组件一行不动。
 */
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type {
  AgentCommand,
  AgentEvent,
  AgentTransport,
  CommandInfo,
  ModelInfo,
  RefreshKind,
  SessionInfo,
  SetKey,
  WorkspaceInfo,
} from './schema';

export interface ToolItem {
  kind: 'tool';
  id: string;
  name: string;
  args: string;
  result: string;
  state: 'run' | 'ok' | 'bad';
  /** 卡片展开态由用户点击决定，流式期间不自动抢焦点 */
  open: boolean;
  at: string;
}

export interface ChatItem {
  kind: 'user' | 'assistant' | 'reasoning' | 'error' | 'notice';
  id: string;
  text: string;
  streaming: boolean;
  at: string;
}

export interface UsageState {
  turns: number;
  steps: number;
  llmMs: number;
  toolMs: number;
  promptTokens: number;
  completionTokens: number;
  cachedTokens: number;
  /** 当前上下文占用 token（喂给模型的估算值） */
  ctxTokens: number;
  /** 上下文压缩阈值（ready 事件给的，0 = 后端未配置/不限） */
  ctxBudget: number;
  /** 本会话累计成本，micro-USD（1e-6 美元；整数过线，前端只做展示换算） */
  costMicroUsd: number;
  /** 成本预算，micro-USD（0 = 没设预算） */
  costBudgetMicroUsd: number;
}

export interface SessionState {
  connected: boolean;
  version: string;
  model: string;
  cwd: string;
  items: Array<ChatItem | ToolItem>;
  streaming: boolean;
  pendingAsk: string | null;
  error: string | null;
  usage: UsageState;
  /** 外壳读数：全部来自后端的 state 事件，前端不自造默认值 */
  thinking: string;
  thinkingStyle: string;
  thinkingBinary: boolean;
  mode: string;
  jsonMode: boolean;
  sessionId: string;
  sessionTitle: string;
  sessionDir: string;
  workspace: string;
  workspaceGuard: boolean;
  busy: boolean;
  sessions: SessionInfo[];
  models: ModelInfo[];
  workspaces: WorkspaceInfo[];
  commands: CommandInfo[];
}

const EMPTY_USAGE: UsageState = {
  turns: 0,
  steps: 0,
  llmMs: 0,
  toolMs: 0,
  promptTokens: 0,
  completionTokens: 0,
  cachedTokens: 0,
  ctxTokens: 0,
  ctxBudget: 0,
  costMicroUsd: 0,
  costBudgetMicroUsd: 0,
};

const INITIAL: SessionState = {
  connected: false,
  version: '',
  model: '',
  cwd: '',
  items: [],
  streaming: false,
  pendingAsk: null,
  error: null,
  usage: EMPTY_USAGE,
  thinking: '',
  thinkingStyle: '',
  thinkingBinary: false,
  mode: '',
  jsonMode: false,
  sessionId: '',
  sessionTitle: '',
  sessionDir: '',
  workspace: '',
  workspaceGuard: false,
  busy: false,
  sessions: [],
  models: [],
  workspaces: [],
  commands: [],
};

/** HH:MM，事件到达时刻（真实数据下就是墙钟时间）。 */
function clock(at: Date): string {
  const hh = String(at.getHours()).padStart(2, '0');
  const mm = String(at.getMinutes()).padStart(2, '0');
  return `${hh}:${mm}`;
}

/** 工具失败判据：结果开头像错误就当失败（实时与历史回放共用一条规则）。 */
function toolFailed(result: string): boolean {
  return /^\s*(error|failed|Traceback|超时)/i.test(result);
}

/** 纯函数 reducer：便于单测，也让 P2 的传输层替换不需要动视图。 */
export function reduce(state: SessionState, ev: AgentEvent): SessionState {
  const at = clock(new Date());
  const last = state.items[state.items.length - 1];

  switch (ev.type) {
    case 'ready':
      return {
        ...state,
        connected: true,
        version: ev.version,
        model: ev.model,
        cwd: ev.cwd,
        // 预算随握手一起到，圆环不用等第一轮跑完
        usage: {
          ...state.usage,
          ctxBudget: ev.ctxBudget ?? state.usage.ctxBudget,
          costBudgetMicroUsd: ev.costBudgetMicroUsd ?? state.usage.costBudgetMicroUsd,
        },
      };

    case 'state':
      return {
        ...state,
        connected: true,
        version: ev.version,
        model: ev.model,
        cwd: ev.cwd,
        thinking: ev.thinking,
        thinkingStyle: ev.thinkingStyle,
        thinkingBinary: ev.thinkingBinary,
        mode: ev.mode,
        jsonMode: ev.jsonMode,
        sessionId: ev.sessionId,
        sessionTitle: ev.sessionTitle,
        sessionDir: ev.sessionDir,
        workspace: ev.workspace,
        workspaceGuard: ev.workspaceGuard,
        busy: ev.busy,
        usage: {
          ...state.usage,
          ctxBudget: ev.ctxBudget ?? state.usage.ctxBudget,
          costBudgetMicroUsd: ev.costBudgetMicroUsd ?? state.usage.costBudgetMicroUsd,
        },
      };

    case 'sessions':
      return { ...state, sessions: ev.list, sessionDir: ev.dir };

    case 'model_list':
      return { ...state, models: ev.list };

    case 'workspaces':
      return { ...state, workspaces: ev.list, workspaceGuard: ev.guard };

    case 'commands':
      return { ...state, commands: ev.list };

    case 'history': {
      // 整段替换，不是追加：这就是后端 messages_ 的真实内容。
      // 历史里没有逐条时间戳，at 留空让界面省略，不拿"现在"冒充"当时"。
      const items: Array<ChatItem | ToolItem> = [];
      const toolIndex = new Map<string, number>();
      for (const row of ev.list) {
        if (row.kind === 'user' || row.kind === 'assistant') {
          items.push({ kind: row.kind, id: `h${items.length}`, text: row.text, streaming: false, at: '' });
        } else if (row.kind === 'tool') {
          toolIndex.set(row.id, items.length);
          items.push({
            kind: 'tool',
            id: row.id || `h${items.length}`,
            name: row.name,
            args: row.args,
            result: '',
            state: 'ok',
            open: false,
            at: '',
          });
        } else {
          const i = toolIndex.get(row.id);
          if (i === undefined) continue;
          const card = items[i] as ToolItem;
          items[i] = { ...card, result: row.result, state: toolFailed(row.result) ? 'bad' : 'ok' };
        }
      }
      return { ...state, items };
    }

    case 'notice':
      return {
        ...state,
        items: [...state.items, { kind: 'notice', id: `n${state.items.length}`, text: ev.text, streaming: false, at }],
      };

    case 'clear':
      return { ...state, items: [], error: null };

    case 'user':
      return {
        ...state,
        items: [...state.items, { kind: 'user', id: `u${state.items.length}`, text: ev.text, streaming: false, at }],
      };

    case 'reasoning': {
      if (last && last.kind === 'reasoning' && last.streaming) {
        const items = state.items.slice(0, -1);
        items.push({ ...last, text: last.text + ev.text });
        return { ...state, items, streaming: true };
      }
      return {
        ...state,
        streaming: true,
        items: [...state.items, { kind: 'reasoning', id: `r${state.items.length}`, text: ev.text, streaming: true, at }],
      };
    }

    case 'delta': {
      if (last && last.kind === 'assistant' && last.streaming) {
        const items = state.items.slice(0, -1);
        items.push({ ...last, text: last.text + ev.text });
        return { ...state, items, streaming: true };
      }
      return {
        ...state,
        streaming: true,
        items: [...state.items, { kind: 'assistant', id: `a${state.items.length}`, text: ev.text, streaming: true, at }],
      };
    }

    case 'tool_call': {
      // 会话里第一张工具卡默认展开（与原型一致：一眼看到工具产出），其余保持收起
      const first = !state.items.some((it) => it.kind === 'tool');
      return {
        ...state,
        items: [
          ...state.items,
          {
            kind: 'tool',
            id: `t${state.items.length}`,
            name: ev.name,
            args: ev.args,
            result: '',
            state: 'run',
            open: first,
            at,
          },
        ],
      };
    }

    case 'tool_result': {
      // 结果按最近的同名运行中卡片回填
      for (let i = state.items.length - 1; i >= 0; i -= 1) {
        const it = state.items[i];
        if (it.kind === 'tool' && it.name === ev.name && it.state === 'run') {
          const items = state.items.slice();
          items[i] = { ...it, result: ev.result, state: toolFailed(ev.result) ? 'bad' : 'ok' };
          return { ...state, items };
        }
      }
      return state;
    }

    case 'ask':
      return { ...state, pendingAsk: ev.question };

    case 'error':
      return {
        ...state,
        error: ev.message,
        items: [...state.items, { kind: 'error', id: `e${state.items.length}`, text: ev.message, streaming: false, at }],
      };

    case 'background':
      return {
        ...state,
        items: [...state.items, { kind: 'assistant', id: `b${state.items.length}`, text: ev.text, streaming: false, at }],
      };

    case 'models':
    case 'turn_end': {
      // 收尾：所有流式标记归零，仍在跑的工具卡落成完成态
      const items = state.items.map((it) => {
        if (it.kind === 'tool') return it.state === 'run' ? { ...it, state: 'ok' as const } : it;
        return it.streaming ? { ...it, streaming: false } : it;
      });
      return { ...state, items, streaming: false };
    }

    case 'usage':
      return {
        ...state,
        usage: {
          turns: ev.turns,
          steps: ev.steps,
          llmMs: ev.llmMs,
          toolMs: ev.toolMs,
          promptTokens: ev.promptTokens,
          completionTokens: ev.completionTokens,
          cachedTokens: ev.cachedTokens,
          ctxTokens: ev.ctxTokens ?? state.usage.ctxTokens,
          ctxBudget: ev.ctxBudget ?? state.usage.ctxBudget,
          costMicroUsd: ev.costMicroUsd ?? state.usage.costMicroUsd,
          costBudgetMicroUsd: ev.costBudgetMicroUsd ?? state.usage.costBudgetMicroUsd,
        },
      };

    default:
      return state;
  }
}

export interface AgentStream {
  state: SessionState;
  send: (cmd: AgentCommand) => void;
  toggleTool: (id: string) => void;
  answer: (text: string) => void;
  /** 向后端要一次读数：state / sessions / models / workspaces / commands / usage */
  refresh: (what: RefreshKind) => void;
  /** 改后端设置（思考档 / 模型 / 模式 / 工作区 / JSON）：回执是 state + notice */
  set: (key: SetKey, value: string) => void;
  /** 斜杠命令原样下发，回执走 notice */
  command: (text: string) => void;
  /** /new：后端起新会话，本地先把对话流清空 */
  newSession: () => void;
  /** 切会话：后端回 sessions + state + history，对话流按 history 整段重建 */
  useSession: (id: string) => void;
}

/** 订阅 transport，维护会话状态。 */
export function useAgentStream(transport: AgentTransport): AgentStream {
  const [state, setState] = useState<SessionState>(INITIAL);
  const transportRef = useRef(transport);
  transportRef.current = transport;

  useEffect(() => {
    // 重新挂载时清空，避免上一次会话的残影
    setState(INITIAL);
    return transport.subscribe((ev) => setState((prev) => reduce(prev, ev)));
  }, [transport]);

  const send = useCallback((cmd: AgentCommand) => {
    // 本地先行：把用户消息立即入列，不等后端回显（少一帧延迟）
    if (cmd.type === 'prompt') {
      setState((prev) => ({
        ...prev,
        error: null,
        streaming: true,
        items: [
          ...prev.items,
          { kind: 'user', id: `u${prev.items.length}`, text: cmd.text, streaming: false, at: clock(new Date()) },
        ],
      }));
    }
    transportRef.current.send(cmd);
  }, []);

  const toggleTool = useCallback((id: string) => {
    setState((prev) => ({
      ...prev,
      items: prev.items.map((it) => (it.kind === 'tool' && it.id === id ? { ...it, open: !it.open } : it)),
    }));
  }, []);

  const answer = useCallback(
    (text: string) => {
      setState((prev) => ({ ...prev, pendingAsk: null }));
      transportRef.current.send({ type: 'answer', text });
    },
    [],
  );

  const refresh = useCallback((what: RefreshKind) => {
    transportRef.current.send({ type: what });
  }, []);

  const set = useCallback((key: SetKey, value: string) => {
    transportRef.current.send({ type: 'set', key, value });
  }, []);

  const command = useCallback((text: string) => {
    transportRef.current.send({ type: 'command', text });
  }, []);

  const newSession = useCallback(() => {
    setState((prev) => ({ ...prev, items: [], error: null, pendingAsk: null }));
    transportRef.current.send({ type: 'new_session' });
  }, []);

  const useSession = useCallback((id: string) => {
    transportRef.current.send({ type: 'session', id });
  }, []);

  return useMemo(
    () => ({ state, send, toggleTool, answer, refresh, set, command, newSession, useSession }),
    [state, send, toggleTool, answer, refresh, set, command, newSession, useSession],
  );
}