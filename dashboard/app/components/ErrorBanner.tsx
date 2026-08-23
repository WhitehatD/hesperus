"use client";

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
			className={`error-banner${compact ? " error-banner-compact" : ""}`}
			role="alert"
		>
			<span className="error-banner-text">{message}</span>
			{onRetry && (
				<button type="button" className="error-banner-retry" onClick={onRetry}>
					Retry
				</button>
			)}
		</div>
	);
}
