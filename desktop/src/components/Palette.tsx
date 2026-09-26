/**
 * 命令面板（Ctrl+P）。
 *
 * 清单来自后端的 commands 事件——也就是 C++ 里的 kCommands 表本体，
 * 桌面端不会列出终端里没有的命令，也不会漏掉新加的。需要参数的命令
 * 回填到输入框（带尾随空格），其余直接下发，回执走 notice。
 */
import { useEffect, useMemo, useRef, useState, type KeyboardEvent, type ReactElement } from 'react';
import type { CommandInfo } from '../events/schema';

export interface PaletteProps {
  commands: CommandInfo[];
  /** 打开时向后端要一次命令表 */
  onRequest: () => void;
  onRun: (cmd: CommandInfo) => void;
  onClose: () => void;
}

const ROW_H = 40;

export function Palette({ commands, onRequest, onRun, onClose }: PaletteProps): ReactElement {
  const [q, setQ] = useState('');
  const [sel, setSel] = useState(0);
  const input = useRef<HTMLInputElement>(null);
  const list = useRef<HTMLDivElement>(null);

  useEffect(() => {
    onRequest();
    input.current?.focus();
  }, [onRequest]);

  const rows = useMemo(() => {
    const k = q.trim().toLowerCase();
    if (k === '') return commands;
    return commands.filter((c) => c.name.toLowerCase().includes(k) || c.desc.toLowerCase().includes(k));
  }, [commands, q]);

  useEffect(() => {
    setSel(0);
  }, [q]);

  useEffect(() => {
    const box = list.current;
    if (!box) return;
    const top = sel * ROW_H;
    if (top < box.scrollTop) box.scrollTop = top;
    else if (top + ROW_H > box.scrollTop + box.clientHeight) box.scrollTop = top + ROW_H - box.clientHeight;
  }, [sel]);

  const keys = (e: KeyboardEvent<HTMLInputElement>): void => {
    if (e.key === 'Escape') {
      e.preventDefault();
      onClose();
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      setSel((s) => (rows.length ? (s + 1) % rows.length : 0));
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      setSel((s) => (rows.length ? (s - 1 + rows.length) % rows.length : 0));
    } else if (e.key === 'Enter' || e.key === 'Tab') {
      e.preventDefault();
      const c = rows[sel];
      if (c) onRun(c);
    }
  };

  return (
    <div className="pal-mask" onMouseDown={onClose}>
      <div className="pal" onMouseDown={(e) => e.stopPropagation()}>
        <div className="pal-head">
          <input
            ref={input}
            className="ph"
            placeholder="筛选命令"
            value={q}
            onChange={(e) => setQ(e.target.value)}
            onKeyDown={keys}
            aria-label="筛选命令"
          />
          <span className="pal-count mono">{rows.length}/{commands.length}</span>
        </div>
        <div className="pal-list" ref={list} role="listbox">
          {rows.length === 0 ? (
            <div className="pal-empty">{commands.length === 0 ? '后端还没有回命令表' : '没有匹配的命令'}</div>
          ) : (
            rows.map((c, i) => (
              <button
                type="button"
                role="option"
                aria-selected={i === sel}
                key={c.name}
                className="pal-row"
                onMouseEnter={() => setSel(i)}
                onClick={() => onRun(c)}
              >
                <span className="n mono">{c.name}{c.needsArg ? ' ' : ''}</span>
                <span className="d">{c.desc}</span>
                {c.key ? <span className="k mono">{c.key}</span> : null}
              </button>
            ))
          )}
        </div>
        <div className="pal-foot">↑↓ 选择 · Enter 执行 · Esc 关闭 · 带参数的命令会填进输入框</div>
      </div>
    </div>
  );
}
