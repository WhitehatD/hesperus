"use client";

import ConnectionBadge from "../common/ConnectionBadge";
import styles from "./FleetHeader.module.css";

export default function FleetHeader({
	nodeCount,
	images,
	imagesError,
	imagesLoading,
	onRetryImages,
	schedules,
	schedulesError,
	schedulesLoading,
	onRetrySchedules,
	mqttConnected,
}: {
	nodeCount: number;
	images: number;
	imagesError: string | null;
	imagesLoading: boolean;
	onRetryImages: () => void;
	schedules: number;
	schedulesError: string | null;
	schedulesLoading: boolean;
	onRetrySchedules: () => void;
	mqttConnected: boolean;
}) {
	return (
		<header className={styles.header}>
			<div className={styles.headerTitleContainer}>
				<div className={styles.headerLogo}>
					<svg
						width="22"
						height="22"
						viewBox="0 0 24 24"
						fill="none"
						stroke="currentColor"
						strokeWidth="2"
						strokeLinecap="round"
						strokeLinejoin="round"
						style={{ color: "var(--accent)" }}
						aria-hidden="true"
					>
						<circle cx="12" cy="12" r="3" />
						<path d="M12 1v4M12 19v4M4.22 4.22l2.83 2.83M16.95 16.95l2.83 2.83M1 12h4M19 12h4M4.22 19.78l2.83-2.83M16.95 7.05l2.83-2.83" />
					</svg>
				</div>
				<div>
					<h1 className={styles.headerTitle}>Hesperus</h1>
					<div className={styles.headerSubtitle}>
						Autonomous IoT Visual Monitoring
					</div>
				</div>
			</div>
			<div className={styles.fleetSummary}>
				<span className={styles.fleetChip}>
					{nodeCount} node{nodeCount !== 1 ? "s" : ""}
				</span>
				{imagesError ? (
					<button
						type="button"
						className={`${styles.fleetChip} ${styles.fleetChipError}`}
						onClick={onRetryImages}
						title={`Failed to load image count: ${imagesError}. Click to retry.`}
					>
						⚠ images
					</button>
				) : (
					<span className={styles.fleetChip}>
						{imagesLoading ? "\u2026" : images} images
					</span>
				)}
				{schedulesError ? (
					<button
						type="button"
						className={`${styles.fleetChip} ${styles.fleetChipError}`}
						onClick={onRetrySchedules}
						title={`Failed to load schedule count: ${schedulesError}. Click to retry.`}
					>
						⚠ schedules
					</button>
				) : (
					<span className={styles.fleetChip}>
						{schedulesLoading ? "\u2026" : schedules} schedules
					</span>
				)}
				<ConnectionBadge
					connected={mqttConnected}
					label={mqttConnected ? "MQTT Connected" : "Disconnected"}
				/>
			</div>
		</header>
	);
}
