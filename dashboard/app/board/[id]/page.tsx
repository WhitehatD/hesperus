"use client";

import Link from "next/link";
import { use, useCallback, useEffect, useRef, useState } from "react";
import AgentChat from "../../components/AgentChat";
import { useMQTT } from "../../hooks/useMQTT";

type ConnectionState = "online" | "sleeping" | "offline";
// Single source of truth for the board's power mode, derived from the server
// board snapshot (state/lp_mode) + live heartbeats. "unknown" = not yet synced.
type PowerMode = "active" | "ps_rest" | "sleep" | "unknown";

interface BoardState {
	firmware: string | null;
	lastSeen: number | null;
	status: string;
	captures: number;
	lastImageSize: number | null;
	lastLatencyMs: number | null;
	connection: ConnectionState;
	logs: LogEntry[];
}

interface LogEntry {
	id: number;
	time: string;
	level:
		| "info"
		| "success"
		| "warning"
		| "error"
		| "mqtt"
		| "camera"
		| "upload"
		| "ota"
		| "system";
	tag: string;
	text: string;
	meta?: string;
}

interface ImageCapture {
	taskId: number;
	filename: string;
	url: string;
	date: string;
	timestamp: number;
	isNew: boolean;
	analysis?: {
		objective: string;
		findings: string;
		recommendation: string;
		model: string;
		inferenceMs: number;
	};
}

const MAX_LOGS = 500;

export default function BoardPage({
	params,
}: {
	params: Promise<{ id: string }>;
}) {
	const { id: boardId } = use(params);
	const apiBase = process.env.NEXT_PUBLIC_API_URL || "";

	const [board, setBoard] = useState<BoardState>({
		firmware: null,
		lastSeen: null,
		status: "idle",
		captures: 0,
		lastImageSize: null,
		lastLatencyMs: null,
		connection: "sleeping",
		logs: [],
	});
	const [images, setImages] = useState<ImageCapture[]>([]);
	const [selectedImage, setSelectedImage] = useState<ImageCapture | null>(null);
	const [activeTab, setActiveTab] = useState<
		"gallery" | "schedules" | "energy"
	>("gallery");
	const [schedules, setSchedules] = useState<any[]>([]);
	const [taskStatuses, setTaskStatuses] = useState<
		Record<number, { status: string; updatedAt: number }>
	>({});
	const [actionLoading, setActionLoading] = useState<string | null>(null);
	const [powerMode, setPowerMode] = useState<PowerMode>("unknown");

	interface EnergyTotals {
		windows: number;
		totalWindowMs: number;
		totalPsRestMs: number;
		totalCaptureMs: number;
		lastWindowMs: number;
		lastUpdate: number | null;
	}
	const [energy, setEnergy] = useState<EnergyTotals>({
		windows: 0,
		totalWindowMs: 0,
		totalPsRestMs: 0,
		totalCaptureMs: 0,
		lastWindowMs: 0,
		lastUpdate: null,
	});
	const [editingSchedule, setEditingSchedule] = useState<{
		id: number;
		name: string;
		description: string;
		tasks: Array<{ time: string; objective: string }>;
	} | null>(null);

	const logIdRef = useRef(0);
	const addLog = useCallback(
		(level: LogEntry["level"], tag: string, text: string, meta?: string) => {
			logIdRef.current += 1;
			const entry: LogEntry = {
				id: logIdRef.current,
				time: new Date().toLocaleTimeString("en-GB", {
					hour: "2-digit",
					minute: "2-digit",
					second: "2-digit",
				}),
				level,
				tag,
				text,
				meta,
			};
			setBoard((prev) => ({
				...prev,
				logs: [entry, ...prev.logs].slice(0, MAX_LOGS),
			}));
		},
		[],
	);

	const fetchImages = useCallback(() => {
		fetch(`${apiBase}/api/images?board_id=${boardId}`)
			.then((res) => {
				if (!res.ok) throw new Error(res.statusText);
				return res.json();
			})
			.then((data) => {
				if (data.images) {
					setImages(
						data.images.map((img: any) => ({
							taskId: img.task_id,
							filename: img.filename,
							url: `${apiBase}${img.url}`,
							date: img.date,
							timestamp: img.timestamp,
							isNew: false,
							analysis: img.analysis
								? {
										objective: img.analysis.objective ?? "",
										findings: img.analysis.findings ?? "",
										recommendation: img.analysis.recommendation ?? "",
										model: img.analysis.model ?? "",
										inferenceMs: img.analysis.inference_ms ?? 0,
									}
								: undefined,
						})),
					);
				}
			})
			.catch(console.error);
	}, [apiBase, boardId]);

	const fetchSchedules = useCallback(() => {
		fetch(`${apiBase}/api/schedules`)
			.then((r) => r.json())
			.then((data) => {
				const fresh = data.schedules ?? data ?? [];
				setSchedules((prev) => {
					// Merge: keep completed_at from previous state if HTTP response is stale
					if (prev.length === 0) return fresh;
					const prevMap = new Map(
						prev.flatMap((s: any) =>
							(s.tasks || []).map((t: any) => [t.id, t.completed_at]),
						),
					);
					return fresh.map((s: any) => ({
						...s,
						tasks: (s.tasks || []).map((t: any) => ({
							...t,
							completed_at: t.completed_at || prevMap.get(t.id) || null,
						})),
					}));
				});
			})
			.catch(() => {});
	}, [apiBase]);

	useEffect(() => {
		fetchImages();
		fetchSchedules();
	}, [fetchImages, fetchSchedules]);

	// Seed cumulative energy totals from the server on mount
	useEffect(() => {
		fetch(`${apiBase}/api/benchmark/energy`)
			.then((r) => {
				if (!r.ok) return null;
				return r.json();
			})
			.then((data) => {
				if (!data) return;
				setEnergy({
					windows: data.windows ?? 0,
					totalWindowMs: data.total_window_ms ?? 0,
					totalPsRestMs: data.total_ps_rest_ms ?? 0,
					totalCaptureMs: data.total_capture_ms ?? 0,
					lastWindowMs: 0,
					lastUpdate: data.windows > 0 ? Date.now() : null,
				});
			})
			.catch(() => {});
	}, [apiBase]);

	// Hydrate board state from the server snapshot — the single source of truth
	// for power mode + connection. Runs on mount AND polls every 5 s, so a page
	// refresh reflects real board state immediately instead of blanking until the
	// next MQTT heartbeat (sparse in PS-REST — the board sleeps ~97% of the time).
	useEffect(() => {
		let cancelled = false;
		const applySnapshot = async () => {
			try {
				const r = await fetch(`${apiBase}/api/board/state`);
				if (!r.ok) return;
				const snap = await r.json();
				if (cancelled || !snap) return;

				// Derive power mode from the board's reported state. Sleep (deep
				// dormant, or armed-but-awake when no schedule) wins, then PS-REST,
				// then Active. lp_mode and sleep_mode are exclusive on the board.
				if (snap.state === "deep_dormant" || snap.sleep_mode === true) {
					setPowerMode("sleep");
				} else if (snap.lp_mode === "ps_rest") {
					setPowerMode("ps_rest");
				} else if (snap.lp_mode === "normal") {
					setPowerMode("active");
				} // else: leave as-is (unknown until the board reports)

				setBoard((prev) => {
					const lastSeenMs = snap.last_seen
						? new Date(snap.last_seen).getTime()
						: prev.lastSeen;
					const ageMs =
						lastSeenMs != null
							? Date.now() - lastSeenMs
							: Number.POSITIVE_INFINITY;
					let connection: ConnectionState = prev.connection;
					if (snap.state === "deep_dormant") {
						connection = "sleeping";
					} else if (snap.state === "online") {
						connection =
							ageMs < 15000
								? "online"
								: ageMs < 120000
									? "sleeping"
									: "offline";
					}
					return {
						...prev,
						firmware: snap.firmware ?? prev.firmware,
						lastSeen: lastSeenMs,
						connection,
					};
				});
			} catch {
				/* transient — next poll retries */
			}
		};
		applySnapshot();
		const interval = setInterval(applySnapshot, 5000);
		return () => {
			cancelled = true;
			clearInterval(interval);
		};
	}, [apiBase]);

	// Poll schedules as fallback (MQTT push is primary, poll catches reconnects)
	useEffect(() => {
		const interval = setInterval(fetchSchedules, 30000);
		return () => clearInterval(interval);
	}, [fetchSchedules]);

	const handleMessage = useCallback(
		(topic: string, data: Record<string, any>, sourceBoardId: string) => {
			// Dashboard image notification
			if (topic === "dashboard/images/new") {
				fetchImages();
				fetchSchedules();
				addLog(
					"upload",
					"IMG",
					`New image: ${data.filename}`,
					`task #${data.task_id}`,
				);
				return;
			}

			// AI analysis result
			if (topic === "dashboard/analysis/new") {
				setImages((prev) =>
					prev.map((img) =>
						img.filename === data.filename
							? {
									...img,
									analysis: {
										objective: data.objective || "",
										findings: data.findings || "",
										recommendation: data.recommendation || "",
										model: data.model || "",
										inferenceMs: data.inference_ms || 0,
									},
								}
							: img,
					),
				);
				addLog(
					"info",
					"AI",
					`Vision analysis: ${data.model || "?"} · ${data.inference_ms || 0}ms`,
					`task #${data.task_id}`,
				);
				return;
			}

			// Real-time schedule/task updates
			if (topic === "dashboard/schedules/updated") {
				if (data.schedules) {
					setSchedules((prev) => {
						if (prev.length === 0) return data.schedules;
						const prevMap = new Map(
							prev.flatMap((s: any) =>
								(s.tasks || []).map((t: any) => [t.id, t.completed_at]),
							),
						);
						return data.schedules.map((s: any) => ({
							...s,
							tasks: (s.tasks || []).map((t: any) => ({
								...t,
								completed_at: t.completed_at || prevMap.get(t.id) || null,
							})),
						}));
					});
					addLog(
						"mqtt",
						"SCHED",
						`Schedule update via MQTT (${data.schedules.length} schedules)`,
					);
				}
				return;
			}

			// Board firmware logs (raw text from MQTT)
			if (topic === `device/${boardId}/logs`) {
				const raw: string = data.raw || data.message || "";
				const parsed = parseFirmwareLog(raw);
				addLog(parsed.level, parsed.tag, parsed.text, parsed.meta);
				return;
			}

			// Server-side logs
			if (topic === "dashboard/logs") {
				addLog(
					"system",
					"SRV",
					data.text || "Server log",
					data.level || undefined,
				);
				return;
			}

			// Board telemetry — filter by board ID
			if (sourceBoardId !== boardId) return;

			// Track per-task execution status for schedule display
			if (data.task_id && data.status) {
				setTaskStatuses((prev) => ({
					...prev,
					[data.task_id]: {
						status: data.status,
						updatedAt: Date.now(),
					},
				}));
			}

			setBoard((prev) => {
				const update = {
					...prev,
					connection: "online" as ConnectionState,
					lastSeen: Date.now(),
				};
				if (data.firmware) update.firmware = data.firmware;
				if (data.status) update.status = data.status;
				if (data.status === "captured" || data.status === "uploaded") {
					update.captures += 1;
					if (data.size) update.lastImageSize = data.size;
					if (data.latency_ms) update.lastLatencyMs = data.latency_ms;
				}
				// Board going to sleep
				if (data.status === "sleep" || data.status === "cycle_complete") {
					update.connection = "sleeping";
				}
				return update;
			});

			// Reflect the board's reported power mode live (board = source of truth).
			// Heartbeat carries lp_mode + sleep_mode (0/1); they are exclusive.
			if (data.status === "deep_dormant" || data.sleep_mode === 1) {
				setPowerMode("sleep");
			} else if (data.status === "awake") {
				setPowerMode("active");
			} else if (data.lp_mode === "ps_rest") {
				setPowerMode("ps_rest");
			} else if (data.lp_mode === "normal") {
				setPowerMode("active");
			}

			// Produce detailed log from board status
			const status = data.status;
			if (!status) {
				// Heartbeat without status change
				addLog(
					"mqtt",
					"HB",
					"Heartbeat received",
					data.firmware ? `fw ${data.firmware}` : undefined,
				);
				return;
			}

			// Energy phase-timer window (RQ3)
			if (status === "energy") {
				const winMs: number = data.window_ms ?? 0;
				const psMs: number = data.ps_rest_ms ?? 0;
				const capMs: number = data.capture_ms ?? 0;
				setEnergy((prev) => ({
					windows: prev.windows + 1,
					totalWindowMs: prev.totalWindowMs + winMs,
					totalPsRestMs: prev.totalPsRestMs + psMs,
					totalCaptureMs: prev.totalCaptureMs + capMs,
					lastWindowMs: winMs,
					lastUpdate: Date.now(),
				}));
				const psPct = winMs > 0 ? ((psMs / winMs) * 100).toFixed(1) : "0.0";
				const capPct = winMs > 0 ? ((capMs / winMs) * 100).toFixed(2) : "0.00";
				addLog(
					"info",
					"ENERGY",
					`Window ${(winMs / 1000).toFixed(0)}s — ps_rest ${psPct}% capture ${capPct}%`,
					`ps=${psMs}ms cap=${capMs}ms`,
				);
				return;
			}

			const taskMeta = data.task_id ? `task #${data.task_id}` : undefined;

			switch (status) {
				case "idle":
					addLog("info", "BOARD", "Board idle — awaiting commands");
					break;
				case "executing":
					addLog("info", "SCHED", `Executing scheduled task`, taskMeta);
					break;
				case "camera_init":
					addLog(
						"camera",
						"CAM",
						"Camera cold start — initializing sensor",
						taskMeta,
					);
					break;
				case "capturing":
					addLog("camera", "CAM", "Capturing frame", taskMeta);
					break;
				case "captured":
					addLog(
						"success",
						"CAM",
						`Frame captured — ${data.size ? `${(data.size / 1024).toFixed(0)} KB` : "?"}`,
						taskMeta,
					);
					break;
				case "uploading":
					addLog("upload", "HTTP", "Uploading image to server...", taskMeta);
					break;
				case "uploaded":
					addLog(
						"success",
						"HTTP",
						`Upload complete — ${data.bytes ? `${(data.bytes / 1024).toFixed(0)} KB` : "OK"}`,
						data.latency_ms ? `${data.latency_ms}ms` : taskMeta,
					);
					break;
				case "cycle_complete":
					addLog("success", "SCHED", "All scheduled tasks completed for today");
					break;
				case "ota_checking":
					addLog("ota", "OTA", "Checking for firmware update...");
					break;
				case "ota_downloading":
					addLog(
						"ota",
						"OTA",
						`Downloading firmware v${data.version || "?"}`,
						data.progress ? `${data.progress}%` : undefined,
					);
					break;
				case "ota_complete":
					addLog(
						"success",
						"OTA",
						`OTA complete — rebooting to v${data.version || "?"}`,
					);
					break;
				case "sleep":
					addLog(
						"system",
						"PWR",
						"Entering STOP2 sleep mode",
						data.wake_time || undefined,
					);
					break;
				case "wake":
					addLog("system", "PWR", "Woke from sleep — reconnecting");
					break;
				default:
					if (status.includes("error") || status.includes("fail")) {
						addLog("error", "ERR", `${status}`, data.reason || taskMeta);
					} else {
						addLog("info", "BOARD", status, taskMeta);
					}
			}
		},
		[boardId, fetchImages, fetchSchedules, addLog],
	);

	const topics = [
		`device/${boardId}/status`,
		`device/${boardId}/logs`,
		"dashboard/images/new",
		"dashboard/analysis/new",
		"dashboard/logs",
		"dashboard/schedules/updated",
	];
	const { connectionStatus } = useMQTT(topics, handleMessage);

	// Transition: online → sleeping (15s) → offline (2min)
	useEffect(() => {
		const interval = setInterval(() => {
			setBoard((prev) => {
				if (!prev.lastSeen) return prev;
				const elapsed = Date.now() - prev.lastSeen;
				if (prev.connection === "online" && elapsed > 15000) {
					return { ...prev, connection: "sleeping" };
				}
				if (prev.connection === "sleeping" && elapsed > 120000) {
					return { ...prev, connection: "offline" };
				}
				return prev;
			});
		}, 5000);
		return () => clearInterval(interval);
	}, []);

	// Close lightbox on Escape
	useEffect(() => {
		const handleEsc = (e: KeyboardEvent) => {
			if (e.key === "Escape") setSelectedImage(null);
		};
		window.addEventListener("keydown", handleEsc);
		return () => window.removeEventListener("keydown", handleEsc);
	}, []);

	const deleteImage = async (img: ImageCapture) => {
		if (!confirm("Delete this capture permanently?")) return;
		try {
			await fetch(`${apiBase}/api/images/${img.date}/${img.filename}`, {
				method: "DELETE",
			});
			setImages((prev) => prev.filter((i) => i.filename !== img.filename));
			setSelectedImage(null);
		} catch (err) {
			addLog("error", "HTTP", `Delete failed: ${err}`);
		}
	};

	const handleCapture = async () => {
		setActionLoading("capture");
		addLog("info", "CMD", "Sending capture command...");
		try {
			const res = await fetch(`${apiBase}/api/capture`, { method: "POST" });
			if (!res.ok) throw new Error(await res.text());
			const data = await res.json();
			addLog("success", "CMD", "Capture command sent", `task #${data.task_id}`);
		} catch (err) {
			addLog("error", "CMD", `Capture failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handlePing = async () => {
		setActionLoading("ping");
		addLog("info", "CMD", "Sending ping to board...");
		try {
			const res = await fetch(`${apiBase}/api/ping`, { method: "POST" });
			if (!res.ok) throw new Error(await res.text());
			addLog("success", "CMD", "Ping sent — LED sequence triggered");
		} catch (err) {
			addLog("error", "CMD", `Ping failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleSetup = async () => {
		setActionLoading("setup");
		addLog("warning", "CMD", "Entering WiFi setup mode...");
		try {
			const res = await fetch(`${apiBase}/api/erase-wifi`, { method: "POST" });
			if (!res.ok) throw new Error(await res.text());
			addLog(
				"success",
				"CMD",
				"Setup mode activated — board starting AP at 192.168.10.1",
			);
		} catch (err) {
			addLog("error", "CMD", `Setup mode failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleRefresh = () => {
		addLog("info", "CMD", "Refreshing images & schedules...");
		fetchImages();
		fetchSchedules();
	};

	// Single mutually-exclusive power-mode switch. Active/PS-REST/Sleep map to the
	// board's command set; the firmware enforces exclusivity (enabling one clears
	// the others), and the board snapshot poll confirms the real resulting state.
	const handleSetPowerMode = async (mode: Exclude<PowerMode, "unknown">) => {
		if (mode === powerMode) return;
		const prev = powerMode;
		setActionLoading("power");
		setPowerMode(mode); // optimistic — snapshot poll reconciles
		const post = async (path: string) => {
			const res = await fetch(`${apiBase}${path}`, { method: "POST" });
			if (!res.ok) throw new Error(await res.text());
		};
		try {
			if (mode === "ps_rest") {
				await post(`/api/low-power-mode?mode=ps_rest`);
				addLog(
					"system",
					"PWR",
					"Power mode → PS-REST (energy telemetry begins)",
				);
			} else if (mode === "sleep") {
				await post(`/api/schedules/sleep-mode?enabled=true`);
				addLog(
					"system",
					"PWR",
					"Power mode → Sleep (deep dormant; wakes at next schedule or B3)",
				);
			} else {
				// Active: clear both light-sleep (PS-REST) and deep-sleep.
				await post(`/api/low-power-mode?mode=off`);
				await post(`/api/schedules/sleep-mode?enabled=false`);
				addLog(
					"system",
					"PWR",
					"Power mode → Active (always awake & connected)",
				);
			}
		} catch (err) {
			setPowerMode(prev); // revert on failure
			addLog("error", "PWR", `Power mode change failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleEnergyReset = async () => {
		setActionLoading("energy-reset");
		try {
			const res = await fetch(`${apiBase}/api/benchmark/energy/reset`, {
				method: "POST",
			});
			if (!res.ok) throw new Error(await res.text());
			const body = await res.json();
			setEnergy({
				windows: 0,
				totalWindowMs: 0,
				totalPsRestMs: 0,
				totalCaptureMs: 0,
				lastWindowMs: 0,
				lastUpdate: null,
			});
			addLog(
				"system",
				"PWR",
				`Energy windows reset (${body.deleted ?? 0} cleared) — fresh run`,
			);
		} catch (err) {
			addLog("error", "PWR", `Energy reset failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleActivateSchedule = async (scheduleId: number) => {
		setActionLoading(`activate-${scheduleId}`);
		try {
			const res = await fetch(
				`${apiBase}/api/schedules/${scheduleId}/activate`,
				{ method: "POST" },
			);
			if (!res.ok) throw new Error(await res.text());
			addLog("success", "SCHED", `Schedule activated`, `id=${scheduleId}`);
			fetchSchedules();
		} catch (err) {
			addLog("error", "SCHED", `Activate failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleDeactivateSchedule = async (scheduleId: number) => {
		setActionLoading(`deactivate-${scheduleId}`);
		try {
			const res = await fetch(
				`${apiBase}/api/schedules/${scheduleId}/deactivate`,
				{ method: "POST" },
			);
			if (!res.ok) throw new Error(await res.text());
			addLog("info", "SCHED", `Schedule deactivated`, `id=${scheduleId}`);
			fetchSchedules();
		} catch (err) {
			addLog("error", "SCHED", `Deactivate failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleDeleteSchedule = async (scheduleId: number, name: string) => {
		if (!confirm(`Delete schedule "${name}"? This cannot be undone.`)) return;
		setActionLoading(`delete-${scheduleId}`);
		try {
			const res = await fetch(`${apiBase}/api/schedules/${scheduleId}`, {
				method: "DELETE",
			});
			if (!res.ok) throw new Error(await res.text());
			setSchedules((prev) => prev.filter((s: any) => s.id !== scheduleId));
			addLog("info", "SCHED", `Schedule "${name}" deleted`);
		} catch (err) {
			addLog("error", "SCHED", `Delete failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleSaveSchedule = async () => {
		if (!editingSchedule) return;
		setActionLoading("save-schedule");
		try {
			const res = await fetch(
				`${apiBase}/api/schedules/${editingSchedule.id}`,
				{
					method: "PUT",
					headers: { "Content-Type": "application/json" },
					body: JSON.stringify({
						name: editingSchedule.name,
						description: editingSchedule.description,
						tasks: editingSchedule.tasks.map((t, i) => ({
							time: t.time,
							objective: t.objective,
							action: "CAPTURE_IMAGE",
							order: i,
						})),
					}),
				},
			);
			if (!res.ok) throw new Error(await res.text());
			addLog("success", "SCHED", `Schedule "${editingSchedule.name}" updated`);
			setEditingSchedule(null);
			fetchSchedules();
		} catch (err) {
			addLog("error", "SCHED", `Save failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const sortedImages = [...images].sort((a, b) => b.timestamp - a.timestamp);

	return (
		<div className="app-container agent-layout">
			{/* Header */}
			<header className="agent-header-bar">
				<div className="agent-header-left">
					<Link href="/" className="btn-back">
						&larr;
					</Link>
					<h1 className="agent-header-title">Monitoring Agent</h1>
					<span className="agent-header-node">{boardId}</span>
				</div>
				<div className="agent-header-actions">
					<button
						className="btn-action accent"
						onClick={handleCapture}
						disabled={actionLoading !== null}
					>
						{actionLoading === "capture" ? "Sending..." : "Capture"}
					</button>
					<button
						className="btn-action"
						onClick={handlePing}
						disabled={actionLoading !== null}
					>
						{actionLoading === "ping" ? "Sending..." : "Ping"}
					</button>
					<button
						className="btn-action"
						onClick={handleSetup}
						disabled={actionLoading !== null}
					>
						{actionLoading === "setup" ? "Sending..." : "Setup"}
					</button>
					<button className="btn-action" onClick={handleRefresh}>
						Refresh
					</button>
				</div>
				<div className="agent-header-stats">
					<div className={`status-indicator ${board.connection}`}>
						<div className="dot" />
						{board.connection === "online"
							? "Online"
							: board.connection === "sleeping"
								? "Standby"
								: "Offline"}
					</div>
					<span
						className={`header-chip power-chip power-${powerMode}`}
						title="Current power mode — change it in the Power tab"
					>
						{powerMode === "ps_rest"
							? "PS-REST"
							: powerMode === "sleep"
								? "Sleep"
								: powerMode === "active"
									? "Active"
									: "Power \u2026"}
					</span>
					<span className="header-chip">
						FW {board.firmware ? `v${board.firmware}` : "\u2014"}
					</span>
					<span className="header-chip">
						<span className={getStatusClass(board.status)}>{board.status}</span>
					</span>
					<span className="header-chip highlight">{board.captures} caps</span>
					<div className="connection-badge">
						<div
							className={`connection-dot ${connectionStatus === "connected" ? "connected" : "disconnected"}`}
						/>
						MQTT
					</div>
				</div>
			</header>

			{/* Left: Agent Chat */}
			<main className="agent-main">
				<AgentChat boardId={boardId} apiBase={apiBase} fullSize />
			</main>

			{/* Right: Gallery/Schedules (top) + Console (bottom, always visible) */}
			<aside className="agent-panel">
				<div className="panel-upper">
					<div className="panel-tabs">
						<button
							className={`panel-tab ${activeTab === "gallery" ? "active" : ""}`}
							onClick={() => setActiveTab("gallery")}
						>
							Gallery
							{images.length > 0 && (
								<span className="tab-count">{images.length}</span>
							)}
						</button>
						<button
							className={`panel-tab ${activeTab === "schedules" ? "active" : ""}`}
							onClick={() => setActiveTab("schedules")}
						>
							Schedules
							{schedules.length > 0 && (
								<span className="tab-count">{schedules.length}</span>
							)}
						</button>
						<button
							className={`panel-tab ${activeTab === "energy" ? "active" : ""}`}
							onClick={() => setActiveTab("energy")}
						>
							Energy
							{energy.windows > 0 && (
								<span className="tab-count">{energy.windows}</span>
							)}
						</button>
					</div>

					<div className="panel-content">
						{activeTab === "gallery" && (
							<div className="sidebar-gallery">
								{sortedImages.length === 0 ? (
									<div className="empty-state-sm">
										No captures yet. Ask the agent to take a picture.
									</div>
								) : (
									sortedImages.map((img) => (
										<div
											key={img.filename}
											className="sidebar-image-card"
											onClick={() => setSelectedImage(img)}
										>
											{img.analysis && <div className="analysis-indicator" />}
											<div className="sidebar-image-wrapper">
												<img
													src={img.url}
													alt={`Task ${img.taskId}`}
													loading="lazy"
												/>
											</div>
											<div className="sidebar-image-meta">
												<span>#{img.taskId}</span>
												<span className="image-time">
													{new Date(img.timestamp * 1000).toLocaleTimeString()}
												</span>
											</div>
										</div>
									))
								)}
							</div>
						)}

						{activeTab === "schedules" && (
							<div className="schedules-list">
								{schedules.length === 0 ? (
									<div className="empty-state-sm">
										No schedules yet. Ask the agent to create one.
									</div>
								) : (
									schedules.map((sched: any) => {
										const tasks = sched.tasks || [];
										const done = tasks.filter(
											(t: any) => t.completed_at,
										).length;
										const running = tasks.filter(
											(t: any) =>
												!t.completed_at &&
												taskStatuses[t.id] &&
												[
													"executing",
													"camera_init",
													"capturing",
													"captured",
													"uploading",
												].includes(taskStatuses[t.id].status),
										).length;
										const allDone = tasks.length > 0 && done === tasks.length;
										return (
											<div
												key={sched.id}
												className={`schedule-card ${sched.is_active ? "active" : ""} ${allDone ? "completed" : ""}`}
											>
												<div className="schedule-header">
													<span className="schedule-name">{sched.name}</span>
													{allDone ? (
														<span className="schedule-badge done">
															Completed
														</span>
													) : running > 0 ? (
														<span className="schedule-badge running">
															Running
														</span>
													) : sched.is_active ? (
														<span className="schedule-badge">Active</span>
													) : (
														<span className="schedule-badge inactive">
															Inactive
														</span>
													)}
												</div>
												<div className="schedule-progress">
													<div
														className="schedule-progress-bar"
														style={{
															width: tasks.length
																? `${(done / tasks.length) * 100}%`
																: "0%",
														}}
													/>
												</div>
												<div className="schedule-tasks">
													{tasks.map((task: any) => {
														const live = taskStatuses[task.id];
														const isRunning =
															!task.completed_at &&
															live &&
															[
																"executing",
																"camera_init",
																"capturing",
																"captured",
																"uploading",
															].includes(live.status);
														const isFailed =
															!task.completed_at &&
															live &&
															(live.status.includes("error") ||
																live.status.includes("fail"));
														return (
															<div
																key={task.id}
																className={`schedule-task ${task.completed_at ? "task-done" : ""} ${isRunning ? "task-running" : ""} ${isFailed ? "task-failed" : ""}`}
															>
																<span className="schedule-check">
																	{task.completed_at ? (
																		"\u2713"
																	) : isRunning ? (
																		<span
																			className="task-spinner"
																			title={live.status}
																		/>
																	) : isFailed ? (
																		"\u2717"
																	) : (
																		"\u25CB"
																	)}
																</span>
																<span className="schedule-time">
																	{task.time}
																</span>
																<span className="schedule-obj">
																	{task.objective || task.action}
																</span>
																{isRunning && (
																	<span className="task-status-label">
																		{live.status.replace("_", " ")}
																	</span>
																)}
															</div>
														);
													})}
												</div>
												<div className="schedule-actions">
													{!sched.is_active && !allDone && (
														<button
															className="btn-sched-action accent"
															disabled={actionLoading !== null}
															onClick={() => handleActivateSchedule(sched.id)}
														>
															{actionLoading === `activate-${sched.id}`
																? "…"
																: "Activate"}
														</button>
													)}
													{sched.is_active && (
														<button
															className="btn-sched-action"
															disabled={actionLoading !== null}
															onClick={() => handleDeactivateSchedule(sched.id)}
														>
															{actionLoading === `deactivate-${sched.id}`
																? "…"
																: "Deactivate"}
														</button>
													)}
													<button
														className="btn-sched-action"
														disabled={actionLoading !== null}
														onClick={() =>
															setEditingSchedule({
																id: sched.id,
																name: sched.name,
																description: sched.description || "",
																tasks: (sched.tasks || []).map((t: any) => ({
																	time: t.time,
																	objective: t.objective || t.action || "",
																})),
															})
														}
													>
														Edit
													</button>
													<button
														className="btn-sched-action danger"
														disabled={actionLoading !== null}
														onClick={() =>
															handleDeleteSchedule(sched.id, sched.name)
														}
													>
														{actionLoading === `delete-${sched.id}`
															? "…"
															: "Delete"}
													</button>
												</div>
											</div>
										);
									})
								)}
							</div>
						)}

						{activeTab === "energy" && (
							<EnergyPanel
								energy={energy}
								powerMode={powerMode}
								hasSchedule={schedules.length > 0}
								actionLoading={actionLoading}
								onSetPowerMode={handleSetPowerMode}
								onReset={handleEnergyReset}
							/>
						)}
					</div>
				</div>

				{/* Console — always visible */}
				<div className="panel-console">
					<div className="console-header">
						<span className="console-title">Console</span>
						<span className="console-count">{board.logs.length}</span>
						<button
							className="btn-text"
							onClick={() => setBoard((prev) => ({ ...prev, logs: [] }))}
						>
							Clear
						</button>
					</div>
					<div className="console-body">
						{board.logs.length === 0 ? (
							<div className="terminal-empty">
								Waiting for board telemetry...
							</div>
						) : (
							board.logs.map((log) => (
								<div key={log.id} className={`log-entry log-${log.level}`}>
									<span className="log-time">{log.time}</span>
									<span className="log-tag">{log.tag}</span>
									<span className="log-text">{log.text}</span>
									{log.meta && <span className="log-meta">{log.meta}</span>}
								</div>
							))
						)}
					</div>
				</div>
			</aside>

			{/* Lightbox */}
			{selectedImage && (
				<div
					className="lightbox-overlay"
					onClick={() => setSelectedImage(null)}
				>
					<div
						className="lightbox-content"
						onClick={(e) => e.stopPropagation()}
					>
						<button
							className="lightbox-close"
							onClick={() => setSelectedImage(null)}
						>
							&times;
						</button>
						<img
							src={selectedImage.url}
							alt="Full size capture"
							className="lightbox-image"
						/>
						<div className="lightbox-footer">
							<div>
								<h3>Task #{selectedImage.taskId}</h3>
								<p>
									{new Date(selectedImage.timestamp * 1000).toLocaleString()}
								</p>
							</div>
							<div className="lightbox-actions">
								<a
									href={selectedImage.url}
									download
									className="btn btn-secondary"
								>
									Download
								</a>
								<button
									className="btn btn-danger"
									onClick={() => deleteImage(selectedImage)}
								>
									Delete
								</button>
							</div>
						</div>
						{selectedImage.analysis && (
							<div className="analysis-panel">
								<div className="analysis-header">
									<span className="analysis-meta">
										{selectedImage.analysis.model} &middot;{" "}
										{selectedImage.analysis.inferenceMs.toFixed(0)}ms
									</span>
								</div>
								{selectedImage.analysis.objective && (
									<div className="analysis-row">
										<span className="analysis-label">Objective</span>
										<p>{selectedImage.analysis.objective}</p>
									</div>
								)}
								<div className="analysis-row">
									<span className="analysis-label">Findings</span>
									<p>{selectedImage.analysis.findings}</p>
								</div>
								<div className="analysis-row">
									<span className="analysis-label">Recommendation</span>
									<p>{selectedImage.analysis.recommendation}</p>
								</div>
							</div>
						)}
					</div>
				</div>
			)}

			{/* Schedule Edit Modal */}
			{editingSchedule && (
				<div
					className="lightbox-overlay"
					onClick={() => setEditingSchedule(null)}
				>
					<div
						className="schedule-edit-modal"
						onClick={(e) => e.stopPropagation()}
					>
						<div className="modal-header">
							<h3>Edit Schedule</h3>
							<button
								className="lightbox-close"
								onClick={() => setEditingSchedule(null)}
							>
								&times;
							</button>
						</div>

						<div className="modal-field">
							<label className="modal-label" htmlFor="sched-edit-name">
								Name
							</label>
							<input
								id="sched-edit-name"
								className="modal-input"
								value={editingSchedule.name}
								onChange={(e) =>
									setEditingSchedule((prev) =>
										prev ? { ...prev, name: e.target.value } : prev,
									)
								}
							/>
						</div>

						<div className="modal-tasks">
							<div className="modal-tasks-header">
								<span className="modal-label">Tasks</span>
								<button
									className="btn-sched-action accent"
									onClick={() =>
										setEditingSchedule((prev) =>
											prev
												? {
														...prev,
														tasks: [
															...prev.tasks,
															{ time: "09:00", objective: "" },
														],
													}
												: prev,
										)
									}
								>
									+ Add
								</button>
							</div>
							{editingSchedule.tasks.map((task, idx) => (
								<div key={idx} className="modal-task-row">
									<input
										className="modal-input time-input"
										type="time"
										value={task.time.substring(0, 5)}
										onChange={(e) =>
											setEditingSchedule((prev) => {
												if (!prev) return prev;
												const tasks = [...prev.tasks];
												tasks[idx] = { ...tasks[idx], time: e.target.value };
												return { ...prev, tasks };
											})
										}
									/>
									<input
										className="modal-input flex-1"
										placeholder="Objective (e.g. Check if door is open)"
										value={task.objective}
										onChange={(e) =>
											setEditingSchedule((prev) => {
												if (!prev) return prev;
												const tasks = [...prev.tasks];
												tasks[idx] = {
													...tasks[idx],
													objective: e.target.value,
												};
												return { ...prev, tasks };
											})
										}
									/>
									<button
										className="btn-sched-action danger"
										onClick={() =>
											setEditingSchedule((prev) => {
												if (!prev) return prev;
												return {
													...prev,
													tasks: prev.tasks.filter((_, i) => i !== idx),
												};
											})
										}
									>
										×
									</button>
								</div>
							))}
						</div>

						<div className="modal-footer">
							<button
								className="btn btn-secondary"
								onClick={() => setEditingSchedule(null)}
							>
								Cancel
							</button>
							<button
								className="btn btn-primary"
								disabled={actionLoading === "save-schedule"}
								onClick={handleSaveSchedule}
							>
								{actionLoading === "save-schedule"
									? "Saving…"
									: "Save Schedule"}
							</button>
						</div>
					</div>
				</div>
			)}
		</div>
	);
}

// ── Energy model constants — mirrored exactly from scripts/energy_model.py ──
// Datasheet current figures (typical @ ~3.0-3.3 V)
const I_MCU_RUN_MA = 19.0; // STM32U585 Run @160MHz
const I_MCU_STOP2_UA = 2.0; // STOP2 + RTC (bench-verified ~2 uA)
const I_CAM_ACTIVE_MA = 150.0; // OV5640 active capture
const I_CAM_OFF_UA = 20.0; // OV5640 power-down
const I_WIFI_ACTIVE_MA = 230.0; // EMW3080 connected + uploading
const I_WIFI_PS_MA = 3.0; // EMW3080 802.11 power-save / DTIM-sleep
// Note: I_WIFI_OFF_UA (10.0 uA) is used by DEEP_DORMANT mode only; not in measured path
const VOLTAGE_V = 3.3;
const BATTERY_MAH = 2000.0;

function activeBurstCurrentMa(): number {
	return I_MCU_RUN_MA + I_CAM_ACTIVE_MA + I_WIFI_ACTIVE_MA;
}

function wifiPsRestCurrentMa(): number {
	return I_WIFI_PS_MA + (I_MCU_STOP2_UA + I_CAM_OFF_UA) / 1000.0;
}

function continuousDailyJ(): number {
	const iA = activeBurstCurrentMa() / 1000.0;
	return iA * VOLTAGE_V * 86400.0;
}

interface MeasuredEnergy {
	fRest: number;
	fCap: number;
	fOther: number;
	avgMa: number;
	dailyJ: number;
	batteryDays: number;
	savingsRatio: number;
}

function measuredDailyEnergy(
	totalWindowMs: number,
	totalPsRestMs: number,
	totalCaptureMs: number,
): MeasuredEnergy | null {
	if (totalWindowMs <= 0) return null;
	const fRest = totalPsRestMs / totalWindowMs;
	const fCap = totalCaptureMs / totalWindowMs;
	const fOther = Math.max(1.0 - fRest - fCap, 0.0);
	const iRest = wifiPsRestCurrentMa();
	const iCap = activeBurstCurrentMa();
	const iOther = I_MCU_RUN_MA + I_WIFI_PS_MA; // awake but not capturing (camera off)
	const avgMa = fRest * iRest + fCap * iCap + fOther * iOther;
	const dailyJ = (avgMa / 1000.0) * VOLTAGE_V * 86400.0;
	const batteryJ = (BATTERY_MAH / 1000.0) * VOLTAGE_V * 3600.0;
	const batteryDays = batteryJ / Math.max(dailyJ, 1e-9);
	const savingsRatio = continuousDailyJ() / Math.max(dailyJ, 1e-9);
	return { fRest, fCap, fOther, avgMa, dailyJ, batteryDays, savingsRatio };
}

function fmtMs(ms: number): string {
	const totalSec = Math.floor(ms / 1000);
	const h = Math.floor(totalSec / 3600);
	const m = Math.floor((totalSec % 3600) / 60);
	const s = totalSec % 60;
	if (h > 0) return `${h}h ${m}m`;
	if (m > 0) return `${m}m ${s}s`;
	return `${s}s`;
}

interface EnergyPanelProps {
	energy: {
		windows: number;
		totalWindowMs: number;
		totalPsRestMs: number;
		totalCaptureMs: number;
		lastWindowMs: number;
		lastUpdate: number | null;
	};
	powerMode: PowerMode;
	hasSchedule: boolean;
	actionLoading: string | null;
	onSetPowerMode: (mode: "active" | "ps_rest" | "sleep") => void;
	onReset: () => void;
}

const POWER_MODES = [
	{ id: "active", label: "Active", current: "~22 mA" },
	{ id: "ps_rest", label: "PS-REST", current: "~3 mA" },
	{ id: "sleep", label: "Sleep", current: "~2 µA" },
] as const;

function EnergyPanel({
	energy,
	powerMode,
	hasSchedule,
	actionLoading,
	onSetPowerMode,
	onReset,
}: EnergyPanelProps) {
	const m = measuredDailyEnergy(
		energy.totalWindowMs,
		energy.totalPsRestMs,
		energy.totalCaptureMs,
	);
	// Per-state current draws (mA) used by the model — shown inline so each
	// duty-split slice is self-explanatory.
	const iRest = wifiPsRestCurrentMa(); // STOP2 + Wi-Fi power-save
	const iCap = activeBurstCurrentMa(); // MCU + camera + Wi-Fi upload
	const iOther = I_MCU_RUN_MA + I_WIFI_PS_MA; // awake, camera off

	const modeDesc: Record<PowerMode, string> = {
		unknown: "Syncing power mode from the board\u2026",
		active:
			"Always awake & connected. Highest power — use for setup and live work.",
		ps_rest:
			"MCU sleeps in STOP2 between ~3 s polls; Wi-Fi stays connected in 802.11 power-save (~3 mA). Stays reachable. This is the RQ3 energy-measurement mode — a window is reported every ~60 s.",
		sleep:
			"Deep dormant: Wi-Fi + camera off (~2 µA). The board is UNREACHABLE and wakes only at the next scheduled capture or a physical B3 press.",
	};

	return (
		<div className="energy-panel">
			{/* Power mode — mutually exclusive segmented control */}
			<fieldset className="power-mode-control">
				<legend className="energy-label">Power mode</legend>
				<div className="power-mode-segments">
					{POWER_MODES.map((opt) => (
						<button
							key={opt.id}
							className={`power-seg${powerMode === opt.id ? " active" : ""}`}
							onClick={() => onSetPowerMode(opt.id)}
							disabled={actionLoading !== null || powerMode === opt.id}
							aria-pressed={powerMode === opt.id}
						>
							<span className="power-seg-label">{opt.label}</span>
							<span className="power-seg-current">{opt.current}</span>
						</button>
					))}
				</div>
				<div className="power-mode-desc">
					{actionLoading === "power"
						? "Switching power mode\u2026"
						: modeDesc[powerMode]}
				</div>
				{powerMode === "sleep" && !hasSchedule && (
					<div className="power-mode-warn">
						⚠ No schedule set — the board will stay dormant until you press the
						physical B3 button. Add a schedule so it wakes itself to capture.
					</div>
				)}
			</fieldset>

			{energy.windows > 0 && (
				<div className="energy-actions-row">
					<span className="energy-section-sub" style={{ margin: 0 }}>
						RQ3 energy — measured on-device while in PS-REST.
					</span>
					<button
						className="btn-action"
						onClick={onReset}
						disabled={actionLoading !== null}
						title="Clear stored energy windows and start a fresh measurement run"
					>
						{actionLoading === "energy-reset" ? "Resetting…" : "Reset run"}
					</button>
				</div>
			)}

			{energy.windows === 0 ? (
				<div className="energy-empty">
					No energy telemetry yet — enable PS-REST to start the run.
				</div>
			) : (
				<>
					{/* Summary row */}
					<div className="energy-summary-row">
						<div className="energy-stat">
							<span className="energy-stat-value">{energy.windows}</span>
							<span className="energy-stat-label">windows</span>
							<span className="energy-help">
								60&nbsp;s wall-clock windows the board has reported this run
							</span>
						</div>
						<div className="energy-stat">
							<span className="energy-stat-value">
								{fmtMs(energy.totalWindowMs)}
							</span>
							<span className="energy-stat-label">observed</span>
							<span className="energy-help">
								total real time measured (sum of all windows)
							</span>
						</div>
						<div className="energy-stat">
							<span className="energy-stat-value">
								{energy.lastUpdate
									? new Date(energy.lastUpdate).toLocaleTimeString("en-GB", {
											hour: "2-digit",
											minute: "2-digit",
											second: "2-digit",
										})
									: "\u2014"}
							</span>
							<span className="energy-stat-label">last window</span>
							<span className="energy-help">
								clock time the newest window arrived
							</span>
						</div>
					</div>

					{/* Duty split */}
					{m && (
						<>
							<div className="energy-section-title">Measured duty split</div>
							<div className="energy-section-sub">
								Share of real (wall-clock) time the board spent in each power
								state — measured on-device via the RTC, not estimated. These
								three add up to 100%.
							</div>
							<div className="energy-duty-bar">
								<div
									className="energy-duty-seg energy-duty-rest"
									style={{ width: `${m.fRest * 100}%` }}
									title={`PS-REST ${(m.fRest * 100).toFixed(1)}% — STOP2 sleep + Wi-Fi power-save, ~${iRest.toFixed(1)} mA`}
								/>
								<div
									className="energy-duty-seg energy-duty-cap"
									style={{ width: `${m.fCap * 100}%` }}
									title={`Capture ${(m.fCap * 100).toFixed(2)}% — camera + Wi-Fi upload active, ~${iCap.toFixed(0)} mA`}
								/>
								<div
									className="energy-duty-seg energy-duty-other"
									style={{ width: `${m.fOther * 100}%` }}
									title={`Other ${(m.fOther * 100).toFixed(1)}% — awake & idle, camera off, ~${iOther.toFixed(0)} mA`}
								/>
							</div>
							<div className="energy-duty-legend">
								<span className="energy-legend-item energy-legend-rest">
									PS-REST {(m.fRest * 100).toFixed(1)}%
									<span className="energy-legend-help">
										asleep (STOP2) + Wi-Fi power-save · ~{iRest.toFixed(1)} mA
									</span>
								</span>
								<span className="energy-legend-item energy-legend-cap">
									Capture {(m.fCap * 100).toFixed(2)}%
									<span className="energy-legend-help">
										camera + Wi-Fi upload · ~{iCap.toFixed(0)} mA
									</span>
								</span>
								<span className="energy-legend-item energy-legend-other">
									Other {(m.fOther * 100).toFixed(1)}%
									<span className="energy-legend-help">
										awake & idle, camera off · ~{iOther.toFixed(0)} mA
									</span>
								</span>
							</div>

							{/* Indicative projection */}
							<div className="energy-section-title">
								Indicative projection{" "}
								<span className="energy-section-note">
									(authoritative: <code>energy_model.py</code>)
								</span>
							</div>
							<div className="energy-section-sub">
								The measured duty split above, extrapolated to a full day using
								datasheet component currents at 3.3&nbsp;V. Indicative only —
								the thesis figures come from <code>energy_model.py</code>.
							</div>
							<div className="energy-projection-grid">
								<div className="energy-proj-item">
									<span className="energy-proj-value">
										{m.avgMa.toFixed(2)} mA
									</span>
									<span className="energy-proj-label">avg current</span>
									<span className="energy-help">
										duty-weighted mean draw at 3.3&nbsp;V
									</span>
								</div>
								<div className="energy-proj-item">
									<span className="energy-proj-value">
										{m.dailyJ.toFixed(1)} J
									</span>
									<span className="energy-proj-label">energy / day</span>
									<span className="energy-help">
										= avg current × 3.3&nbsp;V × 24&nbsp;h
									</span>
								</div>
								<div className="energy-proj-item">
									<span className="energy-proj-value">
										{m.batteryDays >= 30
											? `${(m.batteryDays / 30.4).toFixed(1)} mo`
											: `${m.batteryDays.toFixed(1)} d`}
									</span>
									<span className="energy-proj-label">
										battery ({BATTERY_MAH.toFixed(0)} mAh)
									</span>
									<span className="energy-help">
										projected runtime on one {BATTERY_MAH.toFixed(0)}&nbsp;mAh
										cell
									</span>
								</div>
								<div className="energy-proj-item">
									<span className="energy-proj-value energy-savings">
										{m.savingsRatio.toFixed(0)}x
									</span>
									<span className="energy-proj-label">vs continuous</span>
									<span className="energy-help">
										longer than always-on capture (no sleep)
									</span>
								</div>
							</div>
						</>
					)}
				</>
			)}
		</div>
	);
}

const FIRMWARE_LOG_RE = /^\[\s*(\d+)ms\]\s*\[(\w+)\s*\]\s*\[([^\]]+)\]\s*(.*)$/;

const FW_LEVEL_MAP: Record<string, LogEntry["level"]> = {
	DBG: "mqtt",
	INFO: "info",
	OK: "success",
	WARN: "warning",
	ERR: "error",
};

function parseFirmwareLog(raw: string): {
	level: LogEntry["level"];
	tag: string;
	text: string;
	meta?: string;
} {
	const m = FIRMWARE_LOG_RE.exec(raw);
	if (m) {
		const [, ms, level, tag, text] = m;
		return {
			level: FW_LEVEL_MAP[level] || "system",
			tag: tag.substring(0, 5),
			text: text.trim(),
			meta: `${(Number(ms) / 1000).toFixed(1)}s`,
		};
	}
	return { level: "system", tag: "FW", text: raw || "(empty)" };
}

function getStatusClass(status: string): string {
	if (status.includes("error") || status.includes("failed"))
		return "status-error";
	if (status.includes("captur") || status.includes("upload"))
		return "status-active";
	if (status.includes("ota") || status.includes("ping")) return "status-ota";
	return "status-idle";
}
