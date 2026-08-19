# Hesperus

**Autonomous IoT Visual Monitoring — natural language in, autonomous visual intelligence out.**

A complete edge-to-cloud system where you tell an AI agent *what to monitor* in plain English, and a physical STM32 camera board autonomously captures, uploads, and analyzes images using multimodal LLMs — no camera feeds, no manual triggers, no human in the loop.

> Named for the evening star — the light that appears at dusk, when the watch begins. The
> system's core behavior isn't continuous watching, it's a device that wakes on schedule,
> captures once, and returns to dormancy: the *duty cycle*, not the *stare*, is the point
> (see [Key Findings](#key-findings) below — a 100–252× energy reduction depends on it).

> Bachelor Thesis — Design and Implementation of an Autonomous IoT Visual Monitoring System with Cloud-Based AI Planning and Analysis
>
> Alexandru-Ionut Cioc | Supervisors: Prof. Tang & Prof. Kouzinopoulos | 2026

> **The headline result:** a self-hosted 30B vision model matched the best cloud model's quality within statistical noise at five to ten times the speed (0.77s vs 3.6 to 7.8s per call). Write-up: [ciocandco.com/writing/self-hosted-vs-cloud-vlm-benchmark](https://ciocandco.com/writing/self-hosted-vs-cloud-vlm-benchmark) | full findings: [results/findings_v3.md](results/findings_v3.md)

<p align="center">
  <img src="https://img.shields.io/badge/STM32U585AI-Cortex--M33-03234B?style=flat-square&logo=stmicroelectronics&logoColor=white" />
  <img src="https://img.shields.io/badge/FastAPI-Python_3.11-009688?style=flat-square&logo=fastapi&logoColor=white" />
  <img src="https://img.shields.io/badge/Next.js_16-React_19-000?style=flat-square&logo=next.js" />
  <img src="https://img.shields.io/badge/MQTT-Mosquitto-660066?style=flat-square&logo=eclipsemosquitto&logoColor=white" />
  <img src="https://img.shields.io/badge/OTA-Dual_Bank_Flash-FF6F00?style=flat-square" />
  <img src="https://img.shields.io/badge/CI%2FCD-GitHub_Actions-2088FF?style=flat-square&logo=githubactions&logoColor=white" />
</p>

---

## System Overview

The project spans five layers, from register-level hardware drivers to a cloud AI pipeline:

| Layer | Technology | Lines of Code | What It Does |
|-------|-----------|---------------|--------------|
| **Firmware** | Bare-metal C, ARM Cortex-M33 | ~8,000 | OV5640 camera driver with register-level AEC tuning, on-board JPEG encoder, hand-rolled MQTT 3.1.1 client, dual-bank OTA, DCMI/DMA capture, resumable chunked upload, adaptive STOP2 sleep with wake-on-ping, hardware watchdog |
| **Backend** | FastAPI, SQLAlchemy async, Python | ~5,700 | AI planning engine, multimodal LLM orchestration (OpenRouter primary, Claude/Gemini for benchmarking), MQTT broker bridge, resumable chunked-upload endpoints, SSE streaming |
| **Frontend** | Next.js 16, React 19, TypeScript | ~3,000 | Real-time MQTT WebSocket dashboard, agentic chat interface, live board console, image gallery with AI analysis overlay |
| **Infrastructure** | Docker Compose, GitHub Actions, Nginx | ~500 | Automated CI/CD, cross-compilation, OTA binary delivery, VPS deployment, container orchestration |
| **Hardware** | STM32 B-U585I-IOT02A Discovery Kit | — | 160MHz Cortex-M33, 768KB SRAM, OV5640 5MP camera, EMW3080 WiFi (SPI), dual-bank 2MB flash |

A `git push` compiles ARM firmware, runs 74 backend tests, builds Docker images, deploys to a VPS, and delivers a firmware update over-the-air to the board.

---

## Key Findings

The working system doubles as an empirical study (1,200 analysis calls across 6 vision-language backends on [20 indoor scenes](scripts/benchmark_images/), plus 450 planning calls on 9 scheduling prompts, repeated trials). The main results:

- **A local 30B model is the deployment sweet spot.** Qwen3-VL-30B on a single GPU lands within ~0.6 points (of 9, LLM-judged) of the best cloud model on analysis quality, at **0.77 s median latency — 5–10× faster than cloud, at zero per-call cost — with a ~10× tighter latency tail.** On planning it routes natural-language requests as reliably as cloud Claude (≈100%) and trails only on schedule-count accuracy (59% vs 91–98%); the 3B model is excluded as too weak — it does not emit valid tool calls and scores lowest on quality. Recommendation: local VLM for perception, cloud for count-critical planning.

- **Extended thinking buys nothing here.** A no-thinking control arm matched or beat the thinking model on *both* planning and analysis quality while saving latency and tokens, so extended reasoning adds cost without accuracy in this perception-and-scheduling setting.

- **A duty-cycled node can stay agent-responsive.** The firmware sleeps in STOP2 between captures (datasheet ~2 µA floor) yet remains reachable: a **WIFI_PS_REST** mode keeps WiFi associated through sleep, and the MCU wakes from STOP2 on a 3-second poll cycle to check for commands, giving a **0.1–2.6 s command latency** (measured on hardware). Over 129 h of live deployment the on-device phase-timer measured a **100× lower daily energy** than continuous capture (252× projected for the WiFi-off DEEP_DORMANT schedule). The duty cycle is measured on-device; the absolute currents remain datasheet values (no power meter), so the Joule figures carry datasheet uncertainty while the ratio does not.

> Full per-backend tables, methodology, and limitations: [`results/findings_v3.md`](results/findings_v3.md) and the IEEE report. The quality ranking is robust to judge choice: a second, independent Gemini 3 judge agrees on the backend ranking at Spearman ρ = 0.83, with no self-preference bias. The evaluation corpus is the 20 curated [`scripts/benchmark_images/`](scripts/benchmark_images/) (MIT Indoor Scenes — Quattoni & Torralba, CVPR 2009; per-image provenance in [`MANIFEST.tsv`](scripts/benchmark_images/MANIFEST.tsv)), and the raw run is `results/benchmark_20260528_203558.jsonl`.

---

## The Agentic Pipeline

A natural language prompt becomes autonomous hardware behavior:

```mermaid
flowchart LR
    subgraph Input
        User["User prompt\n'Monitor the parking lot\nevery 30 min for 2 hours'"]
    end

    subgraph Planning ["Cloud — AI Planning"]
        Planner["Planning Engine\nClaude / Qwen"]
        Schedule["Schedule\n09:00, 09:30, 10:00, 10:30"]
        Planner --> Schedule
    end

    subgraph Edge ["STM32 Board"]
        MQTT_RX["MQTT\nReceive"]
        RTC["RTC Alarm\nWake from STOP2"]
        CAM["OV5640\nCapture RGB565"]
        JPEG["On-board JPEG encode\n614KB → ~10-45KB"]
        Upload["Resumable HTTP upload\n32KB chunks, deadline + retry"]
        MQTT_RX --> RTC --> CAM --> JPEG --> Upload
    end

    subgraph Analysis ["Cloud — Vision Analysis"]
        LLM["Multimodal LLM\nOpenRouter (Qwen3-VL) · Claude · Gemini 3"]
        Result["Findings +\nRecommendation"]
        LLM --> Result
    end

    subgraph Output
        Dashboard["Dashboard\nSSE + MQTT WebSocket"]
    end

    User --> Planner
    Schedule -- "MQTT command" --> MQTT_RX
    Upload --> LLM
    Result --> Dashboard
```

Production runs on **OpenRouter** — `qwen/qwen3.7-flash` for planning/tool-calling and `qwen/qwen3-vl-30b-a3b-instruct` for image analysis, chosen for the cost/latency profile of a live agent chat. Additional backends are wired in for thesis benchmarking (the Claude entries include a no-thinking control arm):
- **Claude Sonnet 4.6 / Sonnet 4.6 (no-think) / Haiku 4.5** via Anthropic API — benchmark comparison
- **Qwen3-VL-30B-A3B** via llama.cpp — open-weight, self-hosted on an RTX 6000 Ada
- **Qwen2.5-VL-3B** via llama.cpp — lightweight edge candidate
- **Gemini 3 Flash** via API — commercial baseline

---

## Architecture

```mermaid
graph TD
    subgraph Edge ["Edge Device — STM32 B-U585I-IOT02A"]
        direction TB
        CAM["OV5640 Camera\nVGA RGB565"]
        MCU["Cortex-M33 160MHz\nBare-Metal C"]
        WIFI["EMW3080 WiFi\nSPI Transport"]
        FLASH["Dual-Bank Flash\nOTA + Credentials"]

        CAM -- "DCMI / DMA" --> MCU
        MCU -- "SPI" --> WIFI
        MCU -- "HAL Flash" --> FLASH
    end

    subgraph Cloud ["Cloud VPS — Docker Compose"]
        direction TB
        MQTT["Mosquitto\nMQTT 3.1.1"]
        API["FastAPI\nAI Planner · Image Pipeline"]
        DB[("SQLite\nAsync")]
        LLM["Multimodal LLMs\nClaude · Gemini 3 · Qwen3-VL"]

        MQTT <-- "Pub/Sub" --> API
        API <--> DB
        API -- "Inference API" --> LLM
    end

    subgraph UI ["User Interface"]
        DASH["Next.js 16 Dashboard\nAgent Chat · Gallery · Console"]
    end

    WIFI -- "MQTT telemetry + commands" --> MQTT
    WIFI -- "HTTP POST images" --> API
    API -. "OTA firmware binary" .-> WIFI
    API -- "REST + SSE" --> DASH
    MQTT -- "WebSocket" --> DASH
```

---

## The Agentic Loop — How It Works

The dashboard's chat interface is backed by a full agentic loop: a user message triggers LLM reasoning, tool execution against real hardware, and streaming feedback — all in a single request/response cycle.

### 1. Entry Point: User Message

The user types a natural language message in the chat (e.g., *"Monitor the hallway every 5 minutes for an hour"*). The frontend sends this to `POST /api/agent/chat`, which returns a Server-Sent Events (SSE) stream. The frontend renders each event as it arrives — thinking indicators, tool execution spinners, results, and final replies — giving the user real-time visibility into what the agent is doing.

### 2. LLM Tool Dispatch

The server loads the conversation history from the database, appends the new message, and calls the planner model (OpenRouter, `qwen/qwen3.7-flash` by default) with tool-calling enabled — a single production backend, no model picker in the UI. The system prompt defines the agent's personality (action-first, bias toward capturing fresh data) and routing rules that map user intents to specific tools.

The model's response contains a mix of text and tool calls. Each tool call specifies which tool to invoke and with what parameters. The server processes these sequentially:

```
User: "Monitor for 30 seconds"
  → Claude decides: capture_sequence(count=4, interval_ms=7500)
  → Server executes the tool
  → Server streams progress events back
```

When no API key is configured, a **rule-based fallback dispatcher** maps keywords to tools directly — ensuring the system works without any LLM.

### 3. The Agent's 14 Tools

| Tool | Trigger Examples | What It Does |
|------|-----------------|--------------|
| `capture_now` | *"take a picture"*, *"what do you see"* | Sends `capture_now` MQTT command → board captures, JPEG-encodes on-device, and uploads over a resumable chunked transfer → multimodal LLM analyzes → findings streamed back with inline image thumbnail |
| `capture_sequence` | *"monitor for 30 seconds"*, *"burst of 5 shots"* | Creates a schedule in DB, sends `capture_sequence` with ms-precision delays to board, tracks per-image completion in real time |
| `create_schedule` | *"monitor every 10 min for 2 hours"* | AI planner generates HH:MM task list → saved to DB → activated → MQTT command to board sets RTC alarms |
| `activate_schedule` | *"activate schedule 3"*, *"resume monitoring"* | Activates an existing inactive schedule (deactivating all others first) without re-generating it |
| `deactivate_schedule` | *"stop monitoring"*, *"cancel"* | Deactivates the active schedule in DB → sends `delete_schedule` to board → dashboard updates in real time via MQTT |
| `modify_schedule` | *"change to every 15 min"*, *"reschedule for tomorrow"* | Re-plans an existing schedule via the AI planner — replaces its tasks in-place without creating a new record |
| `delete_schedule` | *"delete schedule 2"*, *"remove it"* | Permanently deletes a schedule and all its tasks from the database |
| `ping_board` | *"ping"*, *"is it alive"* | Sends `ping` MQTT command → board flashes LEDs in a pattern to confirm it's responsive |
| `start_portal` | *"setup mode"*, *"reconfigure wifi"* | Sends `start_portal` → board starts WiFi AP at 192.168.10.1 for credential reconfiguration |
| `sleep_mode` | *"sleep the board"*, *"wake up"* | Toggles STOP2 low-power mode on the board (2µA between tasks) |
| `analyze_latest` | *"show last analysis"* | Fetches the most recent AI analysis result from the database |
| `get_board_status` | *"status"*, *"uptime"* | Queries DB for analysis count, latest result, and board telemetry |
| `synthesize_schedule` | *"summarize findings"*, *"what did you learn"* | Reads all recent analyses, feeds them to Claude for cross-observation pattern synthesis |
| `delete_image` | *"delete that image"*, *"remove photo"* | Deletes a captured image and its AI analysis from storage and the database |

### 4. SSE Streaming Protocol

Each agent response is an SSE stream with typed events. The frontend renders these progressively:

```
data: {"event":"thinking","text":"Processing: \"take a picture\""}
data: {"event":"tool_call","id":"capture","label":"Sending capture command..."}
data: {"event":"tool_result","id":"capture","success":true,"summary":"Capture sent (task #42)"}
data: {"event":"tool_call","id":"upload","label":"Waiting for image..."}
data: {"event":"tool_update","id":"upload","heartbeat":true}       ← keepalive every 5 s
data: {"event":"tool_call","id":"img_1","label":"Image 1/1 ready…"}
data: {"event":"tool_result","id":"img_1","success":true,"summary":"Image 1/1 analyzed (task #42)","image_url":"/api/images/2026-04-26/task_42_1777204113.jpg"}
data: {"event":"tool_result","id":"upload","success":true,"summary":"1/1 images analyzed"}
data: {"event":"reply","text":"**Capture complete.** The hallway is empty..."}
data: {"event":"done"}
```

The frontend maps these to a visual execution trace: thinking indicator → spinning steps → checkmarks → final markdown reply. Each `tool_update` heartbeat keeps the SSE connection alive while the board is capturing. Per-image steps render a 280px inline thumbnail once complete; clicking opens the full image.

### 5. Real-Time MQTT Feedback

While the agent executes, the board independently publishes status updates over MQTT. The dashboard subscribes to 6 active topics and renders everything into a unified console:

| Topic | Direction | Content |
|-------|-----------|---------|
| `device/{id}/status` | Board → Dashboard | Telemetry: firmware, status, capture progress, sleep/wake |
| `device/{id}/logs` | Board → Dashboard | Raw firmware debug logs (`[ms] [LEVEL] [TAG] message`) |
| `dashboard/images/new` | Server → Dashboard | New image notification (triggers gallery refresh) |
| `dashboard/analysis/new` | Server → Dashboard | AI analysis result (updates gallery overlay) |
| `dashboard/logs` | Server → Dashboard | Server-side application logs (deploy, errors, agent activity) |
| `dashboard/schedules/updated` | Server → Dashboard | Full schedule state (activation, deactivation, task completion) |

The schedule updates topic (`dashboard/schedules/updated`) fires whenever any schedule state changes — activation, deactivation, deletion, or individual task completion. The server publishes the full schedule list, and the frontend replaces its state, giving instant real-time visibility into monitoring progress without polling.

### 6. Schedule Lifecycle

```
User message → LLM picks create_schedule → AI planner generates tasks
  → DB: schedule saved + activated (others deactivated)
  → MQTT: schedule command sent to board
  → Dashboard: schedules/updated (real-time UI update)

Board wakes via RTC alarm → captures image → JPEG-encodes on-device → resumable chunked upload to server
  → Server: reassembles + validates (CRC32), marks task.completed_at, triggers AI analysis
  → MQTT: images/new + schedules/updated (task progress bar advances)
  → MQTT: analysis/new (AI findings overlay appears on image)

All tasks done → Board sends cycle_complete
  → Server: auto-deactivates schedule
  → MQTT: schedules/updated (schedule marked completed in UI)

User: "stop monitoring" → LLM picks deactivate_schedule
  → DB: schedule.is_active = False
  → MQTT: delete_schedule to board + schedules/updated to dashboard
```

### 7. Conversation Persistence

Chat sessions are stored in SQLite (`chat_sessions` + `chat_messages` tables). Each board can have multiple named sessions. The agent loads the last 20 messages as context for Claude, so it can reference previous captures and maintain continuity across interactions. Sessions survive page reloads and server restarts.

---

## Technical Details

### Firmware (Bare-Metal, No RTOS)

- **Custom MQTT 3.1.1 client** over the EMW3080's TCP socket API. Connect, subscribe, publish, ping, and auto-reconnect in ~500 lines of C.
- **Adaptive AEC convergence** — polls the OV5640's luminance register at 50ms intervals. In well-lit scenes, captures in ~300ms. In dark scenes, detects AEC saturation (stable readings) and exits early instead of wasting the full timeout. Night mode extends exposure to 4x VTS (~300ms) automatically.
- **Over-the-air updates** — board polls for new firmware, downloads to RAM in 2KB chunks (avoiding SPI/flash contention), CRC32-verifies, erases inactive flash bank, writes, and performs atomic bank swap. Automatic rollback on boot failure.
- **Adaptive low-power sleep (agent-selectable via `low_power_mode`)** — *WIFI_PS_REST*: WiFi stays associated in 802.11 power-save while the MCU sleeps in STOP2, waking from STOP2 on a 3-second poll cycle to check for commands (command latency measured 0.1–2.6 s on hardware) — low-power *and* agent-responsive. *DEEP_DORMANT* (`sleep_mode`): WiFi off, ~2µA STOP2 until the next scheduled capture or a B3 button press. The IWDG watchdog is frozen in STOP via its option byte so it never resets the board mid-sleep, yet still guards the active phases and recovers a wake-time hang. (STOP2 wake required two STM32U5 silicon-config fixes — the `IWDG_STOP` option-byte freeze and the Smart-Run-Domain `SRDAMR.RTCAPBAMEN` RTC clock — both verified on-device.)
- **Captive portal WiFi provisioning** — no hardcoded credentials. Board starts a SoftAP (`IoT-Setup-XXXX`, password `setup123`) with DNS redirect for automatic captive portal detection on iOS/Android/Windows. Three entry paths: (1) first boot with no stored credentials, (2) stored credentials fail to connect after 3 retries, (3) **hold the B3 USER button for 3 seconds** — RED LED lights at 1s to signal "keep holding", 5x GREEN blinks confirm portal entry. Short press (<3s) triggers an instant capture.
- **Phone hotspot compatible** — EMW3080 uses `MX_WIFI_SEC_AUTO` (WPA/WPA2 PSK only; the module firmware does not support SAE/dragonfly handshake), 2.4GHz only. Explicit 30-second DHCP patience for iPhone hotspot latency. Server addressed by raw IP — no DNS dependency. Does **not** support WPA2-Enterprise (802.1X/RADIUS), which is why university WiFi networks are incompatible.
- **11 MQTT command types**: capture_now, capture_sequence, schedule, delete_schedule, firmware_update, sleep_mode, low_power_mode, ping, set_wifi, start_portal, erase_wifi.

### Backend (FastAPI + AI)

- **AI planning engine** — translates natural language monitoring requests into executable HH:MM task schedules with objectives. The planner is model-agnostic (Claude, Gemini, Qwen).
- **Agentic chat with 14 tools** — the dashboard chat is backed by an OpenRouter-hosted model (`qwen/qwen3.7-flash`) with OpenAI-style tool calling, with an automatic Claude/vLLM fallback path if no OpenRouter key is configured. The agent can capture images, create/activate/deactivate/modify/delete schedules, ping the board, toggle sleep mode, enter setup mode, analyze results, synthesize findings, and delete images — all through natural language, and mid-generation requests can be cancelled from the UI.
- **Full capture pipeline with SSE streaming** — when the agent triggers a capture, the server streams real-time progress events (command sent, image received, per-image analysis, inline thumbnail URL) back to the dashboard. Heartbeat events every 5 s keep the SSE connection alive. For sequences, each image completion is a separate step.
- **Resumable chunked image upload** — `/api/upload/chunk`, `/api/upload/resume`, `/api/upload/complete` let the board send an image as independent byte-ranged chunks over a degraded link, resuming from the server's confirmed offset instead of restarting after a stall, with a CRC32 check on reassembly. The board additionally JPEG-encodes on-device before sending, so a raw-RGB565 fallback (RGB565 → JPEG server-side, same R[15:11] G[10:5] B[4:0] unpacking) only runs if the on-board encoder can't fit a frame in its output buffer.
- **Auto-deactivation** — when the board reports `cycle_complete`, the server automatically deactivates the active schedule.
- **Real-time schedule notifications** — every schedule state change (activate, deactivate, delete, task completion) publishes the full schedule list to `dashboard/schedules/updated` via MQTT, so the frontend updates instantly without polling.
- **Dormancy-safe command delivery** — all board commands route through a single `send_board_command()` chokepoint. When the board reports `deep_dormant`, commands are queued in order and replayed automatically on the next wake, so commands issued during deep sleep are never lost (the board's MQTT is QoS 0, so the broker won't buffer them).

### Dashboard (Next.js 16 + React 19)

- **Always-visible console** — board firmware logs stream via MQTT (`device/stm32/logs`) and are parsed with the firmware's `[ms] [LEVEL] [TAG] message` format, color-coded by level.
- **Agent chat with tool streaming** — SSE events render as a step-by-step execution trace (thinking, tool calls, results, reply). Captured images appear as inline thumbnails in the chat immediately after analysis. Stop button aborts mid-generation, cancelling the in-flight OpenRouter call server-side too.
- **Real-time telemetry** — board status, firmware version, uptime, WiFi RSSI, capture count, connection state (online/standby/offline) all update live via MQTT WebSocket.
- **Multi-session chat persistence** — conversations are stored in the database and survive page reloads.

### CI/CD

- **Path-based filtering** — `dorny/paths-filter` detects which components changed. A firmware-only change skips dashboard builds.
- **Full pipeline**: pytest (90 tests) → Biome lint → TypeScript check → Next.js build → ARM GCC cross-compile → Docker build → VPS deploy → OTA firmware upload. The firmware build/upload job is individually time-bounded (15 min job, 5 min toolchain install) so a runner-side apt stall fails fast instead of blocking a deploy for hours.
- **CI-driven deploy** — on each push the CI job force-recreates the server and dashboard containers on the VPS over SSH; a `nickfedor/watchtower` service additionally polls GHCR every 5 minutes as a fallback.

---

## Repository Structure

```
hesperus/
  firmware/               Bare-metal C (ARM GCC, STM32U585AI)
    Core/Src/
      main.c              Event loop, scheduler, command dispatch
      camera.c            OV5640 driver, AEC tuning, DCMI/DMA capture
      mqtt_handler.c      Hand-rolled MQTT 3.1.1 client
      ota_update.c        Dual-bank OTA with CRC32 + rollback
      wifi.c              EMW3080 TCP/HTTP, NTP, socket management
      captive_portal.c    SoftAP WiFi provisioning web server
    Core/Inc/
      firmware_config.h   All tuneable parameters in one file
    Drivers/              ST BSP + OV5640 + EMW3080 drivers

  server/                 FastAPI backend (Python 3.11)
    app/
      api/
        agent_routes.py   Agentic chat (Claude tool_use + SSE streaming)
        routes.py         Image upload, RGB565 conversion, analysis trigger
        scheduler_routes.py  Schedule CRUD + MQTT dispatch
      agent/              Chat session persistence (SQLAlchemy)
      analysis/           Multimodal LLM analysis pipeline
      planning/           NL prompt to schedule generation
      mqtt/               Async MQTT client + auto-deactivation
    tests/                74 pytest tests (async, in-memory SQLite)

  dashboard/              Next.js 16 frontend (TypeScript, React 19)
    app/
      page.tsx            Fleet overview with live MQTT telemetry
      board/[id]/page.tsx Board detail: chat + gallery + console
      components/
        AgentChat.tsx     Multi-session chat with SSE tool streaming
      hooks/
        useMQTT.ts        WebSocket MQTT hook with board tracking

  mosquitto/              MQTT broker configuration
  docker-compose.yml      Development stack
  docker-compose.prod.yml Production (GHCR images, CI force-recreate + Watchtower fallback)
  .github/workflows/
    ci.yml                Adaptive CI/CD pipeline
```

---

## MQTT Command Reference

All commands are JSON payloads on `device/stm32/commands`:

| Command | Payload | Board Response |
|---------|---------|----------------|
| Single capture | `{"type":"capture_now","task_id":N}` | Capture + HTTP upload + status updates |
| Timed sequence | `{"type":"capture_sequence","task_id":N,"delays_ms":[0,5000,10000]}` | Multiple captures at ms offsets |
| Schedule | `{"type":"schedule","tasks":[{"time":"09:00","action":"CAPTURE_IMAGE","objective":"..."}]}` | Stored in RAM, RTC alarms set |
| Delete schedule | `{"type":"delete_schedule"}` | Clears active schedule |
| OTA update | `{"type":"firmware_update"}` | Polls server, downloads, flashes, reboots |
| Sleep toggle | `{"type":"sleep_mode","enabled":true}` | DEEP_DORMANT (~2µA) STOP2 between scheduled tasks |
| Low-power mode | `{"type":"low_power_mode","mode":"ps_rest"}` | WIFI_PS_REST: WiFi-associated STOP2 with wake-on-ping (`"off"` to disable) |
| Ping | `{"type":"ping"}` | LED strobe (3x red, 3x green) + MQTT ack |
| WiFi config | `{"type":"set_wifi","ssid":"...","password":"..."}` | Save to flash + reconnect |
| WiFi portal | `{"type":"start_portal"}` | Start SoftAP at 192.168.10.1 |
| Erase WiFi | `{"type":"erase_wifi"}` | Wipe credentials + reboot to portal |

---

## Quick Start

### Cloud (VPS or local)

```bash
git clone https://github.com/whitehatd/hesperus.git
cd hesperus
cp server/.env.example server/.env   # Configure API keys
docker compose up -d                 # Mosquitto + FastAPI + Dashboard
```

Dashboard at `http://localhost:3000`, API at `http://localhost:8000/docs`.

### Firmware (first flash only)

```bash
cd firmware
make -j8                                              # Cross-compile
make flash                                            # STLink (one time)
# All subsequent updates deploy via OTA through CI/CD
```

Configure `firmware/Core/Inc/firmware_config.h` for your WiFi SSID and server IP. Or use the captive portal — the board broadcasts a setup network on first boot.

### CI/CD (automatic after setup)

Push to `main` -> GitHub Actions builds everything -> VPS deploys -> board updates OTA.

---

<sub>Bachelor thesis project — Alexandru-Ionut Cioc, 2026</sub>
