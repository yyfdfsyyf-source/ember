/**
 * 几何图元与图标库。
 *
 * 整套 UI 只有三种形状：直角矩形、圆、三角。图标一律由它们拼搭，
 * 不使用 emoji、图标字体或第三方图标库（见 FRONTEND_DESIGN.md §2）。
 * 颜色一律走 CSS 变量，保证深浅主题与交叉淡入生效。
 */
import type { ReactElement } from 'react';

export interface RectProps {
  x?: number;
  y?: number;
  w: number;
  h: number;
  fill: string;
}

/** 直角矩形（rx 恒为 0）。 */
export function Rect({ x = 0, y = 0, w, h, fill }: RectProps): ReactElement {
  return <rect x={x} y={y} width={w} height={h} fill={fill} />;
}

export interface CircleProps {
  cx: number;
  cy: number;
  r: number;
  fill?: string;
  stroke?: string;
  strokeWidth?: number;
}

/** 圆：只表示状态、进度、节点。 */
export function Circle({ cx, cy, r, fill, stroke, strokeWidth }: CircleProps): ReactElement {
  return <circle cx={cx} cy={cy} r={r} fill={fill ?? 'none'} stroke={stroke} strokeWidth={strokeWidth} />;
}

export interface TriangleProps {
  points: string;
  fill: string;
}

/** 三角：只表示方向与生成。 */
export function Triangle({ points, fill }: TriangleProps): ReactElement {
  return <polygon points={points} fill={fill} />;
}

/* ------------------------------------------------------------------ 图标
   每个图标都是「图元拼搭」，尺寸取 8/12/16/20；没有任何描边或圆角。 */

interface IconProps {
  size?: number;
}

/** 对话（矩形 + 尾巴三角） */
export function IconChat({ size = 16 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden="true">
      <Rect w={16} h={10} fill="currentColor" />
      <Triangle points="3,10 8,10 3,15" fill="currentColor" />
    </svg>
  );
}

/** 用量（三根直角柱） */
export function IconUsage({ size = 16 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden="true">
      <Rect x={0} y={9} w={4} h={7} fill="currentColor" />
      <Rect x={6} y={5} w={4} h={11} fill="currentColor" />
      <Rect x={12} y={0} w={4} h={16} fill="currentColor" />
    </svg>
  );
}

/** 空态（方块 + 角上三角） */
export function IconEmpty({ size = 16 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden="true">
      <Rect w={10} h={10} fill="currentColor" />
      <Triangle points="16,16 16,7 7,16" fill="currentColor" />
    </svg>
  );
}

/** 规范（三图元并置） */
export function IconSpec({ size = 16 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden="true">
      <Rect w={7} h={7} fill="currentColor" />
      <Circle cx={12} cy={12} r={4} fill="currentColor" />
      <Triangle points="0,16 7,16 0,9" fill="currentColor" />
    </svg>
  );
}

/** 主题切换（半强调半中性方块） */
export function IconTheme({ size = 16 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden="true">
      <Rect x={0} y={1} w={7} h={14} fill="var(--accent)" />
      <Rect x={8} y={1} w={7} h={14} fill="var(--text-2)" />
    </svg>
  );
}

/** 新建（直角十字） */
export function IconPlus({ size = 14 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 14 14" aria-hidden="true">
      <Rect y={6} w={14} h={2} fill="currentColor" />
      <Rect x={6} w={2} h={14} fill="currentColor" />
    </svg>
  );
}

/** 发送（方形底 + 等边三角） */
export function IconSend({ size = 18 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 18 18" aria-hidden="true">
      <Triangle points="3,2 16,9 3,16" fill="currentColor" />
    </svg>
  );
}

/* 窗口控制：尺寸层级即语义（对角线图标在本语言里不合法） */
export function IconWinMin(): ReactElement {
  return (
    <svg width={16} height={16} viewBox="0 0 16 16" aria-hidden="true">
      <Rect x={3} y={11} w={10} h={2} fill="currentColor" />
    </svg>
  );
}

export function IconWinMax(): ReactElement {
  return (
    <svg width={16} height={16} viewBox="0 0 16 16" aria-hidden="true">
      <Rect x={4} y={4} w={8} h={8} fill="currentColor" />
    </svg>
  );
}

export function IconWinClose(): ReactElement {
  return (
    <svg width={16} height={16} viewBox="0 0 16 16" aria-hidden="true">
      <Rect x={3} y={3} w={10} h={10} fill="currentColor" />
    </svg>
  );
}

/** 品牌图元（字标待定，先用「矩形 + 色条 + 圆」占位） */
export function BrandMark({ size = 16 }: IconProps): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden="true">
      <Rect w={16} h={16} fill="var(--s3)" />
      <Rect y={12} w={16} h={4} fill="var(--accent)" />
      <Circle cx={11} cy={5} r={3} fill="var(--text-2)" />
    </svg>
  );
}

/** 拼搭示例（规范页画廊，见 §2.3） */
export function GalleryFile(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Rect x={4} y={2} w={20} h={28} fill="var(--h7)" />
      <Triangle points="24,2 24,10 16,2" fill="var(--s2)" />
    </svg>
  );
}

export function GallerySearch(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Circle cx={13} cy={13} r={9} fill="var(--h8)" />
      <Triangle points="19,19 28,28 19,28" fill="var(--text-2)" />
    </svg>
  );
}

export function GalleryTerminal(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Rect w={32} h={24} fill="var(--s3)" />
      <Rect x={6} y={14} w={12} h={4} fill="var(--accent)" />
      <Rect x={22} y={6} w={4} h={14} fill="var(--text-2)" />
    </svg>
  );
}

export function GallerySend(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Rect w={32} h={32} fill="var(--accent)" />
      <Triangle points="9,7 26,16 9,25" fill="var(--on-color)" />
    </svg>
  );
}

export function GalleryFold(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Rect w={32} h={32} fill="var(--s2)" />
      <Triangle points="8,12 24,12 16,22" fill="var(--text-1)" />
    </svg>
  );
}

export function GalleryRunning(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Circle cx={16} cy={16} r={10} fill="var(--accent)" />
      <Circle cx={16} cy={16} r={6} fill="var(--s2)" />
    </svg>
  );
}

export function GalleryProgress(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Circle cx={16} cy={16} r={10} fill="none" stroke="var(--h3)" strokeWidth={4} />
      <Circle cx={16} cy={4} r={3} fill="var(--h3)" />
    </svg>
  );
}

export function GalleryWorkspace(): ReactElement {
  return (
    <svg width={32} height={32} viewBox="0 0 32 32" aria-hidden="true">
      <Rect w={4} h={32} fill="var(--h6)" />
      <Circle cx={18} cy={16} r={8} fill="var(--h6)" />
    </svg>
  );
}