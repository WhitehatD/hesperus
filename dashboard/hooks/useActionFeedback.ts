"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import type { ActionFeedbackState } from "@/lib/types";

/** Inline success/failure feedback for user-triggered actions (capture,
 * ping, schedule activate/deactivate, power mode, energy reset, ...),
 * rendered right next to the control that triggered it — the log console
 * alone isn't enough, it's a separate tab the user isn't necessarily
 * looking at when they click. Each keyed message self-clears. */
export function useActionFeedback() {
	const [feedback, setFeedback] = useState<Record<string, ActionFeedbackState>>(
		{},
	);
	const timersRef = useRef<Record<string, ReturnType<typeof setTimeout>>>({});

	const showFeedback = useCallback(
		(key: string, type: "success" | "error", message: string) => {
			setFeedback((prev) => ({ ...prev, [key]: { type, message } }));
			if (timersRef.current[key]) {
				clearTimeout(timersRef.current[key]);
			}
			timersRef.current[key] = setTimeout(
				() => {
					setFeedback((prev) => {
						if (!(key in prev)) return prev;
						const next = { ...prev };
						delete next[key];
						return next;
					});
				},
				type === "error" ? 6000 : 3000,
			);
		},
		[],
	);

	useEffect(() => {
		const timers = timersRef.current;
		return () => {
			for (const t of Object.values(timers)) clearTimeout(t);
		};
	}, []);

	return { feedback, showFeedback };
}
