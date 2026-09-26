/**
 * 真实传输层：通过 Tauri shell 插件拉起自带的 agent sidecar，走 NDJSON stdio。
 *
 * 协议与 mock 完全同 schema（events/schema.ts），一行一个 JSON 事件；
 * 与 C++ 侧 `agent.exe --serve` 的字段名一一对应（见 agent/src/app.cpp 的 runServe）。
 * 只在 Tauri 环境可用，浏览器预览请用 mock（App.tsx 里判定）。
 */
import { Command, type Child } from '@tauri-apps/plugin-shell';
import type { AgentCommand, AgentEvent, AgentTransport } from './schema';

/** sidecar 名：与 tauri.conf.json 的 bundle.externalBin 一致（实际文件带 target triple 后缀）。 */
const SIDECAR = 'binaries/agent';

/**
 * 工作区：agent 的相对路径（file_read / shell_exec / patch）都相对它解析，
 * settings.json 也是从这里读（main.cpp 用的是相对路径 "settings.json"）。
 * 不写死任何机器的绝对路径：取 Vite 环境变量（本地 .env.local，见 .env.example），
 * 没配就继承应用自身目录 —— 打包后把 settings.json 放在 exe 同级即可。
 */
const WORKSPACE = import.meta.env.VITE_EMBER_WORKSPACE as string | undefined;

export function createServeTransport(): AgentTransport {
  const handlers = new Set<(ev: AgentEvent) => void>();
  /** spawn 是异步的：早到的命令先排队，起来后一次性灌进去 */
  let queue: string[] = [];
  let child: Child | null = null;

  const emit = (ev: AgentEvent): void => handlers.forEach((h) => h(ev));

  /**
   * plugin-shell 的 stdout data 事件不保证正好是一行：一次可能给半条事件，
   * 也可能给好几条。自己按 \n 切并把残尾留在缓冲里，否则 JSON.parse 抛错会被
   * 静默丢掉 —— 表现就是握手后那批 sessions/workspaces 列表凭空不见。
   */
  let buf = '';
  const onData = (chunk: string): void => {
    buf += chunk;
    for (;;) {
      const nl = buf.indexOf('\n');
      if (nl < 0) break;
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (!line) continue;
      try {
        emit(JSON.parse(line) as AgentEvent);
      } catch {
        console.warn('[agent] 忽略了非协议输出:', line.slice(0, 120));
      }
    }
  };

  const start = async (): Promise<void> => {
    const cmd = WORKSPACE
      ? Command.sidecar(SIDECAR, ['--serve'], { cwd: WORKSPACE })
      : Command.sidecar(SIDECAR, ['--serve']);

    cmd.stdout.on('data', onData);
    cmd.stderr.on('data', (line: string) => console.warn('[agent]', line));
    cmd.on('error', (message: string) => emit({ type: 'error', message: `agent 启动失败：${message}` }));
    cmd.on('close', () => emit({ type: 'error', message: 'agent 进程已退出' }));

    try {
      child = await cmd.spawn();
    } catch (err) {
      emit({ type: 'error', message: `agent 启动失败：${String(err)}` });
      return;
    }
    for (const line of queue) void child.write(line);
    queue = [];
  };
  void start();

  // 关闭窗口或热重载时让 agent 收工：先发 shutdown，再 kill。
  // 只挂 beforeunload 不够 —— WebView2 里它不一定触发，漏掉的 sidecar 会一直
  // 活着并在退出时写出一个空会话文件（实测热重载一次就多一个孤儿进程）。
  let closed = false;
  const shutdown = (): void => {
    if (!child || closed) return;
    closed = true;
    void child.write(`${JSON.stringify({ type: 'shutdown' } satisfies AgentCommand)}\n`);
    void child.kill();
  };
  window.addEventListener('beforeunload', shutdown);
  window.addEventListener('pagehide', shutdown);

  return {
    send(cmd: AgentCommand): void {
      const line = `${JSON.stringify(cmd)}\n`;
      if (child) void child.write(line);
      else queue.push(line);
    },
    subscribe(handler: (ev: AgentEvent) => void): () => void {
      handlers.add(handler);
      return () => {
        handlers.delete(handler);
      };
    },
  };
}