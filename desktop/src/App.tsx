/**
 * 外壳装配：四屏 + 主题 + 事件流。
 *
 * 传输层二选一：Tauri 里拉起 agent.exe --serve 的真实事件流；
 * 浏览器预览（以及 ?mock=1）用 events/mock.ts 的假后端。两者同 schema，
 * reducer 与组件一行不动。
 *
 * 界面上出现的每一个读数（模型 / 思考档 / 会话 / 工作区 / 版本 / 路径）都来自
 * 后端的 state 与列表事件——前端不再持有默认值，也不再抄一份静态清单。
 */
import { useCallback, useEffect, useMemo, useRef, useState, type ReactElement } from 'react';
import { ChatScreen } from './components/Chat';
import { Inspector, Rail, StatusBar, TopBar } from './components/Shell';
import { Palette } from './components/Palette';
import { createMockTransport } from './events/mock';
import { createServeTransport } from './events/serve';
import { useAgentStream, type ToolItem } from './events/useAgentStream';
import type { CommandInfo } from './events/schema';
import { EmptyScreen, Screen, SpecScreen, UsageScreen } from './screens';
import { isTauri } from './shell/host';
import type { ScreenId } from './types';

type Theme = 'dark' | 'light';

const SCREENS: ScreenId[] = ['chat', 'usage', 'empty', 'spec'];

function initialScreen(): ScreenId {
  const q = new URLSearchParams(window.location.search).get('screen');
  return SCREENS.find((s) => s === q) ?? 'chat';
}

/** 只有 Tauri 里才接真实后端；浏览器预览与非 Tauri 环境一律 mock。 */
function useMockTransport(): boolean {
  const forced = new URLSearchParams(window.location.search).get('mock') === '1';
  return forced || !isTauri();
}

function applyTheme(theme: Theme): void {
  const root = document.documentElement;
  if (theme === 'light') root.dataset.theme = 'light';
  else delete root.dataset.theme;
}

/** 模拟桌面：真机是 Mica/Acrylic 之后的真实桌面，这里只放中性直角矩形，不漏色相。 */
function Desk(): ReactElement {
  return (
    <svg className="desk" viewBox="0 0 1600 1000" preserveAspectRatio="xMidYMid slice" aria-hidden="true">
      <rect width="1600" height="1000" fill="var(--s0)" />
      <rect x="1080" y="120" width="520" height="360" fill="var(--s1)" />
      <rect x="0" y="640" width="640" height="360" fill="var(--s1)" />
    </svg>
  );
}

export function App(): ReactElement {
  const mock = useMockTransport();
  const [screen, setScreen] = useState<ScreenId>(initialScreen);
  const [palette, setPalette] = useState(false);
  const [draft, setDraft] = useState<{ text: string; n: number }>();
  const transport = useMemo(() => (mock ? createMockTransport() : createServeTransport()), [mock]);
  const { state, send, toggleTool, answer, refresh, set, command, newSession, useSession } = useAgentStream(transport);

  const tools = useMemo(() => state.items.filter((it): it is ToolItem => it.kind === 'tool'), [state.items]);

  // 握手后一次性把四张清单要过来：命令面板、模型/思考选择器、会话栏、工作区
  const listed = useRef(false);
  useEffect(() => {
    if (!state.connected || listed.current) return;
    listed.current = true;
    refresh('sessions');
    refresh('models');
    refresh('workspaces');
    refresh('commands');
  }, [state.connected, refresh]);

  // 主题的唯一真相是 <html data-theme>：CSS 只认它，React 就不再存一份会漂移的状态
  const toggleTheme = useCallback(() => {
    applyTheme(document.documentElement.dataset.theme === 'light' ? 'dark' : 'light');
  }, []);

  useEffect(() => {
    const onKey = (e: globalThis.KeyboardEvent): void => {
      if (e.ctrlKey && (e.key === 'p' || e.key === 'P')) {
        e.preventDefault();
        setPalette((v) => !v);
      } else if (e.key === 'Escape') {
        setPalette(false);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  const prompt = useCallback(
    (text: string) => {
      send({ type: 'prompt', text });
      setScreen('chat');
    },
    [send],
  );

  /** 命令面板选中：需要参数的回填输入框，其余直接下发（回执走 notice）。 */
  const runPalette = useCallback(
    (c: CommandInfo) => {
      setPalette(false);
      setScreen('chat');
      if (c.needsArg) {
        setDraft((d) => ({ text: `${c.name} `, n: (d?.n ?? 0) + 1 }));
        return;
      }
      command(c.name);
    },
    [command],
  );

  const pickModel = useCallback((name: string) => set('model', name), [set]);  const pickThinking = useCallback((level: string) => set('thinking', level), [set]);
  const pickWorkspace = useCallback((index: number) => set('workspace', String(index)), [set]);
  const toggleGuard = useCallback((on: boolean) => command(on ? '/ws on' : '/ws off'), [command]);
  /** 稳定引用：Palette 打开时用它要一次命令表，身份变了会重复请求 */
  const requestCommands = useCallback(() => refresh('commands'), [refresh]);

  const read = {
    model: state.model,
    thinking: state.thinking,
    thinkingBinary: state.thinkingBinary,
    mode: state.mode,
    connected: state.connected,
    mock,
  };

  return (
    <>
      {/* Tauri 里窗口是真透明的：透出来的是真实桌面 + 系统材质（Acrylic/Mica），
          只有网页预览才需要画这层模拟桌面 */}
      {isTauri() ? null : <Desk />}
      <div className="win">
        <TopBar
          screen={screen}
          onScreen={setScreen}
          onTheme={toggleTheme}
          onPalette={() => setPalette(true)}
          read={read}
          models={state.models}
          onModel={pickModel}
          onThinking={pickThinking}
        />

        <div className="body">
          {/* 当前会话的唯一真相是后端 state.sessionId，前端不再另存一份 */}
          <Rail
            sessions={state.sessions}
            activeId={state.sessionId}
            busy={state.busy || state.streaming}
            onSelect={useSession}
            onCreate={newSession}
            workspaces={state.workspaces}
            workspaceGuard={state.workspaceGuard}
            onUseWorkspace={pickWorkspace}
            onGuard={toggleGuard}
          />

          <section className="stage">
            <Screen id="chat" active={screen === 'chat'}>
              <ChatScreen
                items={state.items}
                streaming={state.streaming}
                onToggleTool={toggleTool}
                onSend={prompt}
                onCommand={command}
                draft={draft}
              />
            </Screen>
            <Screen id="usage" active={screen === 'usage'}>
              <UsageScreen usage={state.usage} tools={tools} sessions={state.sessions} />
            </Screen>
            <Screen id="empty" active={screen === 'empty'}>
              <EmptyScreen onQuick={prompt} model={state.model} workspace={state.workspace} />
            </Screen>
            <Screen id="spec" active={screen === 'spec'}>
              <SpecScreen active={screen === 'spec'} />
            </Screen>
          </section>

          <Inspector usage={state.usage} tools={tools} pendingAsk={state.pendingAsk} onAnswer={answer} />
        </div>

        <StatusBar
          read={{
            mode: state.mode,
            version: state.version,
            cwd: state.cwd,
            sessionTitle: state.sessionTitle,
            sessionId: state.sessionId,
            connected: state.connected,
          }}
          usage={state.usage}
        />
      </div>
      {palette ? (
        <Palette
          commands={state.commands}
          onRequest={requestCommands}
          onRun={runPalette}
          onClose={() => setPalette(false)}
        />
      ) : null}
    </>
  );
}
