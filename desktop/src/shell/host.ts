/**
 * 宿主环境判定与窗口控制。
 *
 * 浏览器里跑（vite dev 预览）时全部降级为空操作，组件不必关心自己在哪里。
 */
import { getCurrentWindow } from '@tauri-apps/api/window';

/** 是否跑在 Tauri 宿主里（v2 注入 __TAURI_INTERNALS__）。 */
export function isTauri(): boolean {
  return '__TAURI_INTERNALS__' in window;
}

export async function minimize(): Promise<void> {
  if (!isTauri()) return;
  await getCurrentWindow().minimize();
}

export async function toggleMaximize(): Promise<void> {
  if (!isTauri()) return;
  await getCurrentWindow().toggleMaximize();
}

export async function closeWindow(): Promise<void> {
  if (!isTauri()) return;
  await getCurrentWindow().close();
}