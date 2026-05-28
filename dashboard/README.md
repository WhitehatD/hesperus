# Dashboard

Next.js (App Router) operator console for the Autonomous IoT Visual Monitoring system. It renders the board list, a live per-board console with the natural-language **agent chat** (streamed over SSE), and real-time board status over MQTT-WebSocket. The agent reasoning, tool calls, and image analysis are all served by the FastAPI backend.

**Stack:** Next.js 16 · React 19 · TypeScript 5 · Tailwind CSS

## Structure

| Path | Purpose |
|---|---|
| `app/page.tsx` | Board list / landing |
| `app/board/[id]/page.tsx` | Live board console (capture feed, schedules, agent chat) |
| `components/AgentChat.tsx` | Streams the agent's thinking + tool calls via Server-Sent Events |
| `lib/useMQTT.ts` | Subscribes to live board status / heartbeats over MQTT-WebSocket |

## Configuration

Set via environment (e.g. `.env.local` for dev, CI/compose for prod):

| Variable | Purpose |
|---|---|
| `NEXT_PUBLIC_API_URL` | FastAPI backend base URL (browser-side calls) |
| `BACKEND_URL` | FastAPI backend base URL (server-side / route handlers) |
| `NEXT_PUBLIC_MQTT_WS_URL` | MQTT broker WebSocket endpoint for live status |

## Development

```bash
npm install
npm run dev      # http://localhost:3000
npm run build    # production build
npm run lint
```

## Deployment

Built into a Docker image and deployed to the VPS by CI on push to `main` (`Dockerfile` + `.github/workflows/ci.yml`) — **not** Vercel. See the [root README](../README.md) for the full system architecture and deployment pipeline.
