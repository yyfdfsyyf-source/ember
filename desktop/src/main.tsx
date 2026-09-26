/**
 * 入口。URL 参数在渲染前一次性落到 <html> 上：
 *   ?theme=light  分享浅色稿
 *   ?static=1     去掉交错延迟，便于逐屏像素对照
 *   ?screen=usage 直接打开某一屏
 * 不用 StrictMode：mock 传输层会被双跑，事件会重复入列。
 */
import { createRoot } from 'react-dom/client';
import { App } from './App';
import './styles/tokens.css';
import './styles/app.css';

const params = new URLSearchParams(window.location.search);
if (params.get('theme') === 'light') document.documentElement.dataset.theme = 'light';
if (params.get('static') === '1') document.documentElement.dataset.static = '';
// Tauri 宿主里窗口是透明的：页面的 html/body 必须让出底色（见 app.css 末尾）
if ('__TAURI_INTERNALS__' in window) document.documentElement.dataset.shell = 'tauri';

const host = document.getElementById('root');
if (!host) throw new Error('#root 不存在：检查 index.html');

createRoot(host).render(<App />);