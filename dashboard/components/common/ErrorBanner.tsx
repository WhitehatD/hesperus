"use client";

import styles from "./ErrorBanner.module.css";

export default function ErrorBanner({
	message,
	onRetry,
	compact = false,
}: {
	message: string;
	onRetry?: () => void;
	compact?: boolean;
}) {
	return (
		<div
			className={`${styles.errorBanner} ${compact ? styles.errorBannerCompact : ""}`}
			role="alert"
		>
			<span className={styles.errorBannerText}>{message}</span>
			{onRetry && (
				<button
					type="button"
					className={styles.errorBannerRetry}
					onClick={onRetry}
				>
					Retry
				</button>
			)}
		</div>
	);
}
