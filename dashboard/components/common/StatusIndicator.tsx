"use client";

import type { ConnectionState } from "@/lib/types";
import styles from "./StatusIndicator.module.css";

const STATE_CLASS: Record<ConnectionState, string> = {
	online: styles.online,
	sleeping: styles.sleeping,
	offline: styles.offline,
};

/** The connectivity dot + label used both on the fleet board cards
 * (online/offline only) and the board-detail header's NET indicator
 * (online/sleeping/offline, with a "NET" tag to disambiguate it from the
 * adjacent PWR power-mode chip). Identical markup/CSS in both places
 * before this rewrite — a real shared component, not a premature one. */
export default function StatusIndicator({
	state,
	label,
	tag,
	title,
}: {
	state: ConnectionState;
	label: string;
	tag?: string;
	title?: string;
}) {
	return (
		<div
			className={`${styles.statusIndicator} ${STATE_CLASS[state]}`}
			title={title}
		>
			{tag && <span className={styles.statusClusterTag}>{tag}</span>}
			<div className={styles.dot} />
			{label}
		</div>
	);
}
