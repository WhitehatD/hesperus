"use client";

import type { LogEntry, LogLevel } from "@/lib/types";
import buttons from "../common/buttons.module.css";
import styles from "./ConsoleLog.module.css";

const LEVEL_CLASS: Record<LogLevel, string> = {
	info: styles.logInfo,
	success: styles.logSuccess,
	warning: styles.logWarning,
	error: styles.logError,
	mqtt: styles.logMqtt,
	camera: styles.logCamera,
	upload: styles.logUpload,
	ota: styles.logOta,
	system: styles.logSystem,
};

export default function ConsoleLog({
	logs,
	onClear,
}: {
	logs: LogEntry[];
	onClear: () => void;
}) {
	return (
		<div className={styles.panelConsole}>
			<div className={styles.consoleHeader}>
				<span className={styles.consoleTitle}>Console</span>
				<span className={styles.consoleCount}>{logs.length}</span>
				<button type="button" className={buttons.btnText} onClick={onClear}>
					Clear
				</button>
			</div>
			<div className={styles.consoleBody}>
				{logs.length === 0 ? (
					<div className={styles.terminalEmpty}>
						Waiting for board telemetry...
					</div>
				) : (
					logs.map((log) => (
						<div
							key={log.id}
							className={`${styles.logEntry} ${LEVEL_CLASS[log.level]}`}
						>
							<span className={styles.logTime}>{log.time}</span>
							<span className={styles.logTag}>{log.tag}</span>
							<span className={styles.logText}>{log.text}</span>
							{log.meta && <span className={styles.logMeta}>{log.meta}</span>}
						</div>
					))
				)}
			</div>
		</div>
	);
}
