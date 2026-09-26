/**
 * 对话屏：消息流（用户 / 助手 / 思考 / 错误 / 工具卡）。
 *
 * 类名与原型一一对应：.msg-user / .msg / .think / .tool / .code / .caret。
 * 助手文本按 ``` 围栏切块渲染成 .code，`` `x` `` 渲染成内联 code；
 * 流式期间最后一段挂 .caret（CSS 里 1s steps 闪动）。
 */
import { useEffect, useRef, type CSSProperties, type ReactElement, type ReactNode, type TransitionEvent } from 'react';
import { toolFamily } from '../events/schema';
import type { ChatItem, ToolItem } from '../events/useAgentStream';
import { Composer, rise } from './Shell';

export type MsgItem = ChatItem | ToolItem;
type Step = () => number;

/** 交错入场的最大步进（与原型一致：0–9） */
const MAX_STAGGER = 9;

/* --------------------------------------------------------------- 文本分块 */
interface Piece {
  kind: 'text' | 'code';
  lang: string;
  body: string;
}

/** 按 ``` 围栏切块；奇数个围栏时最后一块是"进行中的代码块"，流式安全。 */
function parsePieces(text: string): Piece[] {
  return text.split('```').flatMap((part, i): Piece[] => {
    if (i % 2 === 0) return part === '' ? [] : [{ kind: 'text', lang: '', body: part }];
    const nl = part.indexOf('\n');
    return [
      {
        kind: 'code',
        lang: nl === -1 ? '' : part.slice(0, nl).trim(),
        body: nl === -1 ? part : part.slice(nl + 1),
      },
    ];
  });
}

/** 内联 `code`。 */
function inlineCode(text: string): ReactNode {
  return text
    .split(/(`[^`]*`)/g)
    .filter((seg) => seg !== '')
    .map((seg, i) =>
      seg.startsWith('`') && seg.endsWith('`') && seg.length > 1 ? <code key={i}>{seg.slice(1, -1)}</code> : seg,
    );
}

/* ------------------------------------------------------------------ 消息 */
interface AssistantBodyProps {
  text: string;
  streaming: boolean;
  next: Step;
}

function AssistantBody({ text, streaming, next }: AssistantBodyProps): ReactElement {
  const pieces = parsePieces(text);
  return (
    <>
      {pieces.map((piece, idx) => {
        const tail = idx === pieces.length - 1;
        const caret = streaming && tail ? <span className="caret" /> : null;

        if (piece.kind === 'code') {
          return (
            <div className="code rise" style={rise(next())} key={`c${idx}`}>
              <div className="code-head">
                <span className="lang">{piece.lang || 'code'}</span>
              </div>
              <pre>
                {piece.body}
                {caret}
              </pre>
            </div>
          );
        }

        const paras = piece.body.split(/\n{2,}/).filter((s) => s.trim() !== '');
        return paras.map((para, pi) => {
          const last = caret !== null && pi === paras.length - 1;
          return (
            <p className={last ? 'streaming rise' : 'rise'} style={rise(next())} key={`t${idx}-${pi}`}>
              {inlineCode(para.trim())}
              {last ? caret : null}
            </p>
          );
        });
      })}
    </>
  );
}

/* ---------------------------------------------------------------- 工具卡 */
interface ToolCardProps {
  item: ToolItem;
  i: number;
  onToggle: (id: string) => void;
}

/**
 * 工具卡：4px 工具族色条 + 状态圆 + 增量三角。
 * 展开/收起是 150ms 高度过渡（规范允许的「展开」档），结束后交回 auto，
 * 这样结果流式变长时不会再被固定高度裁切。
 */
function ToolCard({ item, i, onToggle }: ToolCardProps): ReactElement {
  const bodyRef = useRef<HTMLDivElement>(null);
  const initialOpen = useRef(item.open).current;
  const fam = toolFamily(item.name);

  const toggle = (): void => {
    const body = bodyRef.current;
    if (body) {
      const open = item.open;
      body.style.height = open ? `${body.scrollHeight}px` : '0px';
      requestAnimationFrame(() => {
        body.style.height = open ? '0px' : `${body.scrollHeight}px`;
      });
    }
    onToggle(item.id);
  };

  const settle = (e: TransitionEvent<HTMLDivElement>): void => {
    if (e.propertyName !== 'height') return;
    const body = bodyRef.current;
    if (body && item.open) body.style.height = 'auto';
  };

  return (
    <div className="tool rise" style={{ '--i': Math.min(i, MAX_STAGGER), '--fam': fam.color } as CSSProperties} data-open={item.open}>
      <div className="tool-head" onClick={toggle} role="button" tabIndex={0} onKeyDown={(e) => e.key === 'Enter' && toggle()}>
        <span className={`dot ${item.state === 'run' ? 'run' : item.state === 'bad' ? 'bad' : 'ok'}`} />
        <span className="name">{item.name}</span>
        <span className="arg">{item.args}</span>
        {item.state === 'ok' ? <span className="delta" /> : null}
        <span className="tri" />
      </div>
      <div className="tool-body" ref={bodyRef} style={{ height: initialOpen ? 'auto' : 0 }} onTransitionEnd={settle}>
        <div className="code">
          <div className="code-head">
            <span className="lang">{item.name} · 输出</span>
          </div>
          <pre>{item.result || (item.state === 'run' ? '运行中…' : '（无输出）')}</pre>
        </div>
      </div>
    </div>
  );
}

/* ---------------------------------------------------------- 轮次分组 */
interface UserTurn {
  kind: 'user';
  key: string;
  text: string;
  at: string;
}

interface AgentTurn {
  kind: 'agent';
  key: string;
  at: string;
  items: MsgItem[];
}

type Turn = UserTurn | AgentTurn;

/** 用户消息各自成块；其余（思考/正文/工具/错误）归到同一轮助手消息里。 */
function groupItems(items: MsgItem[]): Turn[] {
  const turns: Turn[] = [];
  for (const it of items) {
    if (it.kind === 'user') {
      turns.push({ kind: 'user', key: it.id, text: it.text, at: it.at });
      continue;
    }
    const last = turns[turns.length - 1];
    if (last && last.kind === 'agent') last.items.push(it);
    else turns.push({ kind: 'agent', key: it.id, at: it.at, items: [it] });
  }
  return turns;
}

function renderItem(it: MsgItem, next: Step, onToggle: (id: string) => void): ReactElement {
  if (it.kind === 'tool') return <ToolCard key={it.id} item={it} i={next()} onToggle={onToggle} />;

  if (it.kind === 'reasoning') {
    return (
      <details className="think slide-l" style={rise(next())} key={it.id}>
        <summary>
          <span className="dot info" />
          <span className="lbl">{it.streaming ? '思考中' : '思考'}</span>
          <span className="tri" />
        </summary>
        <div className="txt">{it.text}</div>
      </details>
    );
  }

  if (it.kind === 'notice') {
    // 命令回执：TUI 里那是状态行的一行字，桌面端落在对话流末尾（H2 思考色）
    return (
      <div className="notice rise" style={rise(next())} key={it.id}>
        {it.text}
      </div>
    );
  }

  if (it.kind === 'error') {
    return (
      <div className="code rise" style={rise(next())} key={it.id}>
        <div className="code-head">
          <span className="lang">错误</span>
          <span className="dot bad" />
        </div>
        <pre>{it.text}</pre>
      </div>
    );
  }

  return <AssistantBody key={it.id} text={it.text} streaming={it.streaming} next={next} />;
}

/* ------------------------------------------------------------------ 屏 */
export interface ChatScreenProps {
  items: MsgItem[];
  streaming: boolean;
  onToggleTool: (id: string) => void;
  onSend: (text: string) => void;
  onCommand: (text: string) => void;
  draft?: { text: string; n: number };
}

export function ChatScreen({ items, streaming, onToggleTool, onSend, onCommand, draft }: ChatScreenProps): ReactElement {
  let step = 0;
  const next: Step = () => Math.min(step++, MAX_STAGGER);
  const flow = useRef<HTMLDivElement>(null);

  // 贴着底部才跟随滚动（与 TUI 的 scrollAnchor_ 同语义）：
  // 用户往上翻看历史时，流式输出不打断他的阅读位置
  useEffect(() => {
    const el = flow.current;
    if (!el) return;
    if (el.scrollHeight - el.scrollTop - el.clientHeight < 80) el.scrollTop = el.scrollHeight;
  }, [items]);

  return (
    <div className="chat">
      <div className="flow" ref={flow}>
        <div className="col">
          {groupItems(items).map((turn) =>
            turn.kind === 'user' ? (
              <div className="msg-user rise" style={rise(next())} key={turn.key}>
                {turn.text}
              </div>
            ) : (
              <div className="msg" key={turn.key}>
                <div className="msg-head slide-l" style={rise(next())}>
                  <span className="dot info" />
                  <span className="who">Ember</span>
                  {/* 历史回放没有逐条时间戳，没有就不显示，不拿当前时间凑 */}
                  {turn.at ? <span className="at">{turn.at}</span> : null}
                </div>
                {turn.items.map((it) => renderItem(it, next, onToggleTool))}
              </div>
            ),
          )}
        </div>
      </div>
      <Composer onSend={onSend} onCommand={onCommand} disabled={streaming} draft={draft} />
    </div>
  );
}