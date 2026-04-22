import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { readFileSync } from 'fs'

const semver = readFileSync('../release_version', 'utf-8').trim()

export default defineConfig({
  plugins: [react(), tailwindcss()],
  define: {
    __APP_VERSION__: JSON.stringify(semver),
  },
  server: {
    host: '127.0.0.1',
    proxy: {
      '/api': 'http://localhost:8080',
    },
  },
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: './src/test/setup.ts',
    css: false,
    coverage: {
      exclude: [
        'src/pages/Telemetry.tsx',
        'src/pages/DevPowerFlow.tsx',
        'src/test/**',
      ],
    },
  },
})
