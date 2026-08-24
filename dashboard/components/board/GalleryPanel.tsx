"use client";

import type { ActionFeedbackState, ImageCapture } from "@/lib/types";
import ActionFeedback from "../common/ActionFeedback";
import ErrorBanner from "../common/ErrorBanner";
import styles from "./GalleryPanel.module.css";
import ImageCard from "./ImageCard";

export default function GalleryPanel({
	images,
	loading,
	error,
	onRetry,
	justArrivedFilename,
	onSelect,
	feedback,
}: {
	images: ImageCapture[];
	loading: boolean;
	error: string | null;
	onRetry: () => void;
	justArrivedFilename: string | null;
	onSelect: (image: ImageCapture) => void;
	/** Surfaces delete-image failures (showFeedback("gallery", ...)) — the
	 * lightbox is already closed by then, so this is the only place the
	 * user would see them. */
	feedback?: ActionFeedbackState;
}) {
	const sorted = [...images].sort((a, b) => b.timestamp - a.timestamp);

	return (
		<>
			{error && (
				<ErrorBanner
					compact
					message={`Couldn't load images: ${error}`}
					onRetry={onRetry}
				/>
			)}
			<ActionFeedback feedback={feedback} />
			<div className={styles.sidebarGallery}>
				{sorted.length === 0 ? (
					<div className={styles.emptyStateSm}>
						{loading
							? "Loading captures\u2026"
							: "No captures yet. Ask the agent to take a picture."}
					</div>
				) : (
					sorted.map((img) => (
						<ImageCard
							key={img.filename}
							image={img}
							isJustArrived={img.filename === justArrivedFilename}
							onClick={() => onSelect(img)}
						/>
					))
				)}
			</div>
		</>
	);
}
