"use client";

import Link from "next/link";
import { formatUptime, getStatusVariant, rssiLabel } from "@/lib/format";
import type { BoardTelemetry, RssiVariant } from "@/lib/types";
import buttons from "../common/buttons.module.css";
import StatusIndicator from "../common/StatusIndicator";
import statusVariantStyles from "../common/statusVariant.module.css";
import { ArrowRightIcon } from "../icons/Icons";
import styles from "./BoardCard.module.css";

const RSSI_CLASS: Record<RssiVariant, string> = {
	good: styles.rssiGood,
	fair: styles.rssiFair,
	weak: styles.rssiWeak,
};

export default function BoardCard({
	board,
	imageCount,
	imageCountError,
	imageCountLoading,
	now,
}: {
	board: BoardTelemetry;
	imageCount: number | null;
	imageCountError: string | null;
	imageCountLoading: boolean;
	now: number;
}) {
	const seenAgo = board.lastSeen
		? Math.floor((now - board.lastSeen) / 1000)
		: null;
	const wifi = board.wifiRssi != null ? rssiLabel(board.wifiRssi) : null;
	const statusVariant = getStatusVariant(board.status);

	return (
		<div className={styles.boardCard}>
			<div className={styles.boardHeader}>
				<div className={styles.boardNameGroup}>
					<div className={styles.boardIcon}>
						<svg
							width="18"
							height="18"
							viewBox="0 0 24 24"
							fill="none"
							stroke="currentColor"
							aria-hidden="true"
							strokeWidth="2"
							strokeLinecap="round"
							strokeLinejoin="round"
							style={{ color: "var(--accent)" }}
						>
							<rect x="4" y="4" width="16" height="16" rx="2" />
							<rect x="9" y="9" width="6" height="6" />
							<path d="M15 2v2M15 20v2M2 15h2M20 15h2M9 2v2M9 20v2M2 9h2M20 9h2" />
						</svg>
					</div>
					<div>
						<div className={styles.boardName}>{board.name}</div>
						<div className={styles.boardId}>{board.id}</div>
					</div>
				</div>
				<StatusIndicator
					state={board.isOnline ? "online" : "offline"}
					label={board.isOnline ? "Online" : "Offline"}
				/>
			</div>

			<div className={styles.statsGrid}>
				<div className={styles.statItem}>
					<span className={styles.statLabel}>Firmware</span>
					<span className={styles.statValue}>
						{board.firmware ? `v${board.firmware}` : "\u2014"}
					</span>
				</div>
				<div className={styles.statItem}>
					<span className={styles.statLabel}>Images</span>
					{imageCountError ? (
						<span
							className={styles.statValueError}
							title={`Failed to load: ${imageCountError}`}
						>
							{"\u2014"}
						</span>
					) : imageCountLoading && imageCount === null ? (
						<span className={styles.statValue}>{"\u2026"}</span>
					) : (
						<span className={`${styles.statValue} ${styles.highlight}`}>
							{imageCount ?? 0}
						</span>
					)}
				</div>
				<div className={styles.statItem}>
					<span className={styles.statLabel}>Uptime</span>
					<span className={styles.statValue}>
						{board.uptimeSeconds != null
							? formatUptime(board.uptimeSeconds)
							: "\u2014"}
					</span>
				</div>
				<div className={styles.statItem}>
					<span className={styles.statLabel}>WiFi</span>
					<span
						className={`${styles.statValue} ${wifi ? RSSI_CLASS[wifi.variant] : ""}`}
					>
						{wifi ? `${wifi.text} (${board.wifiRssi}dBm)` : "\u2014"}
					</span>
				</div>
			</div>

			<div className={styles.telemetryBox}>
				<div className={styles.telemetryRow}>
					<span>State</span>
					<span
						className={`${styles.telemetryVal} ${statusVariantStyles[statusVariant]}`}
					>
						{board.status}
					</span>
				</div>
				<div className={styles.telemetryRow}>
					<span>Last Seen</span>
					<span className={styles.telemetryVal}>
						{seenAgo !== null ? `${seenAgo}s ago` : "Never"}
					</span>
				</div>
			</div>

			<div className={styles.boardActions}>
				<Link
					href={`/board/${board.id}`}
					className={`${buttons.btn} ${buttons.btnPrimary} ${buttons.btnExplore}`}
				>
					Open Dashboard
					<ArrowRightIcon size={14} className={buttons.arrow} />
				</Link>
			</div>
		</div>
	);
}
