/**
 * 图表与展示件：圆环进度 / 直角柱状图 / 工具族条 / 色板 / 动效档位 / 空态纹样。
 *
 * 图表一律等比缩放（不写 preserveAspectRatio="none"）：圆点在任意宽度下都是圆。
 * 颜色全部走 CSS 变量，深浅主题与 240ms 变量级交叉淡入才能生效。
 */
import { useEffect, useState, type CSSProperties, type ReactElement } from 'react';
import { Circle, Rect, Triangle } from '../geo/Geo';

/* ---------------------------------------------------------------- 圆环 */
export interface RingProps {
  /** 外框尺寸（正方形） */
  size: number;
  /** 半径 */
  r: number;
  /** 0–100 */
  pct: number;
  label: string;
  sub?: string;
}

/** 圆环进度：--full 是一圈弧长，--dash 是剩余弧长（CSS 里入场时从 --full 走到 --dash）。 */
export function Ring({ size, r, pct, label, sub }: RingProps): ReactElement {
  const c = Math.round(2 * Math.PI * r);
  const center = size / 2;
  const style = { '--full': c, '--dash': Math.round(c * (1 - pct / 100)) } as CSSProperties;

  return (
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} role="img" aria-label={label}>
      <circle className="ring-track" cx={center} cy={center} r={r} />
      <circle className="ring-fill" cx={center} cy={center} r={r} style={style} />
      <text className="ring-txt" x={center} y={center} textAnchor="middle" dominantBaseline="middle">
        {label}
      </text>
      {sub ? (
        <text className="ring-sub" x={center} y={center + Math.round(r * 0.45)} textAnchor="middle">
          {sub}
        </text>
      ) : null}
    </svg>
  );
}

/* -------------------------------------------------------------- 柱状图 */
export interface BarPoint {
  label: string;
  value: number;
}

export interface BarChartProps {
  data: BarPoint[];
  /** 基线 y（默认与原型一致：viewBox 640×180，基线 150） */
  base?: number;
  /** 图形语义由调用方给（这张图既画会话活动也可能画别的聚合） */
  label: string;
}

/** 直角柱状图 + 趋势折线：网格线是唯一允许的线，最高一根走强调色。 */
export function BarChart({ data, base = 150, label }: BarChartProps): ReactElement {
  const width = 640;
  const height = 180;
  const barW = 40;
  const step = 88;
  const x0 = 24;
  // 全 0 时不能除以 0：柱高取 0，图仍在，只是没有柱
  const max = Math.max(1, ...data.map((d) => d.value));
  const gridY = [base, Math.round((base * 2) / 3), Math.round(base / 3)];
  const bars = data.map((d, i) => {
    const h = Math.round((d.value / max) * base);
    return { x: x0 + i * step, y: base - h, h, cx: x0 + i * step + barW / 2, label: d.label, hot: i === data.length - 1 };
  });

  return (
    <svg className="chart" width="100%" viewBox={`0 0 ${width} ${height}`} role="img" aria-label={label}>
      {gridY.map((y) => (
        <line key={y} x1={0} y1={y} x2={width} y2={y} />
      ))}
      {bars.map((b, i) => (
        <rect
          key={b.label}
          className={b.hot ? 'bar hot' : 'bar'}
          x={b.x}
          y={b.y}
          width={barW}
          height={b.h}
          style={{ '--delay': `${i * 40}ms` } as CSSProperties}
        />
      ))}
      <polyline points={bars.map((b) => `${b.cx},${b.y}`).join(' ')} />
      {bars.map((b) => (
        <circle key={b.label} className="pt" cx={b.cx} cy={b.y} r={3} />
      ))}
      {bars.map((b) => (
        <text key={b.label} x={b.x} y={base + 20}>
          {b.label}
        </text>
      ))}
    </svg>
  );
}

/* ------------------------------------------------------------ 工具族条 */
export interface FamRow {
  label: string;
  /** 条宽百分比（相对最满的那一族），由本会话真实工具调用聚合而来 */
  pct: number;
  value: string;
  color: string;
}

export function FamBars({ rows }: { rows: FamRow[] }): ReactElement {
  return (
    <>
      {rows.map((r) => (
        <div className="fam-row" key={r.label}>
          <span className="n">{r.label}</span>
          <span className="bar">
            <i style={{ width: `${r.pct}%`, background: r.color }} />
          </span>
          <span className="v">{r.value}</span>
        </div>
      ))}
    </>
  );
}

/* ------------------------------------------------------------------ 色板 */
export interface SwatchItem {
  color: string;
  name: string;
  hint: string;
}

export function Swatches({ items }: { items: SwatchItem[] }): ReactElement {
  return (
    <div className="swatches">
      {items.map((s) => (
        <div className="sw" key={s.name}>
          <i style={{ background: s.color }} />
          <div className="n">{s.name}</div>
          <div className="h">{s.hint}</div>
        </div>
      ))}
    </div>
  );
}

/* -------------------------------------------------------------- 动效档位 */
interface MotionRowDef {
  dur: string;
  label: string;
  ms: string;
}

const MOTION_ROWS: MotionRowDef[] = [
  { dur: '--d-1', label: '悬停 / 按下', ms: '90ms' },
  { dur: '--d-2', label: '面板 / 列表 / 展开', ms: '150ms' },
  { dur: '--d-3', label: '页面级 / 主题切换', ms: '240ms' },
  { dur: '--d-4', label: '图表入场', ms: '400ms' },
];

/** 动效档位演示：重挂 <i>（key 变）即重启动画，切到本屏时自动依次播放。 */
export function MotionDemo({ active }: { active: boolean }): ReactElement {
  const [plays, setPlays] = useState<number[]>(() => MOTION_ROWS.map(() => 0));

  const play = (i: number): void => {
    setPlays((prev) => prev.map((n, idx) => (idx === i ? n + 1 : n)));
  };

  useEffect(() => {
    if (!active) return;
    const timers = MOTION_ROWS.map((_, i) => window.setTimeout(() => play(i), i * 120));
    return () => {
      timers.forEach((t) => window.clearTimeout(t));
    };
  }, [active]);

  return (
    <div id="motionDemo">
      {MOTION_ROWS.map((row, i) => (
        <div
          key={row.dur}
          className={plays[i] > 0 ? 'motion-row play' : 'motion-row'}
          style={{ '--dur': `var(${row.dur})`, cursor: 'pointer' } as CSSProperties}
          onClick={() => play(i)}
          role="button"
          tabIndex={0}
          onKeyDown={(e) => e.key === 'Enter' && play(i)}
        >
          <span className="lbl">{row.label}</span>
          <span className="track">
            <i key={plays[i]} />
          </span>
          <span className="ms">{row.ms}</span>
        </div>
      ))}
    </div>
  );
}

/* ------------------------------------------------------------ 空态纹样 */
export interface EmptyArtProps {
  className?: string;
  style?: CSSProperties;
}

/** 拼搭纹样：直角矩形 + 圆 + 三角 + 色条，按 8px 网格错落，面积 ≤6%。 */
export function EmptyArt({ className, style }: EmptyArtProps): ReactElement {
  return (
    <svg width={180} height={120} viewBox="0 0 180 120" className={className} style={style} aria-hidden="true">
      <Rect x={0} y={24} w={64} h={64} fill="var(--s2)" />
      <Circle cx={96} cy={56} r={24} fill="var(--accent)" />
      <Rect x={136} y={32} w={4} h={48} fill="var(--h7)" />
      <Triangle points="152,88 180,88 152,60" fill="var(--h3)" />
      <Rect x={64} y={96} w={24} h={8} fill="var(--s3)" />
    </svg>
  );
}