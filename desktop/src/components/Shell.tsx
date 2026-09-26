/** 外壳：顶栏 / 会话栏 / 检查器 / 状态条 / 输入区。全部沿用原型类名，视觉零漂移。 */
import { useEffect, useRef, useState, type CSSProperties, type KeyboardEvent, type ReactElement } from 'react';
import {
  BrandMark,
  IconChat,
  IconEmpty,
  IconPlus,
  IconSend,
  IconSpec,
  IconTheme,
  IconUsage,
  IconWinClose,
  IconWinMax,
  IconWinMin,
  Rect,
  Triangle,
} from '../geo/Geo';
import type { ModelInfo, SessionInfo, WorkspaceInfo } from '../events/schema';
import type { ScreenId } from '../types';
import type { SessionState, ToolItem, UsageState } from '../events/useAgentStream';
import { closeWindow, minimize, toggleMaximize } from '../shell/host';

/** 交错入场延时（一次编排好的入场，60ms 步进）：只给 --i，类名在 JSX 上。 */
export function rise(i: number): CSSProperties {
  return { '--i': i } as CSSProperties;
}

/* ------------------------------------------------------- 下拉选择器（无边框） */
interface PickRow {
  key: string;
  label: string;
  hint?: string;
  current: boolean;
}

interface PickerProps {
  label: string;
  title: string;
  value: string;
  dot: string;
  rows: PickRow[];
  note?: string;
  onPick: (key: string) => void;
}

/**
 * 无边框弹出：面层从 --s1 跳到 --s3，靠明度差浮出来（设计系统 §0.3）。
 * 选中行用 --sel 铺底 + 左 4px 直条，与 §3.5 一致。
 */
function Picker({ label, title, value, dot, rows, note, onPick }: PickerProps): ReactElement {
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const away = (e: MouseEvent): void => {
      if (!box.current?.contains(e.target as Node)) setOpen(false);
    };
    const esc = (e: globalThis.KeyboardEvent): void => {
      if (e.key === 'Escape') setOpen(false);
    };
    document.addEventListener('mousedown', away);
    document.addEventListener('keydown', esc);
    return () => {
      document.removeEventListener('mousedown', away);
      document.removeEventListener('keydown', esc);
    };
  }, [open]);

  return (
    <div className="pick" ref={box}>
      <button
        type="button"
        className="chip btn-ghost"
        title={title}
        aria-expanded={open}
        aria-haspopup="listbox"
        onClick={() => setOpen((v) => !v)}
      >
        <span className={`dot ${dot}`} />
        {value || label}
        <svg width={6} height={4} viewBox="0 0 6 4" aria-hidden="true">
          <Triangle points={open ? '0,4 6,4 3,0' : '0,0 6,0 3,4'} fill="var(--text-2)" />
        </svg>
      </button>
      {open ? (
        <div className="pop" role="listbox" aria-label={title}>
          {rows.length === 0 ? <div className="pop-note">后端还没有给出候选</div> : null}
          {rows.map((r) => (
            <button
              type="button"
              role="option"
              aria-selected={r.current}
              key={r.key}
              className="pop-row"
              onClick={() => {
                setOpen(false);
                onPick(r.key);
              }}
            >
              <span className="t">{r.label}</span>
              {r.hint ? <span className="h mono">{r.hint}</span> : null}
            </button>
          ))}
          {note ? <div className="pop-note">{note}</div> : null}
        </div>
      ) : null}
    </div>
  );
}

const THINK_LEVELS = ['auto', 'none', 'minimal', 'low', 'medium', 'high'];

/* ------------------------------------------------------------------ 顶栏 */
export interface TopBarProps {
  screen: ScreenId;
  onScreen: (id: ScreenId) => void;
  onTheme: () => void;
  /** 外壳读数：全部来自后端 state 事件，前端不再有默认模型/档位常量 */
  read: { model: string; thinking: string; thinkingBinary: boolean; mode: string; connected: boolean; mock: boolean };
  models: ModelInfo[];
  onModel: (name: string) => void;
  onThinking: (level: string) => void;
  onPalette: () => void;
}

export function TopBar({ screen, onScreen, onTheme, read, models, onModel, onThinking, onPalette }: TopBarProps): ReactElement {
  const items: Array<{ id: ScreenId; label: string; icon: ReactElement }> = [
    { id: 'chat', label: '对话', icon: <IconChat /> },
    { id: 'usage', label: '用量', icon: <IconUsage /> },
    { id: 'empty', label: '空态', icon: <IconEmpty /> },
    { id: 'spec', label: '规范自检', icon: <IconSpec /> },
  ];

  const modelRows: PickRow[] = models.map((m) => ({
    key: m.name,
    label: m.name,
    hint: `${m.group || 'default'} · ${m.provider}`,
    current: m.current,
  }));
  const thinkRows: PickRow[] = THINK_LEVELS.map((l) => ({ key: l, label: l, current: l === read.thinking }));

  return (
    // "deep"：空白处（.spacer/.brand/.chip）也能拖；带 tabindex/role 的按钮是 clickable，
    // Tauri 的 drag.js 会让它们挡住拖动，所以窗口按钮不会被误当成拖动区
    <header className="topbar" data-tauri-drag-region="deep">
      <div className="brand">
        <BrandMark />
        <b>Ember</b>
        <span>Desktop</span>
        {read.mock ? <span className="tag mock">MOCK</span> : null}
      </div>

      <nav className="switch" aria-label="界面切换">
        {items.map((it) => (
          <button
            key={it.id}
            type="button"
            className="ctl"
            aria-current={screen === it.id}
            title={it.label}
            onClick={() => onScreen(it.id)}
          >
            {it.icon}
          </button>
        ))}
      </nav>

      <button type="button" className="ctl" title="命令面板 (Ctrl+P)" onClick={onPalette}>
        <IconPalette />
      </button>

      <div className="spacer" />

      <Picker
        title="切换模型（服务商随分组自动切换）"
        label="未连接"
        value={read.model}
        dot={read.connected ? 'ok' : 'bad'}
        rows={modelRows}
        onPick={onModel}
      />
      <Picker
        title="思考强度：/think 的同款六档"
        label="think"
        value={`think:${read.thinking || '—'}`}
        dot="info"
        rows={thinkRows}
        note={read.thinkingBinary ? '此服务商只分开关，minimal/low/high 等效' : undefined}
        onPick={onThinking}
      />

      <button type="button" className="ctl" id="themeBtn" title="切换深浅主题（240ms 交叉淡入）" onClick={onTheme}>
        <IconTheme />
      </button>

      <div style={{ display: 'flex' }}>
        <div className="wctl" title="最小化" role="button" tabIndex={0} onClick={() => void minimize()} onKeyDown={keyAct(() => void minimize())}>
          <IconWinMin />
        </div>
        <div className="wctl" title="最大化" role="button" tabIndex={0} onClick={() => void toggleMaximize()} onKeyDown={keyAct(() => void toggleMaximize())}>
          <IconWinMax />
        </div>
        <div className="wctl close" title="关闭" role="button" tabIndex={0} onClick={() => void closeWindow()} onKeyDown={keyAct(() => void closeWindow())}>
          <IconWinClose />
        </div>
      </div>
    </header>
  );
}

/** 命令面板入口：矩形 + 内部两个直角块（与 §2.3 的终端图元同一语言）。 */
function IconPalette(): ReactElement {
  return (
    <svg width={16} height={16} viewBox="0 0 16 16" aria-hidden="true">
      <Rect w={16} h={16} fill="var(--s3)" />
      <Rect x={3} y={4} w={10} h={2} fill="var(--text-2)" />
      <Rect x={3} y={9} w={6} h={2} fill="var(--accent)" />
    </svg>
  );
}

/** 键盘等价于点击（Enter/Space）。 */
function keyAct(run: () => void): (e: KeyboardEvent<HTMLDivElement>) => void {
  return (e) => {
    if (e.key !== 'Enter' && e.key !== ' ') return;
    e.preventDefault();
    run();
  };
}

/* ---------------------------------------------------------------- 会话栏 */
export interface RailProps {
  /** 后端 sessions 事件的真列表（sessions/ 目录扫描结果） */
  sessions: SessionInfo[];
  activeId: string;
  busy: boolean;
  onSelect: (id: string) => void;
  onCreate: () => void;
  workspaces: WorkspaceInfo[];
  workspaceGuard: boolean;
  onUseWorkspace: (index: number) => void;
  onGuard: (on: boolean) => void;
}

/** 相对时间：刚刚 / 2h / 昨天 / 周三（超过一周给日期）。 */
function relTime(unixSec: number): string {
  const d = Math.max(0, Math.floor(Date.now() / 1000) - unixSec);
  if (d < 60) return '刚刚';
  if (d < 3600) return `${Math.floor(d / 60)}m`;
  if (d < 86_400) return `${Math.floor(d / 3600)}h`;
  if (d < 2 * 86_400) return '昨天';
  const at = new Date(unixSec * 1000);
  if (d < 7 * 86_400) return ['周日', '周一', '周二', '周三', '周四', '周五', '周六'][at.getDay()];
  return `${at.getMonth() + 1}/${at.getDate()}`;
}

/** 路径末段作为工作区名（rail 宽度有限，完整路径放 title）。 */
export function baseName(p: string): string {
  const parts = p.split(/[\\/]/).filter((s) => s !== '');
  return parts[parts.length - 1] ?? p;
}

export function Rail({
  sessions,
  activeId,
  busy,
  onSelect,
  onCreate,
  workspaces,
  workspaceGuard,
  onUseWorkspace,
  onGuard,
}: RailProps): ReactElement {
  return (
    <aside className="rail">
      <div className="rail-head">
        <span>会话</span>
        <button type="button" className="ctl" title="新建会话（后端 /new）" style={{ width: 24, height: 24 }} onClick={onCreate}>
          <IconPlus />
        </button>
      </div>

      {/* 会话行不加 .rise/.slide-*：那些规则只在 .screen[data-active] 与 .inspector 内生效，
          挂在其他地方会永远是 opacity:0 的隐形元素 */}
      <div className="rail-list">
        {sessions.length === 0 ? (
          <div className="rail-empty">后端还没有回会话列表</div>
        ) : (
          sessions.map((s) => {
            const active = s.id === activeId;
            return (
              <div
                key={s.id}
                className="sess"
                aria-current={active}
                onClick={() => onSelect(s.id)}
                role="button"
                tabIndex={0}
                title={`${s.id}\n${s.model}`}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') onSelect(s.id);
                }}
              >
                <div className="t">
                  <span className={`dot ${active && busy ? 'run' : active ? 'ok' : ''}`} />
                  <span>{s.title || '未命名会话'}</span>
                </div>
                <span className="meta">
                  {s.msgCount} 条 · {relTime(s.savedAt)}
                </span>
              </div>
            );
          })
        )}
      </div>

      <div className="rail-foot">
        <div className="rail-head" style={{ paddingBottom: 8 }}>
          <span>工作区</span>
          <button
            type="button"
            className="ctl"
            style={{ width: 24, height: 24 }}
            title={workspaceGuard ? '越界审批：开（点击关闭）' : '越界审批：关（点击开启）'}
            onClick={() => onGuard(!workspaceGuard)}
          >
            <span className={`dot ${workspaceGuard ? 'ok' : 'bad'}`} />
          </button>
        </div>
        {workspaces.length === 0 ? (
          <div className="rail-empty">未登记工作区（不限制路径边界）</div>
        ) : (
          workspaces.map((w) => (
            <div
              key={w.index}
              className="ws-item"
              role="button"
              tabIndex={0}
              title={w.path}
              aria-current={w.current}
              onClick={() => onUseWorkspace(w.index)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') onUseWorkspace(w.index);
              }}
            >
              <svg width={4} height={16} viewBox="0 0 4 16" aria-hidden="true">
                {/* 工作区身份色：按序号在 H6/H7 间固定轮转，不随机（§3.5） */}
                <Rect w={4} h={16} fill={w.index % 2 === 0 ? 'var(--h6)' : 'var(--h7)'} />
              </svg>
              <span>{baseName(w.path)}</span>
            </div>
          ))
        )}
      </div>
    </aside>
  );
}

/* ---------------------------------------------------------------- 检查器 */
export interface InspectorProps {
  usage: UsageState;
  tools: ToolItem[];
  pendingAsk: string | null;
  onAnswer: (text: string) => void;
}

/** 后端没报预算时（0 = 不限），圆环不给百分比。 */
export function Inspector({ usage, tools, pendingAsk, onAnswer }: InspectorProps): ReactElement {
  const ctxUsed = usage.ctxBudget > 0 ? Math.min(100, Math.round((usage.ctxTokens / usage.ctxBudget) * 100)) : 0;
  const circumference = 188; // r = 30
  const dash = Math.round(circumference * (1 - ctxUsed / 100));

  return (
    <aside className="inspector">
      <section className="sect slide-r" style={rise(1)}>
        <h3>本次会话</h3>
        <div className="panel">
          <div className="ring-wrap">
            <svg width={72} height={72} viewBox="0 0 72 72" role="img" aria-label="上下文占用">
              <circle className="ring-track" cx={36} cy={36} r={30} />
              <circle className="ring-fill" cx={36} cy={36} r={30} style={{ '--full': circumference, '--dash': dash } as CSSProperties} />
              <text className="ring-txt" x={36} y={36} textAnchor="middle" dominantBaseline="middle">
                {ctxUsed}%
              </text>
            </svg>
            <div className="mono" style={{ fontSize: 12, lineHeight: '18px', color: 'var(--text-2)' }}>
              {usage.ctxBudget > 0
                ? `上下文 ${fmtTokens(usage.ctxTokens)} / ${fmtTokens(usage.ctxBudget)}`
                : `上下文 ${fmtTokens(usage.ctxTokens)} · 未设上限`}
              <br />
              每轮压缩后自动回落
            </div>
          </div>
        </div>
      </section>

      <section className="sect slide-r" style={rise(2)}>
        <h3>工具调用</h3>
        <div className="panel">
          {tools.length === 0 ? (
            <div className="fam-row" style={{ gridTemplateColumns: '1fr' }}>
              <span style={{ color: 'var(--text-3)', fontSize: 12 }}>本轮还没有工具调用</span>
            </div>
          ) : (
            tools.slice(-4).map((t) => (
              <div key={t.id} className="fam-row" style={{ gridTemplateColumns: '1fr' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 8, width: '100%' }}>
                  <span className={`dot ${t.state === 'run' ? 'run' : t.state === 'bad' ? 'bad' : 'ok'}`} />
                  <span className="mono" style={{ fontSize: 12 }}>
                    {t.name}
                  </span>
                  <span style={{ marginLeft: 'auto', color: 'var(--text-3)', fontSize: 11 }}>
                    {t.state === 'run' ? '运行中' : t.state === 'bad' ? '失败' : '完成'}
                  </span>
                </div>
              </div>
            ))
          )}
        </div>
      </section>

      <section className="sect slide-r" style={rise(3)}>
        <h3>本会话用量</h3>
        <div className="panel">
          <div className="kpi">
            <div className="k">输入 / 输出</div>
            <div className="v" style={{ fontSize: 20, lineHeight: '28px' }}>
              {usage.promptTokens || usage.completionTokens
                ? `${fmtTokens(usage.promptTokens)} / ${fmtTokens(usage.completionTokens)}`
                : '—'}
            </div>
          </div>
          <div className="legend" style={{ marginTop: 12 }}>
            <span>
              <i style={{ background: 'var(--accent)' }} />
              输出
            </span>
            <span>
              <i style={{ background: 'var(--data-2)' }} />
              输入
            </span>
          </div>
        </div>
      </section>

      <section className="sect slide-r" style={rise(4)}>
        <h3>待批准</h3>
        <div className="panel">
          {pendingAsk ? (
            <>
              <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                <span className="dot" />
                <span style={{ fontSize: 12, lineHeight: '18px', color: 'var(--text-2)' }}>{pendingAsk}</span>
              </div>
              <div style={{ display: 'flex', gap: 8, marginTop: 12 }}>
                <button type="button" className="ctl" style={allowBtn} onClick={() => onAnswer('允许')}>
                  允许
                </button>
                <button type="button" className="ctl" style={denyBtn} onClick={() => onAnswer('拒绝')}>
                  拒绝
                </button>
              </div>
            </>
          ) : (
            <span style={{ color: 'var(--text-3)', fontSize: 12 }}>没有待批准的越界操作</span>
          )}
        </div>
      </section>
    </aside>
  );
}

const allowBtn: CSSProperties = {
  width: 'auto',
  padding: '0 12px',
  background: 'var(--accent)',
  color: 'var(--on-color)',
  fontSize: 12,
};
const denyBtn: CSSProperties = { width: 'auto', padding: '0 12px', background: 'var(--s3)', color: 'var(--text-1)', fontSize: 12 };

/* ---------------------------------------------------------------- 状态条 */
export interface StatusBarProps {
  /** 与顶栏同一份 state 读数：模型与思考档只在顶栏出现一次，这里不重复 */
  read: { mode: string; version: string; cwd: string; sessionTitle: string; sessionId: string; connected: boolean };
  usage: UsageState;
}

export function StatusBar({ read, usage }: StatusBarProps): ReactElement {
  const [size, setSize] = useState(() => ({ w: window.innerWidth, h: window.innerHeight }));

  useEffect(() => {
    const onResize = (): void => setSize({ w: window.innerWidth, h: window.innerHeight });
    window.addEventListener('resize', onResize);
    return () => window.removeEventListener('resize', onResize);
  }, []);

  // 全部来自 agent 上报：没有用量就不显示数字（不再造演示值）
  const cachePct = usage.promptTokens > 0 ? Math.round((usage.cachedTokens / usage.promptTokens) * 100) : null;
  const cost = usage.costMicroUsd / 1e6;

  return (
    <footer className="statusbar">
      <span className="ell">{read.connected ? read.sessionTitle || '未命名会话' : '未连接'}</span>
      <span>模式 {read.mode || '—'}</span>
      <b>v{read.version || '—'}</b>
      <span className="mono ell" title={read.cwd}>{read.cwd}</span>
      <div className="right">
        <span>缓存 {cachePct === null ? '—' : `${cachePct}%`}</span>
        <span>成本 {usage.costMicroUsd > 0 ? `$${cost.toFixed(3)}` : '—'}</span>
        <span className="mono">
          {size.w}x{size.h}
        </span>
      </div>
    </footer>
  );
}

/* ---------------------------------------------------------------- 输入区 */
export interface ComposerProps {
  onSend: (text: string) => void;
  /** 以 / 开头的输入走命令通道（后端 runCommand），不发给模型 */
  onCommand: (text: string) => void;
  disabled: boolean;
  /** 命令面板为「需要参数」的命令回填的草稿 */
  draft?: { text: string; n: number };
}

/** 输入区：直角矩形 + 右三角发送（设计系统 §6）。 */
export function Composer({ onSend, onCommand, disabled, draft }: ComposerProps): ReactElement {
  const [text, setText] = useState('');
  const boxRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (!draft) return;
    setText(draft.text);
    boxRef.current?.focus();
  }, [draft]);

  const submit = (): void => {
    const t = text.trim();
    if (!t) return;
    setText('');
    // 命令即使在这一轮还在跑也要能下（后端各命令自己判忙闲）
    if (t[0] === '/') onCommand(t);
    else if (!disabled) onSend(t);
  };

  return (
    <div className="dock">
      <div className="composer slide-r" style={rise(9)}>
        <input
          ref={boxRef}
          className="ph"
          placeholder="接着输入，/ 开头是命令（Ctrl+P 看全部）"
          value={text}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && !e.shiftKey) {
              e.preventDefault();
              submit();
            }
          }}
        />
        <button type="button" className="send" title="发送" onClick={submit} disabled={disabled}>
          <IconSend />
        </button>
      </div>
      <div className="hints">Enter 发送 · / 开头走命令 · Ctrl+P 命令面板 · 工具调用可点击展开</div>
    </div>
  );
}

/** token 计数：1.24M / 18.4k / 6k（整数不补 .0）。 */
function fmtTokens(n: number): string {
  const trim = (v: number): string => (Number.isInteger(v) ? String(v) : v.toFixed(1));
  if (n >= 1e6) return `${trim(n / 1e6)}M`;
  if (n >= 1e3) return `${trim(n / 1e3)}k`;
  return String(n);
}

export { fmtTokens };
export type { SessionState };