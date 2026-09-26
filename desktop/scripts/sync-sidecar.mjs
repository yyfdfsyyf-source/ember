/**
 * 把 C++ 端构建出的可执行文件同步成 Tauri sidecar 需要的名字：
 *   out/agent.exe  →  src-tauri/binaries/agent-<target-triple>.exe
 *
 * Tauri 只用 target triple 后缀区分平台，配置里写的是 "binaries/agent"。
 * 所以顺序是：仓库根目录先跑 .\build.ps1，再跑 tauri dev / tauri build。
 * （tauri dev/build 的 beforeDevCommand / beforeBuildCommand 已经带上了本步骤。）
 */
import { execFileSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const desktop = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repo = resolve(desktop, '..');

/** 本机目标三元组：rustc 的 host 就是它；交叉编译可用 TAURI_TARGET_TRIPLE 覆盖。 */
function targetTriple() {
  if (process.env.TAURI_TARGET_TRIPLE) return process.env.TAURI_TARGET_TRIPLE;
  const raw = execFileSync('rustc', ['-vV'], { encoding: 'utf8' });
  const host = /host:\s*(\S+)/.exec(raw);
  if (!host) throw new Error('无法从 rustc -vV 解析 host');
  return host[1];
}

const triple = targetTriple();
const ext = triple.includes('windows') ? '.exe' : '';
const source = join(repo, 'out', `agent${ext}`);
const destDir = join(desktop, 'src-tauri', 'binaries');
const dest = join(destDir, `agent-${triple}${ext}`);

if (!existsSync(source)) {
  throw new Error(`缺少 ${source}：先在仓库根目录跑 .\\build.ps1`);
}
if (existsSync(dest) && statSync(dest).mtimeMs >= statSync(source).mtimeMs) {
  console.log(`sidecar: 已是最新 ${dest}`);
} else {
  mkdirSync(destDir, { recursive: true });
  copyFileSync(source, dest);
  console.log(`sidecar: ${source} → ${dest} (${(statSync(dest).size / 1e6).toFixed(1)} MB)`);
}