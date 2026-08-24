"use client";

import type { UploadProgress } from "@/lib/types";
import styles from "./UploadProgressBar.module.css";

export default function UploadProgressBar({
	progress,
}: {
	progress: UploadProgress;
}) {
	const pct = Math.min(100, Math.max(0, progress.percent));
	return (
		<div
			className={styles.wrap}
			title={`Task ${progress.taskId}: ${(progress.bytesSent / 1024).toFixed(1)} / ${(progress.bytesTotal / 1024).toFixed(1)} KB`}
		>
			<div className={styles.track}>
				<div className={styles.fill} style={{ width: `${pct}%` }} />
			</div>
			<span className={styles.label}>
				Uploading task {progress.taskId} — {progress.percent}% (
				{(progress.bytesSent / 1024).toFixed(1)} /{" "}
				{(progress.bytesTotal / 1024).toFixed(1)} KB)
			</span>
		</div>
	);
}
