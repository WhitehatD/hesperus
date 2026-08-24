"use client";

import Link from "next/link";
import { getStatusVariant } from "@/lib/format";
import type {
	ActionFeedbackState,
	ConnectionState,
	PowerMode,
} from "@/lib/types";
import ActionFeedback from "../common/ActionFeedback";
import buttons from "../common/buttons.module.css";
import ConnectionBadge from "../common/ConnectionBadge";
import StatusIndicator from "../common/StatusIndicator";
import statusVariantStyles from "../common/statusVariant.module.css";
import { ArrowLeftIcon } from "../icons/Icons";
import styles from "./BoardHeader.module.css";

const POWER_CLASS: Record<PowerMode, string> = {
	active: styles.powerActive,
	ps_rest: styles.powerPsRest,
	sleep: styles.powerSleep,
	unknown: styles.powerUnknown,
};

const POWER_LABEL: Record<PowerMode, string> = {
	active: "Active",
	ps_rest: "PS-REST",
	sleep: "Sleep",
	unknown: "Power \u2026",
};

const CONNECTION_LABEL: Record<ConnectionState, string> = {
	online: "Online",
	sleeping: "Standby",
	offline: "Offline",
};

export interface BoardHeaderProps {
	boardId: string;
	firmware: string | null;
	boardStatus: string;
	connection: ConnectionState;
	powerMode: PowerMode;
	imagesCount: number;
	imagesLoading: boolean;
	imagesError: string | null;
	onRetryImages: () => void;
	snapshotError: string | null;
	onRetrySnapshot: () => void;
	mqttConnected: boolean;
	actionLoading: string | null;
	onCapture: () => void;
	onPing: () => void;
	onSetup: () => void;
	onRefresh: () => void;
	feedback: Record<string, ActionFeedbackState>;
}

export default function BoardHeader({
	boardId,
	firmware,
	boardStatus,
	connection,
	powerMode,
	imagesCount,
	imagesLoading,
	imagesError,
	onRetryImages,
	snapshotError,
	onRetrySnapshot,
	mqttConnected,
	actionLoading,
	onCapture,
	onPing,
	onSetup,
	onRefresh,
	feedback,
}: BoardHeaderProps) {
	const statusVariant = getStatusVariant(boardStatus);

	return (
		<header className={styles.headerBar}>
			<div className={styles.headerLeft}>
				<Link
					href="/"
					className={`${buttons.btnBack} ${styles.backBtn}`}
					title="Back to fleet"
				>
					<ArrowLeftIcon size={14} />
				</Link>
				<h1 className={styles.headerTitle}>Hesperus</h1>
				<span className={styles.headerNode}>{boardId}</span>
			</div>
			<div className={styles.headerActions}>
				<button
					type="button"
					className={`${buttons.btnAction} ${buttons.accent} ${styles.actionBtn}`}
					onClick={onCapture}
					disabled={actionLoading !== null}
				>
					{actionLoading === "capture" ? "Sending..." : "Capture"}
				</button>
				<button
					type="button"
					className={`${buttons.btnAction} ${styles.actionBtn}`}
					onClick={onPing}
					disabled={actionLoading !== null}
				>
					{actionLoading === "ping" ? "Sending..." : "Ping"}
				</button>
				<button
					type="button"
					className={`${buttons.btnAction} ${styles.actionBtn}`}
					onClick={onSetup}
					disabled={actionLoading !== null}
				>
					{actionLoading === "setup" ? "Sending..." : "Setup"}
				</button>
				<button
					type="button"
					className={`${buttons.btnAction} ${styles.actionBtn}`}
					onClick={onRefresh}
				>
					Refresh
				</button>
				<ActionFeedback
					align="right"
					feedback={feedback.capture ?? feedback.ping ?? feedback.setup}
				/>
			</div>
			<div className={styles.headerStats}>
				{/* Two distinct indicators, tagged so they don't read as
				    redundant: NET = network/connectivity (MQTT reachability),
				    PWR = the MCU's own power-saving mode. A board can be NET
				    online while PWR is PS-REST (radio still checks in
				    periodically), or NET sleeping while PWR is Sleep (deep
				    dormant, silent by design). */}
				<StatusIndicator
					state={connection}
					label={CONNECTION_LABEL[connection]}
					tag="NET"
					title="Network — is the board currently reachable over MQTT/Wi-Fi?"
				/>
				<span
					className={`${styles.headerChip} ${styles.powerChip} ${POWER_CLASS[powerMode]}`}
					title="Power — the MCU's current power-saving mode, set from the Power tab"
				>
					<span className={styles.statusClusterTag}>PWR</span>
					{POWER_LABEL[powerMode]}
				</span>
				<span className={styles.headerChip}>
					FW {firmware ? `v${firmware}` : "\u2014"}
				</span>
				<span className={styles.headerChip}>
					<span className={statusVariantStyles[statusVariant]}>
						{boardStatus}
					</span>
				</span>
				{imagesError ? (
					<button
						type="button"
						className={`${styles.headerChip} ${styles.headerChipError}`}
						onClick={onRetryImages}
						title={`Failed to load capture count: ${imagesError}. Click to retry.`}
					>
						⚠ caps
					</button>
				) : (
					<span className={`${styles.headerChip} ${styles.highlight}`}>
						{imagesLoading ? "\u2026" : imagesCount} caps
					</span>
				)}
				{snapshotError && (
					<button
						type="button"
						className={`${styles.headerChip} ${styles.headerChipError}`}
						onClick={onRetrySnapshot}
						title={`Board state sync failed: ${snapshotError}. Click to retry.`}
					>
						⚠ sync
					</button>
				)}
				<ConnectionBadge connected={mqttConnected} label="MQTT" />
			</div>
		</header>
	);
}
