# airies-ups frontend

React + TypeScript + Vite + Tailwind. Web UI for the airies-ups daemon.

The bundle is **compiled into the daemon binary at build time** — `make embed-frontend` in the repo root gzips and brotli-compresses each `dist/` file into a C byte-array source the daemon links in. There is no separate static-file deploy path.

## Layout

```
src/
  pages/        one .tsx per route, with a co-located .test.tsx
  components/   shared UI (Layout, Modal, PowerFlow, Toast, Field)
  hooks/        useApi (polling fetch), useTheme
  utils/        small pure helpers
  test/setup.ts vitest setup (jsdom + testing-library)
```

Routes are registered in `App.tsx`; the sidebar in `components/Layout.tsx` is the canonical nav.

## Dev server

The Vite dev server (`bun run dev`) listens on `http://localhost:5173` and proxies `/api/*` to `http://localhost:8080` (configured in `vite.config.ts`). Hot-reload is on for everything under `src/`.

You need a daemon running locally for the proxy to have a target. From the repo root:

```bash
make debug                              # native debug build, skips frontend embed
./build/airies-upsd                     # run the daemon (binds :8080 by default)
```

Then in this directory:

```bash
bun install
bun run dev
```

Open `http://localhost:5173`.

## Scripts

| Command | What it does |
|---------|--------------|
| `bun run dev` | Vite dev server with HMR. |
| `bun run build` | Type-check (`tsc -b`) + production bundle into `dist/`. The daemon's `make embed-frontend` step picks it up from there. |
| `bun run lint` | ESLint over `src/`. |
| `bun run test` | Vitest, single run. |
| `bun run test:watch` | Vitest in watch mode. |
| `bun run test:coverage` | Vitest with istanbul coverage; reports to `coverage/`. |
| `bun run test:junit` | JUnit-format test report at `test-results.junit.xml`. CI uses this for the MR Tests tab. |

### Coverage provider

We use `@vitest/coverage-istanbul`, not the v8 default. Vitest's v8 provider uses Node's inspector API, which Bun does not expose — `bun run vitest --coverage` with the v8 provider fails with "Coverage APIs are not supported". Istanbul does source-level instrumentation and works fine under Bun.

The CI gate is configured at the pipeline level (see `.gitlab-ci.yml` `coverage:frontend`); local runs do not gate.

## Embedding into the daemon

The Makefile's `embed-frontend` target gzips and brotli-compresses each file under `dist/` and emits `build/embedded_assets.c` containing C byte arrays. The HTTP server does content negotiation at request time and returns the pre-compressed variant the client accepts. There is zero runtime compression cost.

Practical consequence: a frontend-only change is not visible on a deployed Pi until you re-deploy the daemon binary. Re-running `bun run build` is not enough.
