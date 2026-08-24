"use client";

import type { ImageCapture } from "@/lib/types";
import buttons from "../common/buttons.module.css";
import styles from "./Lightbox.module.css";

export default function Lightbox({
	image,
	onClose,
	onDelete,
}: {
	image: ImageCapture;
	onClose: () => void;
	onDelete: (image: ImageCapture) => void;
}) {
	return (
		<div className={styles.overlay} onClick={onClose}>
			<div className={styles.content} onClick={(e) => e.stopPropagation()}>
				<button type="button" className={styles.close} onClick={onClose}>
					&times;
				</button>
				<img src={image.url} alt="Full size capture" className={styles.image} />
				<div className={styles.footer}>
					<div>
						<h3>Task #{image.taskId}</h3>
						<p>{new Date(image.timestamp * 1000).toLocaleString()}</p>
					</div>
					<div className={styles.actions}>
						<a
							href={image.url}
							download
							className={`${buttons.btn} ${buttons.btnSecondary}`}
						>
							Download
						</a>
						<button
							type="button"
							className={`${buttons.btn} ${buttons.btnDanger}`}
							onClick={() => onDelete(image)}
						>
							Delete
						</button>
					</div>
				</div>
				{image.analysis && (
					<div className={styles.analysisPanel}>
						<div className={styles.analysisHeader}>
							<span className={styles.analysisMeta}>
								{image.analysis.model} &middot;{" "}
								{image.analysis.inferenceMs.toFixed(0)}ms
							</span>
						</div>
						{image.analysis.objective && (
							<div className={styles.analysisRow}>
								<span className={styles.analysisLabel}>Objective</span>
								<p>{image.analysis.objective}</p>
							</div>
						)}
						<div className={styles.analysisRow}>
							<span className={styles.analysisLabel}>Findings</span>
							<p>{image.analysis.findings}</p>
						</div>
						<div className={styles.analysisRow}>
							<span className={styles.analysisLabel}>Recommendation</span>
							<p>{image.analysis.recommendation}</p>
						</div>
					</div>
				)}
			</div>
		</div>
	);
}
