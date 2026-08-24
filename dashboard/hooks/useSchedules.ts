"use client";

import { useCallback, useEffect, useState } from "react";
import { getSchedules } from "@/lib/api-client";
import type { Schedule, TaskLiveStatus } from "@/lib/types";
import { useAsyncResource } from "./useAsyncResource";

const POLL_MS = 30_000; // fallback poll; MQTT push is primary, this catches reconnects

/** Merge freshly-fetched schedules with the previous snapshot's
 * `completed_at` values — protects against a stale HTTP response (raced by
 * a faster MQTT push) wiping out a task's completion that already landed. */
function mergeCompletedAt(fresh: Schedule[], prev: Schedule[]): Schedule[] {
	if (prev.length === 0) return fresh;
	const prevMap = new Map(
		prev.flatMap((s) => s.tasks.map((t) => [t.id, t.completed_at] as const)),
	);
	return fresh.map((s) => ({
		...s,
		tasks: s.tasks.map((t) => ({
			...t,
			completed_at: t.completed_at || prevMap.get(t.id) || null,
		})),
	}));
}

export function useSchedules(apiBase: string) {
	const [taskStatuses, setTaskStatuses] = useState<
		Record<number, TaskLiveStatus>
	>({});

	const fetchSchedules = useCallback(
		async (current: Schedule[] | null) => {
			const fresh = await getSchedules(apiBase);
			return mergeCompletedAt(fresh, current ?? []);
		},
		[apiBase],
	);
	const resource = useAsyncResource<Schedule[]>(fetchSchedules);

	useEffect(() => {
		const interval = setInterval(resource.refetch, POLL_MS);
		return () => clearInterval(interval);
	}, [resource.refetch]);

	/** Apply a live dashboard/schedules/updated MQTT push — same merge
	 * semantics as the HTTP fetch. */
	const applyMqttUpdate = useCallback(
		(schedules: Schedule[]) => {
			resource.setData((prev) => mergeCompletedAt(schedules, prev ?? []));
		},
		[resource.setData],
	);

	const setTaskStatus = useCallback((taskId: number, status: string) => {
		setTaskStatuses((prev) => ({
			...prev,
			[taskId]: { status, updatedAt: Date.now() },
		}));
	}, []);

	const removeSchedule = useCallback(
		(scheduleId: number) => {
			resource.setData((prev) =>
				(prev ?? []).filter((s) => s.id !== scheduleId),
			);
		},
		[resource.setData],
	);

	return {
		schedules: resource.data ?? [],
		loading: resource.loading,
		error: resource.error,
		refetch: resource.refetch,
		applyMqttUpdate,
		taskStatuses,
		setTaskStatus,
		removeSchedule,
	};
}
