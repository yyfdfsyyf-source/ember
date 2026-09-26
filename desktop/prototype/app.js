// Ember Desktop · 静态原型交互
// 只做四件事：主题变量级交叉淡入、屏切换与入场重放、演示性展开/流式文本、动效档位演示。
// 规范依据：/FRONTEND_DESIGN.md v2 第 7 节（时长档与曲线）。

(() => {
  const root = document.documentElement;
  const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* ---------------------------------------------------------- 主题（3.7）
     变量级过渡：tokens.css 里注册了 @property 并在 :root 上做了 transition，
     所以这里只切 data-theme，尺寸与位置完全不动。 */
  const themeBtn = document.getElementById('themeBtn');
  themeBtn.addEventListener('click', () => {
    if (root.dataset.theme === 'light') {
      delete root.dataset.theme;
    } else {
      root.dataset.theme = 'light';
    }
  });

  /* ---------------------------------------------------------- 屏切换
     切 data-active 会让 CSS 入场动画重新播放（staggered reveal）。 */
  const screens = Array.from(document.querySelectorAll('.screen'));
  const screenBtns = Array.from(document.querySelectorAll('[data-screen-btn]'));

  function show(name) {
    screens.forEach((s) => s.toggleAttribute('data-active', s.dataset.screen === name));
    screenBtns.forEach((b) => b.setAttribute('aria-current', String(b.dataset.screenBtn === name)));
    if (name === 'spec') playMotionDemo();
  }

  screenBtns.forEach((b) => b.addEventListener('click', () => show(b.dataset.screenBtn)));

  /* ---------------------------------------------------------- 工具卡展开
     150ms 高度过渡（规范允许的「展开」动效：单元素、同列内，无兄弟重排）。 */
  document.querySelectorAll('.tool').forEach((card) => {
    const body = card.querySelector('.tool-body');
    const head = card.querySelector('.tool-head');
    if (!body || !head) return;
    body.style.height = card.dataset.open === 'true' ? 'auto' : '0px';

    head.addEventListener('click', () => {
      const open = card.dataset.open === 'true';
      if (open) {
        body.style.height = body.scrollHeight + 'px';
        requestAnimationFrame(() => { body.style.height = '0px'; });
        card.dataset.open = 'false';
      } else {
        card.dataset.open = 'true';
        body.style.height = '0px';
        requestAnimationFrame(() => { body.style.height = body.scrollHeight + 'px'; });
      }
    });

    body.addEventListener('transitionend', (e) => {
      if (e.propertyName !== 'height') return;
      // 展开完成后交回内容自身高度，后续内容变化不会再被裁切。
      if (card.dataset.open === 'true') body.style.height = 'auto';
    });
  });

  /* ---------------------------------------------------------- 流式文本
     16ms 节流逐字；不做淡入逐字（避免抖动）。reduced-motion 下直接落全量文本。 */
  const target = document.querySelector('[data-stream]');
  if (target) {
    const text = target.getAttribute('data-stream');
    const line = target.closest('p');
    if (reduced) {
      target.textContent = text;
      line?.classList.remove('streaming');
    } else {
      let i = 0;
      const tick = setInterval(() => {
        target.textContent = text.slice(0, ++i);
        if (i >= text.length) {
          clearInterval(tick);
          line?.classList.remove('streaming');
        }
      }, 16);
    }
  }

  /* ---------------------------------------------------------- 动效档位演示 */
  const rows = Array.from(document.querySelectorAll('#motionDemo .motion-row'));

  function playMotionDemo() {
    rows.forEach((row, idx) => {
      const token = getComputedStyle(root).getPropertyValue(row.dataset.dur).trim();
      row.style.setProperty('--dur', token);
      const bar = row.querySelector('.track i');
      setTimeout(() => {
        bar.style.animation = 'none';
        void bar.offsetWidth;           // 强制重排以重启动画
        bar.style.animation = '';
        row.classList.add('play');
      }, idx * 120);
    });
  }

  rows.forEach((row) => {
    row.style.cursor = 'pointer';
    row.addEventListener('click', () => {
      const bar = row.querySelector('.track i');
      bar.style.animation = 'none';
      void bar.offsetWidth;
      bar.style.animation = '';
      row.classList.add('play');
    });
    row.addEventListener('animationend', () => row.classList.remove('play'));
  });

  /* ---------------------------------------------------------- 状态条尺寸
     右下角显示真实视口，不写死（Tauri 里换成窗口逻辑尺寸即可）。 */
  const dimTag = document.getElementById('dimTag');
  const paintDim = () => { if (dimTag) dimTag.textContent = `${innerWidth}x${innerHeight}`; };
  paintDim();
  addEventListener('resize', paintDim);

  /* ---------------------------------------------------------- URL 参数
     ?screen=usage|empty|spec&theme=light —— 静态稿可直接分享某个状态，
     也方便逐屏截图比对。放在最后执行，避免 show() 触发尚未初始化的状态。 */
  const q = new URLSearchParams(location.search);
  if (q.get('theme') === 'light') root.dataset.theme = 'light';
  if (q.get('static') === '1') root.dataset.static = '';
  if (screens.some((s) => s.dataset.screen === q.get('screen'))) show(q.get('screen'));
})();