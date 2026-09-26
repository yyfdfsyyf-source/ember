import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  // 5173 是沙箱/CI 都放行的端口，固定住避免漂移。
  server: { port: 5173, strictPort: true },
  build: { outDir: 'dist', sourcemap: true },
});