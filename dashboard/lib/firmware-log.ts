// Firmware serial-log line parser — relocated verbatim from
// app/board/[id]/page.tsx. Parses lines shaped like:
//   [  1234ms] [INFO] [WIFI] Connected to AP
// as published over MQTT on device/{id}/logs.

import type { LogLevel } from "./types";

const FIRMWARE_LOG_RE = /^\[\s*(\d+)ms\]\s*\[(\w+)\s*\]\s*\[([^\]]+)\]\s*(.*)$/;

const FW_LEVEL_MAP: Record<string, LogLevel> = {
	DBG: "mqtt",
	INFO: "info",
	OK: "success",
	WARN: "warning",
	ERR: "error",
};

export function parseFirmwareLog(raw: string): {
	level: LogLevel;
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
