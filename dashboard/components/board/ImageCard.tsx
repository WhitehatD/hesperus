"use client";

import type { ImageCapture } from "@/lib/types";
import styles from "./ImageCard.module.css";

export default function ImageCard({
	image,
	isJustArrived,
	onClick,
}: {
	image: ImageCapture;
	isJustArrived: boolean;
	onClick: () => void;
}) {
	return (
		<div
			className={`${styles.imageCard} ${isJustArrived ? styles.isNew : ""}`}
			onClick={onClick}
		>
			{isJustArrived && <div className={styles.newBadge}>NEW</div>}
			{image.analysis && <div className={styles.analysisIndicator} />}
			<div className={styles.imageWrapper}>
				<img src={image.url} alt={`Task ${image.taskId}`} loading="lazy" />
			</div>
			<div className={styles.imageMeta}>
				<span>#{image.taskId}</span>
				<span className={styles.imageTime}>
					{new Date(image.timestamp * 1000).toLocaleTimeString()}
				</span>
			</div>
		</div>
	);
}
