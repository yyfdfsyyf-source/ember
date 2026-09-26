/**
 * 屏：用量 / 空态 / 规范自检（对话屏在 components/Chat.tsx）。
 *
 * .screen 常驻挂载、只切 data-active —— 这样切屏会重播交错入场动画（与原型一致）。
 * 注意：React 里必须"有则给、无则不给"地写 data-active，写 data-active="false"
 * 会被 [data-active] 选择器命中（原型踩过这个坑）。
 */
import type { ReactElement, ReactNode } from 'react';
import {
  GalleryFile,
  GalleryFold,
  GalleryProgress,
  GalleryRunning,
  GallerySearch,
  GallerySend,
  GalleryTerminal,
  GalleryWorkspace,
  Rect,
  Triangle,
} from './geo/Geo';
import { BarChart, EmptyArt, FamBars, MotionDemo, Ring, Swatches, type BarPoint, type FamRow, type SwatchItem } from './components/viz';
import { baseName, rise } from './components/Shell';
import { FAMILY_LABEL, toolFamily, type SessionInfo, type ToolFamily } from './events/schema';
import type { ToolItem, UsageState } from './events/useAgentStream';
import type { ScreenId } from './types';

/* ------------------------------------------------------------- 屏容器 */
export interface ScreenProps {
  id: ScreenId;
  active: boolean;
  children: ReactNode;
}

export function Screen({ id, active, children }: ScreenProps): ReactElement {
  return (
    <div className="screen" data-screen={id} {...(active ? { 'data-active': 'true' } : {})}>
      {children}
    </div>
  );
}

/* --------------------------------------------------------------- 用量 */
/** 工具族 → 色相：固定映射，见设计系统 §3.5（other 归到 H2 思考/MCP 色）。 */
const FAMILY_COLOR: Record<ToolFamily, string> = {
  file: 'var(--h7)',
  exec: 'var(--h6)',
  search: 'var(--h8)',
  other: 'var(--h2)',
};

/** 本会话的工具调用按族聚合：柱宽相对最满的一族。 */
function familyRows(tools: ToolItem[]): FamRow[] {
  const count = new Map<ToolFamily, number>();
  for (const t of tools) {
    const f = toolFamily(t.name).family;
    count.set(f, (count.get(f) ?? 0) + 1);
  }
  const max = Math.max(1, ...Array.from(count.values()));
  return (Object.keys(FAMILY_LABEL) as ToolFamily[]).map((f) => ({
    label: FAMILY_LABEL[f],
    pct: Math.round(((count.get(f) ?? 0) / max) * 100),
    value: String(count.get(f) ?? 0),
    color: FAMILY_COLOR[f],
  }));
}

const WEEKDAY = ['周日', '周一', '周二', '周三', '周四', '周五', '周六'];
const DAY_MS = 86_400_000;

/** 近 7 天会话活动：把 sessions 事件里的真实 savedAt 按日落桶，值为消息条数。 */
function weekActivity(sessions: SessionInfo[]): BarPoint[] {
  const midnight = new Date();
  midnight.setHours(0, 0, 0, 0);
  const todayAt = midnight.getTime();
  const buckets: BarPoint[] = Array.from({ length: 7 }, (_, i) => {
    const d = new Date(todayAt - (6 - i) * DAY_MS);
    return { label: WEEKDAY[d.getDay()], value: 0 };
  });
  for (const s of sessions) {
    const at = new Date(s.savedAt * 1000);
    at.setHours(0, 0, 0, 0);
    const back = Math.round((todayAt - at.getTime()) / DAY_MS);
    const idx = 6 - back;
    if (idx >= 0 && idx < 7) buckets[idx].value += s.msgCount;
  }
  return buckets;
}

/** 拆出数值与单位：1.24 / M（KPI 里单位要单独小字号）。 */
function splitTokens(n: number): [string, string] {
  if (n >= 1e6) return [(n / 1e6).toFixed(2), 'M'];
  if (n >= 1e3) return [(n / 1e3).toFixed(1), 'k'];
  return [String(n), ''];
}

export interface UsageScreenProps {
  usage: UsageState;
  tools: ToolItem[];
  sessions: SessionInfo[];
}

export function UsageScreen({ usage, tools, sessions }: UsageScreenProps): ReactElement {
  // 全部口径来自 agent 上报：没有数据就显示 —，不造演示值
  const cachePct = usage.promptTokens > 0 ? Math.round((usage.cachedTokens / usage.promptTokens) * 100) : null;
  const tokens = usage.promptTokens + usage.completionTokens;
  const [tokenValue, tokenUnit] = splitTokens(tokens);
  const cost = usage.costMicroUsd / 1e6;
  const budgetTotal = usage.costBudgetMicroUsd / 1e6;
  const budgetPct = usage.costBudgetMicroUsd > 0 ? (usage.costMicroUsd / usage.costBudgetMicroUsd) * 100 : 0;
  const week = weekActivity(sessions);
  const activeDays = week.filter((d) => d.value > 0).length;

  return (
    <>
      <div className="col">
        <div className="kpis rise" style={rise(0)}>
          <div className="kpi">
            <div className="k">本会话 token</div>
            <div className="v">
              {tokens > 0 ? (
                <>
                  {tokenValue}
                  <span className="u">{tokenUnit}</span>
                </>
              ) : (
                '—'
              )}
            </div>
          </div>
          <div className="kpi secondary">
            <div className="k">成本</div>
            <div className="v">{usage.costMicroUsd > 0 ? `$${cost.toFixed(3)}` : '—'}</div>
          </div>
          <div className="kpi secondary">
            <div className="k">缓存命中</div>
            <div className="v">
              {cachePct === null ? '—' : cachePct}
              {cachePct === null ? null : <span className="u">%</span>}
            </div>
          </div>
        </div>

        <div className="card rise" style={rise(1)}>
          <h4>近 7 天会话活动</h4>
          <BarChart data={week} label="近 7 天每天的会话消息条数" />
          <div className="legend">
            <span>
              <i style={{ background: 'var(--accent)' }} />
              今天
            </span>
            <span>
              <i style={{ background: 'var(--data-2)' }} />
              往日
            </span>
            <span>
              <i style={{ background: 'var(--h2)' }} />
              趋势
            </span>
          </div>
          <div className="hints">
            {sessions.length === 0
              ? '后端还没有回会话列表（sessions 事件）'
              : `${sessions.length} 个会话 · ${activeDays} 天有活动 · 柱高为该日消息条数`}
          </div>
        </div>

        <div className="card rise" style={rise(2)}>
          <h4>本会话工具族分布</h4>
          <FamBars rows={familyRows(tools)} />
          <div className="hints">
            {tools.length === 0 ? '本轮还没有工具调用' : `${tools.length} 次调用，按 §3.5 的固定色相分族`}
          </div>
        </div>
      </div>

      {/* 预算卡在 .col 之外（原型如此）：全宽，不跟内容列一起收窄。
          预算与已花都来自后端（costBudgetMicroUsd / costMicroUsd）。 */}
      <div className="card rise" style={rise(3)}>
        <h4>预算</h4>
        <div className="ring-wrap">
          <Ring
            size={88}
            r={36}
            pct={budgetPct}
            label={usage.costBudgetMicroUsd > 0 ? `${Math.round(budgetPct)}%` : '—'}
            sub={usage.costBudgetMicroUsd > 0 ? `$${cost.toFixed(2)} / $${budgetTotal}` : '未设预算'}
          />
          <div>
            <p style={{ margin: '0 0 8px' }}>
              {usage.costBudgetMicroUsd > 0 ? (
                <>
                  本会话成本占预算 <b style={{ color: budgetPct >= 80 ? 'var(--h4)' : 'var(--text-1)' }}>{Math.round(budgetPct)}%</b>
                  {budgetPct >= 80 ? '，已接近告警线。' : '，未触发告警。'}
                </>
              ) : (
                'settings.json 里设 cost_budget_usd 后这里会画预算环。'
              )}
            </p>
            <p style={{ margin: 0, color: 'var(--text-2)', fontSize: 12, lineHeight: '18px' }}>
              达到 80% 时状态条会出现 H4 等待色提示。
            </p>
          </div>
        </div>
      </div>
    </>
  );
}

/* --------------------------------------------------------------- 空态 */
const QUICK_ACTIONS: Array<{ text: string; icon: ReactElement }> = [
  {
    text: '解释这个仓库的结构',
    icon: (
      <svg width={14} height={14} viewBox="0 0 14 14" aria-hidden="true">
        <Rect w={14} h={14} fill="var(--h7)" />
        <Triangle points="14,0 14,5 9,0" fill="var(--s2)" />
      </svg>
    ),
  },
  {
    text: '跑测试并修失败项',
    icon: (
      <svg width={14} height={14} viewBox="0 0 14 14" aria-hidden="true">
        <Rect w={14} h={14} fill="var(--h6)" />
        <Rect x={3} y={6} w={8} h={2} fill="var(--s1)" />
      </svg>
    ),
  },
  {
    text: '阅读 AGENTS.md 的约定',
    icon: (
      <svg width={14} height={14} viewBox="0 0 14 14" aria-hidden="true">
        <circle cx={7} cy={7} r={7} fill="var(--h8)" />
        <Triangle points="5,4 11,7 5,10" fill="var(--s1)" />
      </svg>
    ),
  },
];

export interface EmptyScreenProps {
  onQuick: (text: string) => void;
  /** 顶栏同一个来源（state 事件），这里只是复述，不再写死模型名 */
  model: string;
  workspace: string;
}

export function EmptyScreen({ onQuick, model, workspace }: EmptyScreenProps): ReactElement {
  return (
    <div className="empty">
      <EmptyArt className="rise" style={rise(0)} />
      <h2 className="rise" style={rise(1)}>
        开始一段对话
      </h2>
      <p className="rise" style={rise(2)}>
        工作区 <span className="mono">{workspace ? baseName(workspace) : '未登记'}</span> · 模型{' '}
        <span className="mono">{model || '未连接'}</span>
      </p>
      <div className="quick rise" style={rise(3)}>
        {QUICK_ACTIONS.map((q) => (
          <a
            key={q.text}
            href="#"
            onClick={(e) => {
              e.preventDefault();
              onQuick(q.text);
            }}
          >
            {q.icon}
            {q.text}
          </a>
        ))}
      </div>
    </div>
  );
}

/* ----------------------------------------------------------- 规范自检 */
const SURFACES: SwatchItem[] = [
  { color: 'var(--s0)', name: '--s0 主区底', hint: '深色 L*4.32 / 浅色 88.83' },
  { color: 'var(--s1)', name: '--s1 面板', hint: '深色 L*9.16 / 浅色 92.40' },
  { color: 'var(--s2)', name: '--s2 内容块', hint: '深色 L*14.08 / 浅色 96.16' },
  { color: 'var(--s3)', name: '--s3 内嵌面', hint: '深色 L*19.69 / 浅色 100' },
];

const HUES: SwatchItem[] = [
  { color: 'var(--accent)', name: 'H1 ember', hint: '主操作 / 运行中 / 焦点' },
  { color: 'var(--h2)', name: 'H2 indigo', hint: '思考 / 外部数据 / MCP' },
  { color: 'var(--h3)', name: 'H3 teal', hint: '成功 / 完成' },
  { color: 'var(--h4)', name: 'H4 amber', hint: '等待批准 / 预算告警' },
  { color: 'var(--h5)', name: 'H5 red', hint: '失败 / 错误' },
  { color: 'var(--h6)', name: 'H6 magenta', hint: '工具族 · 执行类' },
  { color: 'var(--h7)', name: 'H7 violet', hint: '工具族 · 文件类' },
  { color: 'var(--h8)', name: 'H8 lime', hint: '工具族 · 检索类' },
];

const GALLERY: Array<{ icon: ReactElement; label: string }> = [
  { icon: <GalleryFile />, label: '文件 = 矩形减三角' },
  { icon: <GallerySearch />, label: '搜索 = 圆 + 三角' },
  { icon: <GalleryTerminal />, label: '终端 = 矩形 + 直角块' },
  { icon: <GallerySend />, label: '发送 = 方形 + 三角' },
  { icon: <GalleryFold />, label: '折叠 = 方形 + 三角' },
  { icon: <GalleryRunning />, label: '运行 = 同心圆' },
  { icon: <GalleryProgress />, label: '进度 = 圆环 + 圆点' },
  { icon: <GalleryWorkspace />, label: '工作区 = 色条 + 圆' },
];

export function SpecScreen({ active }: { active: boolean }): ReactElement {
  return (
    <div className="col">
      <div className="card rise" style={rise(0)}>
        <h4>面层阶梯（层级只靠明度差，无描边）</h4>
        <Swatches items={SURFACES} />
      </div>

      <div className="card rise" style={rise(1)}>
        <h4>色相板（当前主题取值 · 语义固定）</h4>
        <Swatches items={HUES} />
      </div>

      <div className="card rise" style={rise(2)}>
        <h4>三种图元与拼搭（每条边界不是直线段就是圆弧）</h4>
        <div className="gallery">
          {GALLERY.map((g) => (
            <div className="gitem" key={g.label}>
              {g.icon}
              <span>{g.label}</span>
            </div>
          ))}
        </div>
      </div>

      <div className="card rise" style={rise(3)}>
        <h4>动效档位（点击播放）</h4>
        <MotionDemo active={active} />
        <div className="hints">只动 transform / opacity / stroke-dashoffset；无弹跳、无带位移的循环动画。</div>
      </div>
    </div>
  );
}