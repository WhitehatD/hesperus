"use client";

// Virtual board: this page, open on a phone, replaces the STM32 camera.
//
//   agent / dashboard ── MQTT device/stm32/commands ──► this page
//   this page ── POST /api/upload?task_id=N (JPEG, owner cookie) ──► server
//   this page ── POST /api/camera/status ──► server ──► MQTT device/stm32/status
//
// The server cannot tell it apart from the board: same command payloads,
// same upload endpoint, same status messages (see
// server/app/api/camera_routes.py for why status goes through the server).
//
// Handles capture_now, capture_sequence and ping. Everything else the board
// understood (schedules, sleep, wifi, OTA) has no meaning for a phone and is
// shown in the log as ignored.
//
// Browsers only give camera access to a visible page on HTTPS (or
// localhost), so the phone must stay on this page with the screen on. A
// screen wake lock is requested for that.

import { useCallback, useEffect, useRef, useState } from "react";
import { useMQTT } from "@/hooks/useMQTT";
import styles from "./page.module.css";

const COMMAND_TOPIC = "device/stm32/commands";
const HEARTBEAT_MS = 10_000; // dashboard marks a board offline after 15 s
const MAX_SEQUENCE = 16;
const JPEG_QUALITY = 0.9;
const MAX_WIDTH = 1280; // full phone resolution is slow to upload and analyse

type LogLine = { at: string; text: string; kind: "info" | "ok" | "error" };
type WakeLockSentinelLike = { release: () => Promise<void> };

async function postStatus(status: string, taskId?: number, detail?: string) {
	try {
		await fetch("/api/camera/status", {
			method: "POST",
			headers: { "Content-Type": "application/json" },
			body: JSON.stringify({
				status,
				task_id: taskId ?? null,
				detail: detail ?? "",
			}),
		});
	} catch {
		// Best effort: a missed status only delays the "online" dot.
	}
}

export default function CameraPage() {
	const videoRef = useRef<HTMLVideoElement>(null);
	const canvasRef = useRef<HTMLCanvasElement>(null);
	const busyRef = useRef(false);
	const [cameraError, setCameraError] = useState<string | null>(null);
	const [cameraReady, setCameraReady] = useState(false);
	const [facing, setFacing] = useState<"environment" | "user">("environment");
	const [flash, setFlash] = useState(false);
	const [log, setLog] = useState<LogLine[]>([]);
	const [shots, setShots] = useState(0);

	const addLog = useCallback((text: string, kind: LogLine["kind"] = "info") => {
		const at = new Date().toLocaleTimeString([], {
			hour: "2-digit",
			minute: "2-digit",
			second: "2-digit",
		});
		setLog((prev) => [{ at, text, kind }, ...prev].slice(0, 40));
	}, []);

	// ── Camera stream ──
	useEffect(() => {
		let stream: MediaStream | null = null;
		let cancelled = false;
		setCameraReady(false);
		if (!navigator.mediaDevices?.getUserMedia) {
			setCameraError(
				"This browser has no camera access here. Open the page over HTTPS.",
			);
			return;
		}
		navigator.mediaDevices
			.getUserMedia({
				video: { facingMode: { ideal: facing }, width: { ideal: 1920 } },
				audio: false,
			})
			.then((s) => {
				if (cancelled) {
					for (const t of s.getTracks()) t.stop();
					return;
				}
				stream = s;
				if (videoRef.current) {
					videoRef.current.srcObject = s;
					videoRef.current.play().catch(() => {});
				}
				setCameraError(null);
				setCameraReady(true);
			})
			.catch((e: Error) => setCameraError(`Camera unavailable: ${e.message}`));
		return () => {
			cancelled = true;
			if (stream) for (const t of stream.getTracks()) t.stop();
		};
	}, [facing]);

	// ── Keep the screen on ──
	useEffect(() => {
		let lock: WakeLockSentinelLike | null = null;
		const nav = navigator as Navigator & {
			wakeLock?: { request: (t: "screen") => Promise<WakeLockSentinelLike> };
		};
		const acquire = () => {
			if (document.visibilityState === "visible" && nav.wakeLock) {
				nav.wakeLock
					.request("screen")
					.then((l) => {
						lock = l;
					})
					.catch(() => {});
			}
		};
		acquire();
		document.addEventListener("visibilitychange", acquire);
		return () => {
			document.removeEventListener("visibilitychange", acquire);
			lock?.release().catch(() => {});
		};
	}, []);

	// ── Heartbeat: tells the server this "board" is online ──
	useEffect(() => {
		if (!cameraReady) return;
		postStatus("online");
		const t = setInterval(() => postStatus("online"), HEARTBEAT_MS);
		return () => clearInterval(t);
	}, [cameraReady]);

	// ── Take one photo and upload it ──
	const captureAndUpload = useCallback(
		async (taskId: number): Promise<boolean> => {
			const video = videoRef.current;
			const canvas = canvasRef.current;
			if (!video || !canvas || !video.videoWidth) {
				addLog(`Task #${taskId}: camera not ready`, "error");
				await postStatus("error", taskId, "camera not ready");
				return false;
			}
			await postStatus("capturing", taskId);
			setFlash(true);
			setTimeout(() => setFlash(false), 150);

			const scale = Math.min(1, MAX_WIDTH / video.videoWidth);
			canvas.width = Math.round(video.videoWidth * scale);
			canvas.height = Math.round(video.videoHeight * scale);
			canvas
				.getContext("2d")
				?.drawImage(video, 0, 0, canvas.width, canvas.height);
			const blob = await new Promise<Blob | null>((res) =>
				canvas.toBlob(res, "image/jpeg", JPEG_QUALITY),
			);
			if (!blob) {
				addLog(`Task #${taskId}: could not encode the photo`, "error");
				await postStatus("error", taskId, "encode failed");
				return false;
			}

			await postStatus("uploading", taskId);
			const form = new FormData();
			form.append("file", blob, `phone_${taskId}.jpg`);
			try {
				const res = await fetch(`/api/upload?task_id=${taskId}`, {
					method: "POST",
					body: form,
				});
				if (!res.ok) throw new Error(`upload failed (${res.status})`);
			} catch (e) {
				const msg = e instanceof Error ? e.message : "upload failed";
				addLog(`Task #${taskId}: ${msg}`, "error");
				await postStatus("error", taskId, msg);
				return false;
			}
			await postStatus("uploaded", taskId);
			setShots((n) => n + 1);
			addLog(
				`Task #${taskId}: photo sent (${Math.round(blob.size / 1024)} KB)`,
				"ok",
			);
			return true;
		},
		[addLog],
	);

	// ── Commands from the server, same JSON the board firmware parsed ──
	// biome-ignore lint/suspicious/noExplicitAny: MQTT payloads are arbitrary JSON, checked field by field
	const handleCommand = useCallback(
		async (_topic: string, cmd: Record<string, any>) => {
			const type = cmd.type;
			if (type === "ping") {
				addLog("Ping received", "ok");
				setFlash(true);
				setTimeout(() => setFlash(false), 600);
				await postStatus("ping_received");
				return;
			}
			if (type !== "capture_now" && type !== "capture_sequence") {
				if (type) addLog(`Ignored "${type}" (not supported on a phone)`);
				return;
			}
			if (busyRef.current) {
				addLog(`Busy, skipped "${type}"`, "error");
				return;
			}
			busyRef.current = true;
			try {
				if (type === "capture_now") {
					const taskId = Number(cmd.task_id) || 0;
					addLog(`Capture requested (task #${taskId})`);
					await captureAndUpload(taskId);
				} else {
					// Same contract as the firmware: delays_ms are offsets from
					// now, and photo i uses task_id + i.
					const base = Number(cmd.task_id) || 0;
					const delays: number[] = (
						Array.isArray(cmd.delays_ms) ? cmd.delays_ms : []
					)
						.slice(0, MAX_SEQUENCE)
						.map((d: unknown) => Math.max(0, Number(d) || 0));
					addLog(`Sequence of ${delays.length} requested (from task #${base})`);
					const start = Date.now();
					for (let i = 0; i < delays.length; i++) {
						const wait = start + delays[i] - Date.now();
						if (wait > 0) await new Promise((r) => setTimeout(r, wait));
						await captureAndUpload(base + i);
					}
				}
			} finally {
				busyRef.current = false;
			}
		},
		[addLog, captureAndUpload],
	);

	const { connectionStatus } = useMQTT([COMMAND_TOPIC], handleCommand);

	async function manualShot() {
		if (busyRef.current) return;
		busyRef.current = true;
		try {
			// task_id 0: the server assigns a fresh one, as for the board's button
			await captureAndUpload(0);
		} finally {
			busyRef.current = false;
		}
	}

	const live = connectionStatus === "connected" && cameraReady;

	return (
		<main className={styles.page}>
			<header className={styles.header}>
				<span className={styles.title}>Hesperus camera</span>
				<span className={live ? styles.live : styles.offline}>
					{live
						? "Listening"
						: connectionStatus === "connected"
							? "No camera"
							: "Connecting…"}
				</span>
			</header>

			<div className={styles.viewport}>
				{/* biome-ignore lint/a11y/useMediaCaption: live camera preview, no audio */}
				<video
					ref={videoRef}
					className={styles.video}
					playsInline
					muted
					autoPlay
				/>
				{flash && <div className={styles.flash} />}
				{cameraError && <div className={styles.cameraError}>{cameraError}</div>}
			</div>
			<canvas ref={canvasRef} hidden />

			<div className={styles.controls}>
				<button
					type="button"
					className={styles.shoot}
					onClick={manualShot}
					disabled={!cameraReady}
				>
					Take photo
				</button>
				<button
					type="button"
					className={styles.flip}
					onClick={() =>
						setFacing((f) => (f === "environment" ? "user" : "environment"))
					}
				>
					Flip
				</button>
			</div>

			<p className={styles.hint}>
				Keep this page open with the screen on. The agent's &quot;capture&quot;
				commands take a photo here. {shots} sent this session.
			</p>

			<ol className={styles.log}>
				{log.map((l, i) => (
					// biome-ignore lint/suspicious/noArrayIndexKey: append-only display log
					<li key={i} className={styles[l.kind]}>
						<span className={styles.at}>{l.at}</span> {l.text}
					</li>
				))}
			</ol>
		</main>
	);
}
