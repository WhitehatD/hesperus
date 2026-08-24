"use client";

import type { ActionFeedbackState } from "@/lib/types";
import { CheckIcon, WarningIcon } from "../icons/Icons";
import styles from "./ActionFeedback.module.css";

/** Inline success/failure feedback rendered right next to the control that
 * triggered an action — self-clearing (see hooks/useActionFeedback.ts). */
export default function ActionFeedback({
	feedback,
	align = "left",
}: {
	feedback?: ActionFeedbackState;
	/** Which edge of the zero-width anchor the popup grows from. "right"
	 * for anchors near a container's right/trailing edge (e.g. the last
	 * button in a row) so the popup can't overflow off-screen on mobile —
	 * a "left"-grown popup anchored at the trailing edge of a narrow
	 * viewport has nowhere to grow but off the right side of the screen. */
	align?: "left" | "right";
}) {
	// Zero-footprint anchor, always rendered (not just when feedback is
	// present) — the message itself is `position: absolute` inside it, so
	// it floats OVER the layout instead of inserting/removing a flex
	// sibling every time feedback appears/clears. That in-flow insertion
	// was the exact bug: clicking Ping made every button after it jump
	// sideways because a new flex item had appeared in the row.
	return (
		<span className={styles.actionFeedbackAnchor}>
			{feedback && (
				<span
					className={`${styles.actionFeedback} ${feedback.type === "success" ? styles.success : styles.error} ${align === "right" ? styles.right : styles.left}`}
				>
					{feedback.type === "success" ? (
						<CheckIcon size={12} />
					) : (
						<WarningIcon size={12} />
					)}
					{feedback.message}
				</span>
			)}
		</span>
	);
}
