import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

// 沙箱里的预览是**代理**出来的：浏览器访问 https://<port>-<id>.e2b.app，
// 由平台把请求转给 0.0.0.0:<port>。两件事必须显式配：
//   1) host 必须是 0.0.0.0（绑 127.0.0.1 的话代理连不进来）；
//   2) allowedHosts 必须放开（Vite 5.4.12+ 默认只认 localhost，
//      否则回一句 "Blocked request. This host is not allowed." —— 预览是白屏）。
// HMR 走同一条 https 代理，所以 websocket 要显式指到 443/wss。
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  build: { target: 'es2019', assetsInlineLimit: 100000000, cssCodeSplit: false },
  server: {
    host: '0.0.0.0',
    port: 8099,
    strictPort: true,          // 端口被占就直接报错，别悄悄换到 8100
    allowedHosts: true,
    hmr: { protocol: 'wss', clientPort: 443 },
  },
  preview: {
    host: '0.0.0.0',
    port: 8099,
    strictPort: true,
    allowedHosts: true,
  },
})
