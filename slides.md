---
marp: true
theme: default
paginate: true
html: true
backgroundColor: #0d1117
color: #e6edf3
style: |

  /* Palette
     bg #0d1117 · surface rgba(255,255,255,.04) · border rgba(255,255,255,.08)
     text #e6edf3 · muted #7d8590
     red #f85149 · blue #58a6ff · green #3fb950 · yellow #d29922
  */

  section {
    font-family: 'Segoe UI Variable Text', 'Segoe UI', -apple-system,
                 BlinkMacSystemFont, system-ui, sans-serif;
    font-size: 24px;
    line-height: 1.7;
    padding: 64px 88px;
    background: #0d1117;
    color: #e6edf3;
  }

  h1 { font-size: 1.9em; color: #e6edf3; font-weight: 800; letter-spacing: -0.02em; }
  h2 {
    font-family: 'Segoe UI Variable Display', 'Segoe UI', system-ui, sans-serif;
    color: #e6edf3;
    font-size: 1.35em;
    font-weight: 700;
    letter-spacing: -0.015em;
    padding-bottom: 0;
    margin: 0 0 28px;
    border: none;
  }
  h2::after {
    content: '';
    display: block;
    width: 36px; height: 3px;
    background: #f85149;
    margin-top: 14px;
    border-radius: 2px;
  }

  ul { padding-left: 0; margin: 0; list-style: none; }
  li {
    margin-bottom: 14px;
    line-height: 1.55;
    color: #c9d1d9;
    padding-left: 20px;
    position: relative;
  }
  li::before {
    content: '';
    position: absolute;
    left: 0; top: 0.7em;
    width: 6px; height: 6px;
    border-radius: 50%;
    background: #f85149;
    opacity: 0.65;
  }
  li strong { color: #e6edf3; font-weight: 600; }

  /* ── Title ─────────────────────────────── */
  section.title {
    background:
      radial-gradient(ellipse 60% 80% at 12% 60%, rgba(88,166,255,0.13) 0%, transparent 60%),
      radial-gradient(ellipse 50% 60% at 88% 25%, rgba(248,81,73,0.10) 0%, transparent 55%),
      #0d1117;
    display: flex;
    flex-direction: column;
    justify-content: center;
    padding: 72px 96px;
  }
  section.title h1 {
    color: #f0f6ff;
    font-size: 1.45em;
    line-height: 1.28;
    font-weight: 800;
    letter-spacing: -0.025em;
    margin: 0 0 24px;
    max-width: 90%;
    border: none;
  }
  section.title .sub {
    color: #8b949e;
    font-size: 0.78em;
    line-height: 1.6;
    font-style: italic;
    margin-bottom: 36px;
    max-width: 75%;
  }
  section.title .rule {
    width: 44px; height: 3px;
    background: linear-gradient(90deg, #f85149, #58a6ff);
    border-radius: 2px;
    margin-bottom: 32px;
  }
  section.title .meta {
    font-size: 0.68em;
    color: #7d8590;
    line-height: 1.95;
  }
  section.title .meta strong { color: #c9d1d9; font-weight: 600; }
  section.title .links {
    display: flex; gap: 12px; margin-top: 36px; flex-wrap: wrap;
  }
  section.title .chip {
    font-family: 'Cascadia Code', 'Cascadia Mono', 'Consolas', monospace;
    font-size: 0.6em; font-weight: 500;
    padding: 6px 14px; border-radius: 6px;
    background: rgba(255,255,255,0.05);
    border: 1px solid rgba(255,255,255,0.1);
    color: #8b949e;
  }
  section.title .chip.live {
    background: rgba(248,81,73,0.12);
    border-color: rgba(248,81,73,0.35);
    color: #ffa198;
  }
  section.title .chip.live::before { content: '● '; color: #f85149; }

  /* ── Code ──────────────────────────────── */
  code {
    font-family: 'Cascadia Code', 'Cascadia Mono', 'Consolas', monospace;
    background: rgba(255,255,255,0.07);
    color: #79c0ff;
    padding: 2px 8px;
    border-radius: 4px;
    font-size: 0.78em;
    border: 1px solid rgba(255,255,255,0.08);
  }

  /* ── Tables ────────────────────────────── */
  table {
    width: 100%; font-size: 0.78em; border-collapse: collapse;
    background: transparent !important;
  }
  thead {
    border-bottom: 1px solid rgba(255,255,255,0.12);
    background: transparent !important;
  }
  thead th {
    padding: 11px 18px; text-align: left;
    font-weight: 600; color: #7d8590;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    font-size: 0.75em;
    background: transparent !important;
  }
  tbody { background: transparent !important; }
  tbody tr { background: transparent !important; }
  tbody tr:nth-child(even) {
    background: rgba(255,255,255,0.025) !important;
  }
  tbody td {
    padding: 11px 18px;
    border: none;
    color: #c9d1d9;
    background: transparent !important;
  }

  /* ── Two-column ────────────────────────── */
  .cols {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 64px;
    align-items: start;
  }
  .col h3 {
    font-family: 'Segoe UI Variable Display', 'Segoe UI', sans-serif;
    font-size: 0.7em; font-weight: 700;
    letter-spacing: 0.1em; text-transform: uppercase;
    color: #7d8590;
    margin: 0 0 24px;
  }
  .col-l h3 { color: #f85149; }
  .col-r h3 { color: #58a6ff; }
  .col li::before { background: currentColor; opacity: 0.4; }
  .col-l li::before { background: #f85149; opacity: 0.5; }
  .col-r li::before { background: #58a6ff; opacity: 0.5; }

  /* ── RQ cards ──────────────────────────── */
  .rq {
    background: rgba(255,255,255,0.035);
    border: 1px solid rgba(255,255,255,0.08);
    border-left: 3px solid #58a6ff;
    border-radius: 8px;
    padding: 14px 20px;
    margin-bottom: 10px;
  }
  .rq .tag {
    display: inline-block;
    background: #f85149;
    color: #fff;
    font-size: 0.6em; font-weight: 700;
    padding: 3px 10px; border-radius: 4px;
    letter-spacing: 0.08em;
    margin-bottom: 7px;
    font-family: 'Segoe UI Variable Display', sans-serif;
  }
  .rq .q {
    font-size: 0.9em;
    color: #e6edf3;
    font-weight: 600;
    line-height: 1.4;
    letter-spacing: -0.005em;
  }
  .rq .kpi {
    display: block;
    margin-top: 7px;
    font-size: 0.75em;
    color: #7d8590;
    letter-spacing: 0.01em;
  }

  /* ── Badges ────────────────────────────── */
  .done {
    display: inline-block;
    background: rgba(63,185,80,0.13);
    color: #3fb950;
    font-size: 0.85em; font-weight: 500;
    padding: 3px 10px; border-radius: 4px;
    border: 1px solid rgba(63,185,80,0.3);
  }
  .wip {
    display: inline-block;
    background: rgba(210,153,34,0.13);
    color: #d29922;
    font-size: 0.85em; font-weight: 500;
    padding: 3px 10px; border-radius: 4px;
    border: 1px solid rgba(210,153,34,0.3);
  }

  /* ── Stat boxes ────────────────────────── */
  .stats { display: flex; gap: 14px; margin-top: 36px; }
  .stat {
    flex: 1;
    background: rgba(255,255,255,0.04);
    border: 1px solid rgba(255,255,255,0.08);
    border-radius: 10px;
    padding: 22px 14px 18px;
    text-align: center;
  }
  .stat .v {
    font-family: 'Segoe UI Variable Display', 'Segoe UI', sans-serif;
    font-size: 1.8em; font-weight: 800;
    color: #58a6ff;
    display: block;
    line-height: 1; margin-bottom: 8px;
    letter-spacing: -0.025em;
  }
  .stat .l {
    font-size: 0.5em; color: #7d8590;
    text-transform: uppercase; letter-spacing: 0.08em;
    display: block;
  }

  /* ── Architecture ──────────────────────── */
  .arch {
    display: flex;
    align-items: stretch;
    margin: 28px 0 20px;
    border-radius: 10px;
    overflow: hidden;
    border: 1px solid rgba(255,255,255,0.08);
  }
  .anode { flex: 1; padding: 26px 22px; text-align: center; }
  .anode .at {
    font-size: 0.6em; font-weight: 700;
    text-transform: uppercase; letter-spacing: 0.1em;
    display: block; margin-bottom: 14px;
  }
  .anode .ad {
    font-size: 0.7em;
    line-height: 1.85;
    color: rgba(255,255,255,0.65);
    font-family: 'Cascadia Code', 'Cascadia Mono', 'Consolas', monospace;
  }
  .aconn {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 0 4px;
    min-width: 36px;
    background: rgba(255,255,255,0.02);
    border-left: 1px solid rgba(255,255,255,0.06);
    border-right: 1px solid rgba(255,255,255,0.06);
  }
  .aconn .ar {
    font-size: 1.6em;
    color: #6e7681;
    line-height: 1;
  }
  .n1 { background: #161b22; }
  .n1 .at { color: #58a6ff; }
  .n2 { background: #0f1c2d; }
  .n2 .at { color: #79c0ff; }
  .n3 { background: #1c0f10; }
  .n3 .at { color: #f85149; }

  .pipe {
    background: rgba(255,255,255,0.025);
    border: 1px solid rgba(255,255,255,0.06);
    border-left: 3px solid #f85149;
    border-radius: 7px;
    padding: 14px 22px;
    font-family: 'Cascadia Code', 'Cascadia Mono', 'Consolas', monospace;
    font-size: 0.65em;
    text-align: center;
    color: #8b949e;
    margin: 16px 0 0;
  }
  .pipe .hi { color: #f85149; font-weight: 600; }
  .pipe .bl { color: #58a6ff; font-weight: 600; }

  /* ── Callout ────────────────────────────── */
  .box {
    background: rgba(88,166,255,0.07);
    border: 1px solid rgba(88,166,255,0.2);
    border-radius: 8px;
    padding: 18px 24px;
    margin-top: 24px;
    font-size: 0.85em;
    color: #c9d1d9;
    line-height: 1.5;
  }
  .box strong { color: #58a6ff; }

  /* ── KPI grid ──────────────────────────── */
  .kpi-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; margin-top: 12px; }
  .kpi-card {
    background: rgba(255,255,255,0.035);
    border: 1px solid rgba(255,255,255,0.08);
    border-left: 3px solid #f85149;
    border-radius: 8px;
    padding: 16px 18px;
  }
  .kpi-card .kn {
    font-weight: 700; color: #e6edf3;
    display: block; margin-bottom: 5px;
    font-size: 0.95em;
  }
  .kpi-card .kd {
    color: #7d8590;
    font-size: 0.8em;
    line-height: 1.45;
  }

  .dim { color: #7d8590; }
  section::after { color: #484f58; font-size: 0.55em; }

  /* ── HTML tables (dark-safe, no thead) ── */
  .dark-table { width: 100%; border-collapse: collapse; font-size: 0.8em; }
  .dark-table tr { background: transparent !important; }
  .dark-table td {
    padding: 11px 18px;
    border-bottom: 1px solid rgba(255,255,255,0.05);
    color: #c9d1d9;
    background: transparent !important;
  }
  .dark-table tr.hdr td {
    color: #7d8590;
    font-weight: 700;
    font-size: 0.78em;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    border-bottom: 1px solid rgba(255,255,255,0.14);
    background: transparent !important;
  }
  .dark-table tr:last-child td { border-bottom: none; }
  .dark-table tr.dl td { color: #e6edf3; border-top: 1px solid rgba(248,81,73,0.3); }
  .dark-table tr.dl td:first-child { color: #f85149; font-weight: 700; }
---

<!-- _class: title -->

# Autonomous IoT Visual Monitoring with Cloud-Based AI Planning and Analysis

<p class="sub">You describe what to monitor. A microcontroller does the rest — autonomously.</p>
<div class="rule"></div>

<p class="meta">
  <strong>Alexandru-Ionut Cioc</strong><br>
  <strong>Prof. Guangzhi Tang</strong> · <strong>Prof. Charis Kouzinopoulos</strong><br>
  Dept. of Advanced Computing Sciences · Maastricht University
</p>

<div class="links">
  <span class="chip live">hesperus.ciocandco.com — live</span>
  <span class="chip">github.com/WhitehatD/hesperus</span>
</div>

<!-- Speaker note: Good morning. To orient everyone in one sentence: this is a system that takes a plain-English monitoring request and turns it into autonomous hardware behaviour — running live on my desk right now at the IP shown. The core idea: you describe what to monitor in plain English, like "check the parking lot every morning at 8", and a physical microcontroller does the rest — schedules itself, wakes up, captures, sends the image to an AI for analysis, and streams the result back to a live dashboard. No human in the loop. -->

---

## Motivation

<div class="cols">
<div class="col col-l">

### Status quo

- Always-on cameras
- 2 GB / hour bandwidth
- No sleep cycle
- Pixels, not meaning

</div>
<div class="col col-r">

### What's now possible

- LLMs reason about images
- MCUs sleep at 2 µA
- Capture only when planned

</div>
</div>

<div class="box">
<strong>The gap:</strong> no published system, to our knowledge, uses an LLM as an executive controller that directly configures IoT hardware.
</div>

<!-- Speaker note: The problem: most security cameras run continuously. That's gigabytes of mostly nothing, 24/7, and even then a human has to watch it to understand what happened. Two things changed recently. First, modern AI models can look at an image and actually tell you what's in it. Second, modern microcontrollers can sleep at almost zero current draw and wake up on a precise timer. So the idea is — instead of always-on recording, let AI figure out when to capture and what to look for, then have the hardware execute that plan. To our knowledge, no published system has connected them like this — using the LLM as an executive controller that directly configures hardware registers and schedules. The literature survey in the thesis backs that claim, but the phrasing on the slide is intentionally hedged. -->

---

## Research Questions

<div class="rq">
  <span class="tag">RQ 1</span>
  <div class="q">How can natural language user requirements be effectively decomposed and translated into executable, energy-efficient hardware schedules for constrained IoT devices?</div>
</div>

<div class="rq">
  <span class="tag">RQ 2</span>
  <div class="q">What are the trade-offs in inference latency, accuracy, and operational cost between local open-weight models (Qwen3-VL) versus commercial cloud APIs (Gemini 3) for IoT visual data analysis?</div>
</div>

<div class="rq">
  <span class="tag">RQ 3</span>
  <div class="q">To what extent can an autonomous plan-execute-analyze feedback loop reduce data transmission and power consumption compared to continuous monitoring?</div>
</div>

<!-- Speaker note: Three research questions. The first is about whether AI can reliably turn a plain English instruction into a real hardware schedule — specific times, capture frequencies, what to look for. The second is a practical comparison: if I host an open-source AI model privately versus using a paid cloud API like Gemini, what do I actually lose or gain in accuracy, speed, and cost? The third is the payoff question: does this whole approach actually save power and bandwidth compared to just leaving a camera running all the time? -->

---

## Architecture

<div class="arch">
  <div class="anode n1">
    <span class="at">Edge</span>
    <div class="ad">STM32U585AI<br>OV5640 · WiFi<br>2 µA sleep</div>
  </div>
  <div class="aconn"><span class="ar">⇄</span></div>
  <div class="anode n2">
    <span class="at">Cloud · Docker on VPS</span>
    <div class="ad">FastAPI :8000<br>Mosquitto :1883<br>4 LLM backends</div>
  </div>
  <div class="aconn"><span class="ar">⇄</span></div>
  <div class="anode n3">
    <span class="at">Browser</span>
    <div class="ad">Next.js 16 :80<br>Agentic chat<br>Real-time MQTT</div>
  </div>
</div>

<div class="pipe">
<span class="hi">NL prompt</span> &nbsp;→&nbsp; AI planner &nbsp;→&nbsp; schedule to board &nbsp;→&nbsp; capture &nbsp;→&nbsp; AI analysis &nbsp;→&nbsp; <span class="bl">live dashboard</span>
</div>

<!-- Speaker note: Three components. Left is the physical board — a microcontroller with a camera and WiFi module, sitting on my desk, sleeping at almost zero current between captures. In the middle is the cloud backend: a Python API server that handles the AI planning and image analysis, plus an MQTT message broker — all in Docker on a cheap VPS. On the right is the dashboard, a web app where I or an examiner can type instructions and see what the system found. The pipeline at the bottom shows the full loop: I type a monitoring goal, AI turns it into a schedule, the schedule goes to the board, the board wakes up and captures, the image comes back, AI analyses it, and the result streams live to the browser. -->

---

## Firmware

<div class="cols">
<div class="col col-l">

### Power

- Deep sleep at 2 µA
- Hardware timer wake
- Auto-exposure stabilisation
- Hardware watchdog

</div>
<div class="col col-r">

### Connectivity

- Custom MQTT client (~400 lines C)
- Wireless firmware updates + auto-rollback
- WiFi provisioning via browser

</div>
</div>

<div class="box" style="margin-top: 36px;">
Bare-metal C · STM32U585AI · No RTOS · ~5,000 lines
</div>

<!-- Speaker note: Bare-metal C, no operating system. Between captures the board sleeps at about two microamps and a hardware timer wakes it on schedule. I wrote the WiFi messaging protocol from scratch — no suitable library existed for this chip. Updates ship wirelessly with auto-rollback, and first-time WiFi setup runs through a browser, so no credentials are baked into the binary. -->

---

## Backend & Dashboard

<div class="cols">
<div class="col col-l">

### Server

- Natural language → schedule planner
- Camera image conversion + analysis
- 4 AI model backends
- Live results streaming

</div>
<div class="col col-r">

### Dashboard

- AI assistant with tool access
- Real-time device updates
- Inline image thumbnails
- Multi-session history

</div>
</div>

<div class="box" style="margin-top: 36px; font-size: 0.78em;">
<strong>Every git push:</strong>&nbsp;&nbsp;<code>tests</code> · <code>lint</code> · <code>build</code> · <code>ARM compile</code> · <code>Docker push</code> · <code>deploy VPS</code> · <code>update board</code>
</div>

<!-- Speaker note: The server does two things: planning — turning a natural-language instruction into a structured schedule — and analysis, sending each captured image to whichever of the four AI models I'm benchmarking. The dashboard is an AI chat interface that can call functions on the live system mid-conversation. Every git push runs the full pipeline automatically: tests, lint, build, ARM cross-compile, Docker push, VPS deploy, board update. -->

---

## Evaluation

<p class="dim" style="margin-top: -12px; margin-bottom: 28px; font-size: 0.92em;">
20 captures × 4 models · full end-to-end timing per run
</p>

<div class="kpi-grid">
  <div class="kpi-card">
    <span class="kn">Plan Accuracy</span>
    <span class="kd">Did the system understand the monitoring request?</span>
  </div>
  <div class="kpi-card">
    <span class="kn">Energy Efficiency</span>
    <span class="kd">Power draw vs always-on camera over 24 h</span>
  </div>
  <div class="kpi-card">
    <span class="kn">Operational Latency</span>
    <span class="kd">Time from typed request to first photo taken</span>
  </div>
  <div class="kpi-card">
    <span class="kn">Analysis Quality</span>
    <span class="kd">How accurately each model describes the image</span>
  </div>
</div>

<p class="dim" style="margin-top: 28px; font-size: 0.85em;">Models: Claude Haiku 4.5 · Gemini 3 Flash · Qwen3-VL-30B (self-hosted) · Qwen2.5-VL-3B (self-hosted)</p>

<!-- Speaker note: For the evaluation I'm running 20 captures with each of four AI models and measuring the things the research questions actually ask about. Does the AI correctly understand what I asked it to monitor? How much power does this approach save compared to an always-on camera? How long does it take from when I type a request to when the board actually takes the first photo? And how accurately does each model describe what's in the image? Two of the models are commercial — you pay per call. Two are open-source models I run on a local GPU, so no cost and the images stay private. Data collection starts this week. -->

---

## Progress

<table class="dark-table">
  <tr class="hdr"><td>Component</td><td>Status</td></tr>
  <tr><td>Firmware · OTA · Sleep modes · WiFi provisioning</td><td><span class="done">Complete</span></td></tr>
  <tr><td>Backend · AI planning · 4 model backends</td><td><span class="done">Complete</span></td></tr>
  <tr><td>Dashboard · AI chat · Real-time updates</td><td><span class="done">Complete</span></td></tr>
  <tr><td>Benchmark tooling</td><td><span class="done">Complete</span></td></tr>
  <tr><td>Benchmark data collection</td><td><span class="wip">This week</span></td></tr>
  <tr><td>Thesis writing (LaTeX)</td><td><span class="wip">In progress</span></td></tr>
</table>

<div class="stats">
  <div class="stat"><span class="v">14k</span><span class="l">Lines of code</span></div>
  <div class="stat"><span class="v">57</span><span class="l">Tests</span></div>
  <div class="stat"><span class="v">263</span><span class="l">Commits</span></div>
  <div class="stat"><span class="v">4</span><span class="l">LLM backends</span></div>
  <div class="stat"><span class="v">Live</span><span class="l">on VPS</span></div>
</div>

<!-- Speaker note: To give a sense of where things stand — everything in the implementation is done and the system has been running live on a server for weeks. 14,000 lines of code, 57 passing tests, 263 commits. What's left is collecting the benchmark data, which I start this week, and writing the thesis, which I've already started. -->

---

## Plan to June

<table class="dark-table">
  <tr class="hdr"><td>Window</td><td>Focus</td></tr>
  <tr><td><strong>Apr 28 – May 4</strong></td><td>Benchmark runs</td></tr>
  <tr><td><strong>May 5 – 25</strong></td><td>Thesis writing — all chapters</td></tr>
  <tr class="dl"><td>May 26</td><td>First draft → supervisors</td></tr>
  <tr><td><strong>Jun 1 – 4</strong></td><td>Revisions</td></tr>
  <tr class="dl"><td>Jun 5</td><td>Final version → Canvas</td></tr>
  <tr><td><strong>Jun 8 – 19</strong></td><td>Defense window</td></tr>
</table>

<div class="box" style="margin-top: 28px; font-size: 0.82em;">
Deadlines: first draft <strong>May 26</strong> · final version <strong>Jun 5</strong> · defense <strong>Jun 8–19</strong>
&nbsp;·&nbsp;
Live: <strong>89.167.11.147</strong>
</div>

<!-- Speaker note: Two hard deadlines. May 26th I need to send the first full draft to both supervisors. June 5th is the final submission to Canvas — examiners and the thesis coordinator receive it then. Defense window is June 8th to 19th. I have three weeks of writing left starting now, with benchmark data coming in this week. I already have detailed technical documentation for every component so the writing is mostly structuring what I built and interpreting the numbers. The system will be live during the defense if the examiners want to interact with it. Thank you. -->
