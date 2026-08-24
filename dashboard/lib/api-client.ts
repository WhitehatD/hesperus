// Typed API client for the FastAPI backend. Every network call the
// dashboard makes to the server (as opposed to the Next.js-local
// app/api/agent/* SSE proxy routes, which stay in AgentChat) goes through
// here — previously ~20 call sites each hand-rolled their own
// `fetch(...)` + `if (!res.ok) throw ...` + ad-hoc response shape.
//
// `apiBase` is the same value every caller already used:
// `process.env.NEXT_PUBLIC_API_URL || ""` — empty string resolves through
// next.config.mjs's rewrite to the FastAPI server; a real URL is used in
// local dev setups that talk to FastAPI directly.

import type { EnergyTotals, ImageCapture, Schedule } from "./types";

export class ApiError extends Error {
	status: number;
	constructor(message: string, status: number) {
		super(message);
		this.name = "ApiError";
		this.status = status;
	}
}

async function request<T>(
	path: string,
	init?: RequestInit,
	parse: "json" | "none" = "json",
): Promise<T> {
	const res = await fetch(path, init);
	if (!res.ok) {
		const body = await res.text().catch(() => "");
		throw new ApiError(body || `Request failed (${res.status})`, res.status);
	}
	if (parse === "none") return undefined as T;
	return (await res.json()) as T;
}

/* ── Deserialization: raw (snake_case, server-shaped) -> domain types ── */

// biome-ignore lint/suspicious/noExplicitAny: raw server JSON, shape-checked field by field below
function mapImageCapture(apiBase: string, img: any): ImageCapture {
	return {
		taskId: img.task_id,
		boardId: img.board_id || "stm32",
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
	};
}

/* ── Reads ── */

export async function getImages(
	apiBase: string,
	boardId?: string,
): Promise<ImageCapture[]> {
	const qs = boardId ? `?board_id=${boardId}` : "";
	// biome-ignore lint/suspicious/noExplicitAny: raw server JSON
	const data = await request<{ images: any[] }>(`${apiBase}/api/images${qs}`);
	return (data.images ?? []).map((img) => mapImageCapture(apiBase, img));
}

export async function getSchedules(apiBase: string): Promise<Schedule[]> {
	const data = await request<{ schedules?: Schedule[] } | Schedule[]>(
		`${apiBase}/api/schedules`,
	);
	if (Array.isArray(data)) return data;
	return data.schedules ?? [];
}

export interface EnergySeed {
	windows: number;
	total_window_ms: number;
	total_ps_rest_ms: number;
	total_capture_ms: number;
}

export function getEnergySeed(apiBase: string): Promise<EnergySeed> {
	return request<EnergySeed>(`${apiBase}/api/benchmark/energy`);
}

export function energySeedToTotals(seed: EnergySeed): EnergyTotals {
	return {
		windows: seed.windows ?? 0,
		totalWindowMs: seed.total_window_ms ?? 0,
		totalPsRestMs: seed.total_ps_rest_ms ?? 0,
		totalCaptureMs: seed.total_capture_ms ?? 0,
		lastWindowMs: 0,
		lastUpdate: seed.windows > 0 ? Date.now() : null,
	};
}

export interface BoardSnapshot {
	state: string;
	sleep_mode?: boolean;
	lp_mode?: "ps_rest" | "normal" | null;
	firmware?: string | null;
	last_seen?: string | null;
}

export function getBoardState(apiBase: string): Promise<BoardSnapshot> {
	return request<BoardSnapshot>(`${apiBase}/api/board/state`);
}

/* ── Writes ── */

export function deleteImage(
	apiBase: string,
	date: string,
	filename: string,
): Promise<void> {
	return request(
		`${apiBase}/api/images/${date}/${filename}`,
		{ method: "DELETE" },
		"none",
	);
}

export function postCapture(apiBase: string): Promise<{ task_id: number }> {
	return request(`${apiBase}/api/capture`, { method: "POST" });
}

export function postPing(apiBase: string): Promise<void> {
	return request(`${apiBase}/api/ping`, { method: "POST" }, "none");
}

export function postSetupMode(apiBase: string): Promise<void> {
	return request(`${apiBase}/api/erase-wifi`, { method: "POST" }, "none");
}

export function postPowerMode(
	apiBase: string,
	mode: "ps_rest" | "off",
): Promise<void> {
	return request(
		`${apiBase}/api/low-power-mode?mode=${mode}`,
		{ method: "POST" },
		"none",
	);
}

export function postSleepMode(
	apiBase: string,
	enabled: boolean,
): Promise<void> {
	return request(
		`${apiBase}/api/schedules/sleep-mode?enabled=${enabled}`,
		{ method: "POST" },
		"none",
	);
}

export function postEnergyReset(
	apiBase: string,
): Promise<{ deleted?: number }> {
	return request(`${apiBase}/api/benchmark/energy/reset`, { method: "POST" });
}

export function activateSchedule(
	apiBase: string,
	scheduleId: number,
): Promise<void> {
	return request(
		`${apiBase}/api/schedules/${scheduleId}/activate`,
		{ method: "POST" },
		"none",
	);
}

export function deactivateSchedule(
	apiBase: string,
	scheduleId: number,
): Promise<void> {
	return request(
		`${apiBase}/api/schedules/${scheduleId}/deactivate`,
		{ method: "POST" },
		"none",
	);
}

export function deleteSchedule(
	apiBase: string,
	scheduleId: number,
): Promise<void> {
	return request(
		`${apiBase}/api/schedules/${scheduleId}`,
		{ method: "DELETE" },
		"none",
	);
}

export interface ScheduleSavePayload {
	name: string;
	description: string;
	tasks: Array<{
		time: string;
		objective: string;
		action: string;
		order: number;
	}>;
}

export function saveSchedule(
	apiBase: string,
	scheduleId: number,
	payload: ScheduleSavePayload,
): Promise<void> {
	return request(
		`${apiBase}/api/schedules/${scheduleId}`,
		{
			method: "PUT",
			headers: { "Content-Type": "application/json" },
			body: JSON.stringify(payload),
		},
		"none",
	);
}
