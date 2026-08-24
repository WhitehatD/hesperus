"use client";

import { useCallback, useEffect, useRef, useState } from "react";

export interface AsyncResourceState<T> {
	data: T | null;
	loading: boolean;
	error: string | null;
	refetch: () => void;
	setData: (updater: T | ((prev: T | null) => T)) => void;
}

/**
 * One shared async-resource pattern for the whole dashboard: {data, loading, error, refetch}.
 *
 * Every ad-hoc `fetch(...).then(...).catch(console.error)` / `.catch(() => {})` used to
 * silently swallow failures — on a hotspot with real multi-second dead-air windows that
 * reads as "the app is broken" with zero indication why. This hook makes failure a
 * first-class, visible state instead.
 *
 * `fetcher` receives the resource's current data so merge-with-previous logic (e.g.
 * "keep completed_at from the last good snapshot if this response is stale") can be
 * expressed without a second ref/effect — it must reject/throw on failure, never swallow.
 * `fetcher` identity is the effect's dependency, same convention as useMQTT.ts's topicKey
 * pattern: wrap the fetcher passed in with useCallback and list its real deps there like
 * a normal effect.
 *
 * Guards against out-of-order responses (a slow earlier request resolving after a faster
 * later one) with a request-id check, so a manual retry or a poll tick can never clobber
 * fresher data with stale data.
 */
export function useAsyncResource<T>(
	fetcher: (current: T | null) => Promise<T>,
): AsyncResourceState<T> {
	const [data, setDataState] = useState<T | null>(null);
	const [loading, setLoading] = useState(true);
	const [error, setError] = useState<string | null>(null);

	const fetcherRef = useRef(fetcher);
	fetcherRef.current = fetcher;
	const dataRef = useRef<T | null>(data);
	dataRef.current = data;
	const requestIdRef = useRef(0);

	const setData = useCallback((updater: T | ((prev: T | null) => T)) => {
		setDataState((prev) => {
			const next =
				typeof updater === "function"
					? (updater as (prev: T | null) => T)(prev)
					: updater;
			dataRef.current = next;
			return next;
		});
	}, []);

	const load = useCallback(() => {
		const requestId = ++requestIdRef.current;
		setLoading(true);
		// Deliberately NOT clearing `error` here: a poll tick (schedules every
		// 30s, board state every 5s) calls refetch() on a timer, and clearing
		// then re-setting the same error on every failed tick would flicker
		// the error banner on/off. The previous error stays visible until this
		// attempt actually succeeds (cleared below) or produces a new message.
		fetcherRef
			.current(dataRef.current)
			.then((result) => {
				if (requestId !== requestIdRef.current) return; // superseded by a newer request
				setData(result);
				setError(null);
				setLoading(false);
			})
			.catch((err) => {
				if (requestId !== requestIdRef.current) return;
				setError(err instanceof Error ? err.message : String(err));
				setLoading(false);
			});
	}, [setData]);

	// biome-ignore lint/correctness/useExhaustiveDependencies: fetcher/data are read via refs inside load(); re-fetch is intentionally driven by the caller's fetcher identity only (mirrors useMQTT.ts's topicKey pattern)
	useEffect(() => {
		load();
	}, [fetcher]);

	return { data, loading, error, refetch: load, setData };
}
