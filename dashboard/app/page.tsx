"use client";

import { useCallback, useEffect, useState } from "react";
import ErrorBanner from "@/components/common/ErrorBanner";
import BoardCard from "@/components/fleet/BoardCard";
import FleetHeader from "@/components/fleet/FleetHeader";
import { useAsyncResource } from "@/hooks/useAsyncResource";
import { useBoardTracker } from "@/hooks/useMQTT";
import { getImages, getSchedules } from "@/lib/api-client";
import styles from "./page.module.css";

const FLEET_TOPICS = ["device/+/status"];

export default function DashboardPage() {
	const { boards, connectionStatus } = useBoardTracker(FLEET_TOPICS);
	const [now, setNow] = useState(Date.now());

	const apiBase = process.env.NEXT_PUBLIC_API_URL || "";

	useEffect(() => {
		const timer = setInterval(() => setNow(Date.now()), 1000);
		return () => clearInterval(timer);
	}, []);

	// Real per-board capture counts, fetched from server history. This is
	// the ONLY source of truth for the "Images" stat — there is no
	// MQTT-derived fallback (useMQTT.ts's BoardTelemetry.captures is
	// session-only and starts at 0 on every page load; silently
	// substituting it here for a failed/pending fetch used to render a
	// plausible-looking wrong zero).
	const fetchTotalImages = useCallback(async () => {
		const images = await getImages(apiBase);
		const counts: Record<string, number> = {};
		for (const img of images) {
			counts[img.boardId] = (counts[img.boardId] || 0) + 1;
		}
		return counts;
	}, [apiBase]);
	const imagesStats = useAsyncResource(fetchTotalImages);
	const totalImages = imagesStats.data ?? {};

	const fetchScheduleCount = useCallback(async () => {
		const scheds = await getSchedules(apiBase);
		return scheds.length;
	}, [apiBase]);
	const scheduleStats = useAsyncResource(fetchScheduleCount);
	const scheduleCount = scheduleStats.data ?? 0;

	const boardList = Object.values(boards);

	return (
		<div className="app-container">
			<FleetHeader
				nodeCount={boardList.length}
				images={Object.values(totalImages).reduce((a, b) => a + b, 0)}
				imagesError={imagesStats.error}
				imagesLoading={imagesStats.loading && imagesStats.data === null}
				onRetryImages={imagesStats.refetch}
				schedules={scheduleCount}
				schedulesError={scheduleStats.error}
				schedulesLoading={scheduleStats.loading && scheduleStats.data === null}
				onRetrySchedules={scheduleStats.refetch}
				mqttConnected={connectionStatus === "connected"}
			/>

			{(imagesStats.error || scheduleStats.error) && (
				<ErrorBanner
					message={
						imagesStats.error && scheduleStats.error
							? "Couldn't load image or schedule counts from the server."
							: imagesStats.error
								? "Couldn't load image counts from the server."
								: "Couldn't load schedule counts from the server."
					}
					onRetry={() => {
						if (imagesStats.error) imagesStats.refetch();
						if (scheduleStats.error) scheduleStats.refetch();
					}}
				/>
			)}

			<section>
				<div className={styles.boardsGrid}>
					{boardList.length === 0 && connectionStatus === "connected" && (
						<div className={styles.emptyState}>
							Waiting for edge nodes to check in via MQTT...
						</div>
					)}

					{boardList.length === 0 && connectionStatus !== "connected" && (
						<div className={styles.loadingCard}>
							<div className={`${styles.skeleton} ${styles.skeletonLine}`} />
							<div className={`${styles.skeleton} ${styles.skeletonLine}`} />
							<div className={`${styles.skeleton} ${styles.skeletonLine}`} />
						</div>
					)}

					{boardList.map((board) => (
						<BoardCard
							key={board.id}
							board={board}
							imageCount={totalImages[board.id] ?? null}
							imageCountError={imagesStats.error}
							imageCountLoading={
								imagesStats.loading && imagesStats.data === null
							}
							now={now}
						/>
					))}
				</div>
			</section>
		</div>
	);
}
