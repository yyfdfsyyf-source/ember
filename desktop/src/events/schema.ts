/**
 * Ember 事件协议（与 C++ 侧 `agent.exe --serve` 的 NDJSON 一一对应）。
 *
 * 改动这里之前先对齐 agent/src/app.cpp 的 runServe()：
 * 命令是 stdin 一行一个 JSON，事件是 stdout 一行一个 JSON。
 */

/** 后端 → 前端的事件。type 与 C++ serveEmitStreamEvent() 的分支同名。 */
export type AgentEvent =
  | {
      type: 'ready';
      version: string;
      model: string;
      mode: string;
      cwd: string;
      session: string;
      /** 上下文预算（token）与成本预算（micro-USD），握手时就给，圆环不用等第一轮结束 */
      ctxBudget?: number;
      costBudgetMicroUsd?: number;
    }
  /**
   * 外壳状态：随 ready 到一次，之后每次 set / 切会话再发一次。
   * 顶栏的模型与思考档、状态条、会话栏高亮、工作区列表全读这里。
   */
  | {
      type: 'state';
      version: string;
      model: string;
      thinking: string;
      thinkingStyle: string;
      thinkingBinary: boolean;
      mode: string;
      jsonMode: boolean;
      cwd: string;
      sessionDir: string;
      sessionId: string;
      sessionTitle: string;
      ctxBudget?: number;
      costBudgetMicroUsd?: number;
      workspace: string;
      workspaceGuard: boolean;
      busy: boolean;
    }
  | { type: 'sessions'; dir: string; list: SessionInfo[] }
  | { type: 'model_list'; list: ModelInfo[]; source: 'settings' | 'fetched' }
  | { type: 'workspaces'; list: WorkspaceInfo[]; active: number; guard: boolean }
  /** TUI 的 kCommands 表本体：命令面板直接列这份，不再抄一份会漂移的清单。 */
  | { type: 'commands'; list: CommandInfo[] }
  /**
   * 当前 messages_ 的完整回放（切会话、以及启动时恢复了历史会话）：
   * 工具调用与结果都带，桌面端因此和终端看到同一段历史。
   */
  | { type: 'history'; list: HistoryRow[] }
  /** 斜杠命令与设置变更的回执（TUI 的状态行在桌面端没有落点，走这条）。 */
  | { type: 'notice'; text: string }
  /** 后端清了对话（/clear），前端同步清空。 */
  | { type: 'clear' }
  /**
   * 用户消息。只用于回放会话历史（P3 读 sessions 目录时用）：
   * 实时发送时前端已本地入列，后端不回显这一条，避免出现两条。
   */
  | { type: 'user'; text: string }
  | { type: 'delta'; text: string }
  | { type: 'reasoning'; text: string }
  | { type: 'tool_call'; name: string; args: string }
  | { type: 'tool_result'; name: string; result: string }
  | { type: 'ask'; question: string }
  | { type: 'models'; text: string; rc: number }
  | { type: 'background'; text: string }
  | { type: 'error'; message: string }
  | { type: 'turn_end'; rc: number }
  /**
   * 用量。promptTokens/completionTokens/cachedTokens 是本会话累计；
   * ctxTokens 是当前上下文占用（喂给模型的估算 token 数），ctxBudget 是压缩阈值；
   * 金额一律整数 micro-USD（1e-6 美元），避免浮点噪声。
   */
  | {
      type: 'usage';
      turns: number;
      steps: number;
      llmMs: number;
      toolMs: number;
      promptTokens: number;
      completionTokens: number;
      cachedTokens: number;
      ctxTokens?: number;
      ctxBudget?: number;
      costMicroUsd?: number;
      costBudgetMicroUsd?: number;
    };

/** 历史回放的一行：文本气泡、工具卡、以及回填给工具卡的结果。 */
export type HistoryRow =
  | { kind: 'user'; text: string }
  | { kind: 'assistant'; text: string }
  | { kind: 'tool'; id: string; name: string; args: string }
  | { kind: 'tool_result'; id: string; result: string };

/** 会话文件的一行（sessions/ 目录扫描结果，不含正文）。 */
export interface SessionInfo {
  id: string;
  title: string;
  model: string;
  msgCount: number;
  /** Unix 秒 */
  savedAt: number;
  current: boolean;
}

export interface ModelInfo {
  name: string;
  /** 所属分组（空 = 顶层默认连接未分组） */
  group: string;
  provider: string;
  current: boolean;
}

export interface WorkspaceInfo {
  index: number;
  path: string;
  current: boolean;
}

export interface CommandInfo {
  /** 含前导斜杠，如 "/think" */
  name: string;
  desc: string;
  key: string;
  needsArg: boolean;
}

/** 可设置的键：与 C++ App::serveSet 一一对应。 */
export type SetKey = 'thinking' | 'model' | 'mode' | 'workspace' | 'json';
export type RefreshKind =
  | 'state'
  | 'models'
  | 'sessions'
  | 'workspaces'
  | 'commands'
  | 'history'
  | 'usage';

/** 前端 → 后端的命令。 */
export type AgentCommand =
  | { type: 'prompt'; text: string }
  | { type: 'answer'; text: string }
  /** 斜杠命令原样交给 C++ 的 runCommand，回执走 notice */
  | { type: 'command'; text: string }
  | { type: 'set'; key: SetKey; value: string }
  | { type: 'session'; id: string }
  /** 无参数的读取/动作类命令，一次列全（{type:'state'} 等） */
  | { type: RefreshKind }
  | { type: 'new_session' | 'compact' | 'clear' | 'shutdown' };

/** 传输层：P1 用 mock，P2 换成 Tauri sidecar，不改上面任何类型。 */
export interface AgentTransport {
  send(cmd: AgentCommand): void;
  subscribe(handler: (ev: AgentEvent) => void): () => void;
}

/** 工具族 → 色相（固定映射，见设计系统 §3.5；不允许按调用频次重新分配）。 */
export type ToolFamily = 'file' | 'exec' | 'search' | 'other';

export interface FamilyStyle {
  family: ToolFamily;
  color: string;
}

const EXEC_TOOLS = new Set([
  'shell_exec',
  'verify',
  'background_start',
  'background_status',
  'background_kill',
  'background_list',
  'run_code',
  'subagent',
  'subagent_parallel',
]);

const SEARCH_TOOLS = new Set([
  'grep',
  'glob',
  'web_fetch',
  'web_search',
  'rag_index',
  'rag_search',
  'browser_open',
  'browser_snapshot',
  'browser_eval',
]);

/** 工具名 → 色相（H7 文件类 / H6 执行类 / H8 检索类 / 其余中性）。 */
export function toolFamily(name: string): FamilyStyle {
  if (EXEC_TOOLS.has(name)) return { family: 'exec', color: 'var(--h6)' };
  if (SEARCH_TOOLS.has(name)) return { family: 'search', color: 'var(--h8)' };
  if (name.startsWith('browser_') || name.startsWith('desktop_')) return { family: 'search', color: 'var(--h8)' };
  if (name === 'list_tools' || name === 'get_config') return { family: 'other', color: 'var(--text-3)' };
  return { family: 'file', color: 'var(--h7)' };
}

/** 工具族的中文名（用量页的分布图用）。 */
export const FAMILY_LABEL: Record<ToolFamily, string> = {
  file: '文件类',
  exec: '执行类',
  search: '检索/网络类',
  other: '思考 / MCP',
};