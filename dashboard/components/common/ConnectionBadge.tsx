"use client";

import styles from "./ConnectionBadge.module.css";

/** MQTT connection-status pill — identical markup/CSS on the fleet page
 * and the board-detail header before this rewrite, only the label text
 * differed. */
export default function ConnectionBadge({
	connected,
	label,
}: {
	connected: boolean;
	label: string;
}) {
	return (
		<div className={styles.connectionBadge}>
			<div
				className={`${styles.connectionDot} ${connected ? styles.connected : styles.disconnected}`}
			/>
			{label}
		</div>
	);
}
