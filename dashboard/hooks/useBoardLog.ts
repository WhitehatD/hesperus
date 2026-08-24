"use client";

import { useCallback, useRef, useState } from "react";
import type { LogEntry, LogLevel } from "@/lib/types";

const MAX_LOGS = 500;

/** The board detail page's always-visible console log — a capped ring
 * buffer of {level, tag, text, meta} entries fed by MQTT/firmware events. */
export function useBoardLog() {
	const [logs, setLogs] = useState<LogEntry[]>([]);
	const logIdRef = useRef(0);

	const addLog = useCallback(
		(level: LogLevel, tag: string, text: string, meta?: string) => {
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
			setLogs((prev) => [entry, ...prev].slice(0, MAX_LOGS));
		},
		[],
	);

	const clearLogs = useCallback(() => setLogs([]), []);

	return { logs, addLog, clearLogs };
}
