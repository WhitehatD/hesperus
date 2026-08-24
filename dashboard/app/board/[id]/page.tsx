"use client";

import { use, useCallback, useEffect, useRef, useState } from "react";
import AgentChat from "@/components/agent/AgentChat";
import BoardHeader from "@/components/board/BoardHeader";
import ConsoleLog from "@/components/board/ConsoleLog";
import EnergyPanel from "@/components/board/EnergyPanel";
import GalleryPanel from "@/components/board/GalleryPanel";
import Lightbox from "@/components/board/Lightbox";
import ScheduleEditModal from "@/components/board/ScheduleEditModal";
import SchedulesPanel from "@/components/board/SchedulesPanel";
import UploadProgressBar from "@/components/board/UploadProgressBar";
import ConfirmDialog from "@/components/common/ConfirmDialog";
import { useActionFeedback } from "@/hooks/useActionFeedback";
import { useBoardLog } from "@/hooks/useBoardLog";
import { useBoardSnapshot } from "@/hooks/useBoardSnapshot";
import { useEnergy } from "@/hooks/useEnergy";
import { useGallery } from "@/hooks/useGallery";
import { useMQTT } from "@/hooks/useMQTT";
import { useSchedules } from "@/hooks/useSchedules";
import {
	activateSchedule as apiActivateSchedule,
	deactivateSchedule as apiDeactivateSchedule,
	deleteImage as apiDeleteImage,
	deleteSchedule as apiDeleteSchedule,
	saveSchedule as apiSaveSchedule,
	postCapture,
	postEnergyReset,
	postPing,
	postPowerMode,
	postSetupMode,
	postSleepMode,
} from "@/lib/api-client";
import { parseFirmwareLog } from "@/lib/firmware-log";
import type {
	EditingSchedule,
	ImageCapture,
	PowerMode,
	UploadProgress,
} from "@/lib/types";
import styles from "./page.module.css";

export default function BoardPage({
	params,
}: {
	params: Promise<{ id: string }>;
}) {
	const { id: boardId } = use(params);
	const apiBase = process.env.NEXT_PUBLIC_API_URL || "";

	const [selectedImage, setSelectedImage] = useState<ImageCapture | null>(null);
	const [activeTab, setActiveTab] = useState<
		"gallery" | "schedules" | "energy"
	>("gallery");
	const [editingSchedule, setEditingSchedule] =
		useState<EditingSchedule | null>(null);
	const [actionLoading, setActionLoading] = useState<string | null>(null);
	const [uploadProgress, setUploadProgress] = useState<UploadProgress | null>(
		null,
	);
	const [confirmState, setConfirmState] = useState<{
		title: string;
		message: string;
		onConfirm: () => void;
	} | null>(null);

	const { logs, addLog, clearLogs } = useBoardLog();
	const { feedback, showFeedback } = useActionFeedback();
	const snapshot = useBoardSnapshot(apiBase);
	const gallery = useGallery(apiBase, boardId);
	const schedules = useSchedules(apiBase);
	const energy = useEnergy(apiBase);

	const handleMessage = useCallback(
		// biome-ignore lint/suspicious/noExplicitAny: MQTT payloads are arbitrary JSON, shape varies per topic (see useMQTT.ts's MessageHandler)
		(topic: string, data: Record<string, any>, sourceBoardId: string) => {
			// Dashboard image notification
			if (topic === "dashboard/images/new") {
				gallery.refetch();
				schedules.refetch();
				if (data.filename) gallery.flashJustArrived(data.filename);
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
				gallery.applyAnalysis(data.filename, {
					objective: data.objective || "",
					findings: data.findings || "",
					recommendation: data.recommendation || "",
					model: data.model || "",
					inferenceMs: data.inference_ms || 0,
				});
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
					schedules.applyMqttUpdate(data.schedules);
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
				schedules.setTaskStatus(data.task_id, data.status);
			}

			snapshot.applyHeartbeat({ firmware: data.firmware, status: data.status });
			// Board going to sleep
			if (data.status === "sleep" || data.status === "cycle_complete") {
				snapshot.markSleeping();
			}
			snapshot.applyHeartbeatPowerMode(data);

			// Produce detailed log from board status
			const status = data.status;
			if (!status) {
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
				energy.applyWindow(winMs, psMs, capMs);
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
					addLog("info", "SCHED", "Executing scheduled task", taskMeta);
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
					// Per-chunk progress (firmware publishes on every chunk send).
					// Only log once per task (on the 0%->first-chunk transition) so
					// the log panel doesn't get flooded with a line per chunk; the
					// progress bar itself updates on every message.
					if (
						typeof data.bytes_total === "number" &&
						typeof data.bytes_sent === "number" &&
						typeof data.task_id === "number"
					) {
						setUploadProgress((prev) => {
							if (!prev || prev.taskId !== data.task_id) {
								addLog(
									"upload",
									"HTTP",
									"Uploading image to server...",
									taskMeta,
								);
							}
							return {
								taskId: data.task_id,
								bytesSent: data.bytes_sent,
								bytesTotal: data.bytes_total,
								percent:
									typeof data.progress === "number"
										? data.progress
										: Math.round(
												(data.bytes_sent / Math.max(1, data.bytes_total)) * 100,
											),
								updatedAt: Date.now(),
							};
						});
					} else {
						addLog("upload", "HTTP", "Uploading image to server...", taskMeta);
					}
					break;
				case "uploaded":
					setUploadProgress(null);
					addLog(
						"success",
						"HTTP",
						`Upload complete — ${data.bytes ? `${(data.bytes / 1024).toFixed(0)} KB` : "OK"}`,
						data.latency_ms ? `${data.latency_ms}ms` : taskMeta,
					);
					break;
				case "error":
					setUploadProgress(null);
					addLog("error", "ERR", "error", data.reason || taskMeta);
					break;
				case "cycle_complete":
					setUploadProgress(null);
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
		[boardId, gallery, schedules, snapshot, energy, addLog],
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

	// Close lightbox on Escape
	useEffect(() => {
		const handleEsc = (e: KeyboardEvent) => {
			if (e.key === "Escape") setSelectedImage(null);
		};
		window.addEventListener("keydown", handleEsc);
		return () => window.removeEventListener("keydown", handleEsc);
	}, []);

	const doDeleteImage = async (img: ImageCapture) => {
		try {
			await apiDeleteImage(apiBase, img.date, img.filename);
			gallery.removeImage(img.filename);
			setSelectedImage(null);
		} catch (err) {
			addLog("error", "HTTP", `Delete failed: ${err}`);
			showFeedback("gallery", "error", `Delete failed: ${err}`);
		}
	};

	const handleCapture = async () => {
		setActionLoading("capture");
		addLog("info", "CMD", "Sending capture command...");
		try {
			const data = await postCapture(apiBase);
			addLog("success", "CMD", "Capture command sent", `task #${data.task_id}`);
			showFeedback("capture", "success", "Capture command sent");
		} catch (err) {
			addLog("error", "CMD", `Capture failed: ${err}`);
			showFeedback("capture", "error", `Capture failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handlePing = async () => {
		setActionLoading("ping");
		addLog("info", "CMD", "Sending ping to board...");
		try {
			await postPing(apiBase);
			addLog("success", "CMD", "Ping sent — LED sequence triggered");
			showFeedback("ping", "success", "Ping sent");
		} catch (err) {
			addLog("error", "CMD", `Ping failed: ${err}`);
			showFeedback("ping", "error", `Ping failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleSetup = async () => {
		setActionLoading("setup");
		addLog("warning", "CMD", "Entering WiFi setup mode...");
		try {
			await postSetupMode(apiBase);
			addLog(
				"success",
				"CMD",
				"Setup mode activated — board starting AP at 192.168.10.1",
			);
			showFeedback("setup", "success", "Setup mode activated");
		} catch (err) {
			addLog("error", "CMD", `Setup mode failed: ${err}`);
			showFeedback("setup", "error", `Setup mode failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleRefresh = () => {
		addLog("info", "CMD", "Refreshing images & schedules...");
		gallery.refetch();
		schedules.refetch();
	};

	// Single mutually-exclusive power-mode switch. Active/PS-REST/Sleep map to
	// the board's command set; the firmware enforces exclusivity (enabling one
	// clears the others), and the board snapshot poll confirms the real
	// resulting state.
	const handleSetPowerMode = async (mode: Exclude<PowerMode, "unknown">) => {
		if (mode === snapshot.powerMode) return;
		const prev = snapshot.powerMode;
		setActionLoading("power");
		snapshot.setOptimisticPowerMode(mode); // optimistic — snapshot poll reconciles
		try {
			if (mode === "ps_rest") {
				await postPowerMode(apiBase, "ps_rest");
				addLog(
					"system",
					"PWR",
					"Power mode → PS-REST (energy telemetry begins)",
				);
			} else if (mode === "sleep") {
				await postSleepMode(apiBase, true);
				addLog(
					"system",
					"PWR",
					"Power mode → Sleep (deep dormant; wakes at next schedule or B3)",
				);
			} else {
				// Active: clear both light-sleep (PS-REST) and deep-sleep.
				await postPowerMode(apiBase, "off");
				await postSleepMode(apiBase, false);
				addLog(
					"system",
					"PWR",
					"Power mode → Active (always awake & connected)",
				);
			}
			showFeedback("power", "success", `Power mode → ${mode}`);
		} catch (err) {
			snapshot.setOptimisticPowerMode(prev); // revert on failure
			addLog("error", "PWR", `Power mode change failed: ${err}`);
			showFeedback("power", "error", `Power mode change failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleEnergyReset = async () => {
		setActionLoading("energy-reset");
		try {
			const body = await postEnergyReset(apiBase);
			energy.reset();
			addLog(
				"system",
				"PWR",
				`Energy windows reset (${body.deleted ?? 0} cleared) — fresh run`,
			);
			showFeedback("energy-reset", "success", "Energy run reset");
		} catch (err) {
			addLog("error", "PWR", `Energy reset failed: ${err}`);
			showFeedback("energy-reset", "error", `Reset failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleActivateSchedule = async (scheduleId: number) => {
		setActionLoading(`activate-${scheduleId}`);
		try {
			await apiActivateSchedule(apiBase, scheduleId);
			addLog("success", "SCHED", "Schedule activated", `id=${scheduleId}`);
			showFeedback(`schedule-${scheduleId}`, "success", "Activated");
			schedules.refetch();
		} catch (err) {
			addLog("error", "SCHED", `Activate failed: ${err}`);
			showFeedback(
				`schedule-${scheduleId}`,
				"error",
				`Activate failed: ${err}`,
			);
		} finally {
			setActionLoading(null);
		}
	};

	const handleDeactivateSchedule = async (scheduleId: number) => {
		setActionLoading(`deactivate-${scheduleId}`);
		try {
			await apiDeactivateSchedule(apiBase, scheduleId);
			addLog("info", "SCHED", "Schedule deactivated", `id=${scheduleId}`);
			showFeedback(`schedule-${scheduleId}`, "success", "Deactivated");
			schedules.refetch();
		} catch (err) {
			addLog("error", "SCHED", `Deactivate failed: ${err}`);
			showFeedback(
				`schedule-${scheduleId}`,
				"error",
				`Deactivate failed: ${err}`,
			);
		} finally {
			setActionLoading(null);
		}
	};

	const doDeleteSchedule = async (scheduleId: number, name: string) => {
		setActionLoading(`delete-${scheduleId}`);
		try {
			await apiDeleteSchedule(apiBase, scheduleId);
			schedules.removeSchedule(scheduleId);
			addLog("info", "SCHED", `Schedule "${name}" deleted`);
		} catch (err) {
			addLog("error", "SCHED", `Delete failed: ${err}`);
			showFeedback(`schedule-${scheduleId}`, "error", `Delete failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	const handleSaveSchedule = async () => {
		if (!editingSchedule) return;
		setActionLoading("save-schedule");
		try {
			await apiSaveSchedule(apiBase, editingSchedule.id, {
				name: editingSchedule.name,
				description: editingSchedule.description,
				tasks: editingSchedule.tasks.map((t, i) => ({
					time: t.time,
					objective: t.objective,
					action: "CAPTURE_IMAGE",
					order: i,
				})),
			});
			addLog("success", "SCHED", `Schedule "${editingSchedule.name}" updated`);
			showFeedback("save-schedule", "success", "Schedule saved");
			setEditingSchedule(null);
			schedules.refetch();
		} catch (err) {
			addLog("error", "SCHED", `Save failed: ${err}`);
			showFeedback("save-schedule", "error", `Save failed: ${err}`);
		} finally {
			setActionLoading(null);
		}
	};

	return (
		<div className={`app-container ${styles.agentLayout}`}>
			<BoardHeader
				boardId={boardId}
				firmware={snapshot.firmware}
				boardStatus={snapshot.status}
				connection={snapshot.connection}
				powerMode={snapshot.powerMode}
				imagesCount={gallery.images.length}
				imagesLoading={gallery.loading && gallery.images.length === 0}
				imagesError={gallery.error}
				onRetryImages={gallery.refetch}
				snapshotError={snapshot.error}
				onRetrySnapshot={snapshot.refetch}
				mqttConnected={connectionStatus === "connected"}
				actionLoading={actionLoading}
				onCapture={handleCapture}
				onPing={handlePing}
				onSetup={handleSetup}
				onRefresh={handleRefresh}
				feedback={feedback}
			/>

			{uploadProgress && <UploadProgressBar progress={uploadProgress} />}

			<main className={styles.agentMain}>
				<AgentChat boardId={boardId} apiBase={apiBase} fullSize />
			</main>

			<aside className={styles.agentPanel}>
				<div className={styles.panelUpper}>
					<div className={styles.panelTabs}>
						<button
							type="button"
							className={`${styles.panelTab} ${activeTab === "gallery" ? styles.active : ""}`}
							onClick={() => setActiveTab("gallery")}
						>
							Gallery
							{gallery.images.length > 0 && (
								<span className={styles.tabCount}>{gallery.images.length}</span>
							)}
						</button>
						<button
							type="button"
							className={`${styles.panelTab} ${activeTab === "schedules" ? styles.active : ""}`}
							onClick={() => setActiveTab("schedules")}
						>
							Schedules
							{schedules.schedules.length > 0 && (
								<span className={styles.tabCount}>
									{schedules.schedules.length}
								</span>
							)}
						</button>
						<button
							type="button"
							className={`${styles.panelTab} ${activeTab === "energy" ? styles.active : ""}`}
							onClick={() => setActiveTab("energy")}
						>
							Power &amp; Energy
							{energy.energy.windows > 0 && (
								<span className={styles.tabCount}>{energy.energy.windows}</span>
							)}
						</button>
					</div>

					<div className={styles.panelContent}>
						{activeTab === "gallery" && (
							<GalleryPanel
								images={gallery.images}
								loading={gallery.loading}
								error={gallery.error}
								onRetry={gallery.refetch}
								justArrivedFilename={gallery.justArrivedFilename}
								onSelect={setSelectedImage}
								feedback={feedback.gallery}
							/>
						)}

						{activeTab === "schedules" && (
							<SchedulesPanel
								schedules={schedules.schedules}
								loading={schedules.loading}
								error={schedules.error}
								onRetry={schedules.refetch}
								taskStatuses={schedules.taskStatuses}
								actionLoading={actionLoading}
								feedback={feedback}
								onActivate={handleActivateSchedule}
								onDeactivate={handleDeactivateSchedule}
								onEdit={setEditingSchedule}
								onDelete={(scheduleId, name) =>
									setConfirmState({
										title: "Delete Schedule",
										message: `Delete schedule "${name}"? This cannot be undone.`,
										onConfirm: () => doDeleteSchedule(scheduleId, name),
									})
								}
							/>
						)}

						{activeTab === "energy" && (
							<EnergyPanel
								energy={energy.energy}
								powerMode={snapshot.powerMode}
								hasSchedule={schedules.schedules.length > 0}
								actionLoading={actionLoading}
								onSetPowerMode={handleSetPowerMode}
								onReset={handleEnergyReset}
								energyError={energy.error}
								onRetryEnergy={energy.refetch}
								feedbackPower={feedback.power}
								feedbackReset={feedback["energy-reset"]}
							/>
						)}
					</div>
				</div>

				<ConsoleLog logs={logs} onClear={clearLogs} />
			</aside>

			{selectedImage && (
				<Lightbox
					image={selectedImage}
					onClose={() => setSelectedImage(null)}
					onDelete={(img) =>
						setConfirmState({
							title: "Delete Capture",
							message: "Delete this capture permanently?",
							onConfirm: () => doDeleteImage(img),
						})
					}
				/>
			)}

			{editingSchedule && (
				<ScheduleEditModal
					draft={editingSchedule}
					onChange={(updater) =>
						setEditingSchedule((prev) => (prev ? updater(prev) : prev))
					}
					onSave={handleSaveSchedule}
					onCancel={() => setEditingSchedule(null)}
					saving={actionLoading === "save-schedule"}
					feedback={feedback["save-schedule"]}
				/>
			)}

			<ConfirmDialog
				open={confirmState !== null}
				title={confirmState?.title ?? ""}
				message={confirmState?.message ?? ""}
				confirmLabel="Delete"
				onConfirm={() => {
					confirmState?.onConfirm();
					setConfirmState(null);
				}}
				onCancel={() => setConfirmState(null)}
			/>
		</div>
	);
}
