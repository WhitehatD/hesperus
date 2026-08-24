// Shared domain types for the Hesperus dashboard. Single source of truth —
// previously these shapes were re-declared inline (and inconsistently typed
// as `any`) in both app/page.tsx and app/board/[id]/page.tsx.

export type ConnectionState = "online" | "sleeping" | "offline";

// Single source of truth for the board's power mode, derived from the server
// board snapshot (state/lp_mode) + live heartbeats. "unknown" = not yet synced.
export type PowerMode = "active" | "ps_rest" | "sleep" | "unknown";

export type LogLevel =
	| "info"
	| "success"
	| "warning"
	| "error"
	| "mqtt"
	| "camera"
	| "upload"
	| "ota"
	| "system";

export interface LogEntry {
	id: number;
	time: string;
	level: LogLevel;
	tag: string;
	text: string;
	meta?: string;
}

export interface BoardState {
	firmware: string | null;
	lastSeen: number | null;
	status: string;
	lastImageSize: number | null;
	lastLatencyMs: number | null;
	connection: ConnectionState;
}

export interface ImageAnalysis {
	objective: string;
	findings: string;
	recommendation: string;
	model: string;
	inferenceMs: number;
}

export interface ImageCapture {
	taskId: number;
	/** Defaults to "stm32" when the server omits board_id (single-board
	 * deployments) — matches the firmware/server's own fallback. */
	boardId: string;
	filename: string;
	url: string;
	date: string;
	timestamp: number;
	isNew: boolean;
	analysis?: ImageAnalysis;
}

export interface ScheduleTask {
	id: number;
	time: string;
	objective: string;
	action: string;
	order: number;
	completed_at: string | null;
}

export interface Schedule {
	id: number;
	name: string;
	description: string;
	is_active: boolean;
	tasks: ScheduleTask[];
}

/** Draft state for the schedule-edit modal — a task here has no id/action/
 * order/completed_at yet, only what the form actually edits. */
export interface EditingSchedule {
	id: number;
	name: string;
	description: string;
	tasks: Array<{ time: string; objective: string }>;
}

export interface TaskLiveStatus {
	status: string;
	updatedAt: number;
}

export interface EnergyTotals {
	windows: number;
	totalWindowMs: number;
	totalPsRestMs: number;
	totalCaptureMs: number;
	lastWindowMs: number;
	lastUpdate: number | null;
}

export interface UploadProgress {
	taskId: number;
	bytesSent: number;
	bytesTotal: number;
	percent: number;
	updatedAt: number;
}

export interface ActionFeedbackState {
	type: "success" | "error";
	message: string;
}

/** Fleet-page per-board telemetry, aggregated purely from MQTT (see
 * hooks/useMQTT.ts's useBoardTracker). */
export interface BoardTelemetry {
	id: string;
	name: string;
	firmware: string | null;
	lastSeen: number | null;
	status: string;
	captures: number;
	lastImageSize: number | null;
	lastLatencyMs: number | null;
	isOnline: boolean;
	uptimeSeconds: number | null;
	wifiRssi: number | null;
}

export type StatusVariant = "error" | "active" | "ota" | "idle";

export type RssiVariant = "good" | "fair" | "weak";
