// Pure formatting helpers shared across the dashboard. Consolidates what
// used to be copy-pasted between app/page.tsx and app/board/[id]/page.tsx
// (formatUptime/getStatusClass existed twice, verbatim, in both files).
//
// Functions that used to return a raw global CSS class name now return a
// semantic variant instead — with CSS Modules a class name is scoped to its
// own component file, so the mapping from variant -> module class lives
// next to the component that owns the style, not here.

import type { RssiVariant, StatusVariant } from "./types";

/** Board uptime, in seconds -> compact display string. */
export function formatUptime(seconds: number): string {
	if (seconds < 60) return `${seconds}s`;
	if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
	const h = Math.floor(seconds / 3600);
	const m = Math.floor((seconds % 3600) / 60);
	return m > 0 ? `${h}h ${m}m` : `${h}h`;
}

/** Energy-window / generic duration, in milliseconds -> compact display string.
 * Deliberately distinct from formatUptime: this always shows the finer unit
 * alongside the coarser one (e.g. "2h 5m", "3m 12s") instead of dropping it,
 * which matters for the energy panel's sub-hour precision. */
export function fmtMs(ms: number): string {
	const totalSec = Math.floor(ms / 1000);
	const h = Math.floor(totalSec / 3600);
	const m = Math.floor((totalSec % 3600) / 60);
	const s = totalSec % 60;
	if (h > 0) return `${h}h ${m}m`;
	if (m > 0) return `${m}m ${s}s`;
	return `${s}s`;
}

export function rssiLabel(rssi: number): {
	text: string;
	variant: RssiVariant;
} {
	if (rssi >= -50) return { text: "Excellent", variant: "good" };
	if (rssi >= -65) return { text: "Good", variant: "good" };
	if (rssi >= -75) return { text: "Fair", variant: "fair" };
	return { text: "Weak", variant: "weak" };
}

export function getStatusVariant(status: string): StatusVariant {
	if (status.includes("error") || status.includes("failed")) return "error";
	if (status.includes("captur") || status.includes("upload")) return "active";
	if (status.includes("ota") || status.includes("ping")) return "ota";
	return "idle";
}
