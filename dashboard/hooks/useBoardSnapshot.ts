"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { getBoardState } from "@/lib/api-client";
import type { ConnectionState, PowerMode } from "@/lib/types";
import { useAsyncResource } from "./useAsyncResource";

const POLL_MS = 5000;
// After sending a power-mode command the board still sends heartbeats with
// the OLD state until the MQTT command propagates (~2-10s). Suppress
// heartbeat-based powerMode updates for this long to prevent the optimistic
// state from flickering back before the board confirms the change.
const POWER_COMMAND_SUPPRESS_MS = 15_000;

export interface BoardSnapshotState {
	firmware: string | null;
	lastSeen: number | null;
	connection: ConnectionState;
	powerMode: PowerMode;
	/** Raw board status string from the last MQTT message that carried one
	 * (e.g. "idle", "capturing", "uploading"). Defaults to "idle". */
	status: string;
	error: string | null;
	refetch: () => void;
	/** Optimistically set powerMode and suppress heartbeat-driven overwrites
	 * for POWER_COMMAND_SUPPRESS_MS — call right after issuing a power
	 * command, before the board has had a chance to confirm it. */
	setOptimisticPowerMode: (mode: PowerMode) => void;
	/** Apply a live MQTT heartbeat's power-mode signal, honoring the
	 * suppression window above. */
	applyHeartbeatPowerMode: (data: {
		status?: string;
		sleep_mode?: number;
		lp_mode?: string;
	}) => void;
	/** Apply a live MQTT heartbeat's connection/firmware/status signal. */
	applyHeartbeat: (data: { firmware?: string; status?: string }) => void;
	markSleeping: () => void;
}

/** Hydrate board state (connection/power-mode/firmware) from the server
 * snapshot — the single source of truth. Runs on mount and polls every 5s,
 * so a page refresh reflects real board state immediately instead of
 * blanking until the next MQTT heartbeat (sparse in PS-REST — the board
 * sleeps ~97% of the time). Live MQTT heartbeats layer on top via
 * applyHeartbeat/applyHeartbeatPowerMode. */
export function useBoardSnapshot(apiBase: string): BoardSnapshotState {
	const [firmware, setFirmware] = useState<string | null>(null);
	const [lastSeen, setLastSeen] = useState<number | null>(null);
	const [connection, setConnection] = useState<ConnectionState>("sleeping");
	const [powerMode, setPowerMode] = useState<PowerMode>("unknown");
	const [status, setStatus] = useState<string>("idle");
	const powerModePendingUntil = useRef<number>(0);

	const fetchSnapshot = useCallback(() => getBoardState(apiBase), [apiBase]);
	const resource = useAsyncResource(fetchSnapshot);

	// biome-ignore lint/correctness/useExhaustiveDependencies: lastSeen intentionally omitted — this effect only reacts to a new snapshot, reading the prior lastSeen as a fallback
	useEffect(() => {
		const snap = resource.data;
		if (!snap) return;

		// Derive power mode from the board's reported state. Sleep (deep
		// dormant, or armed-but-awake when no schedule) wins, then PS-REST,
		// then Active. lp_mode and sleep_mode are exclusive on the board.
		if (snap.state === "deep_dormant" || snap.sleep_mode === true) {
			setPowerMode("sleep");
		} else if (snap.lp_mode === "ps_rest") {
			setPowerMode("ps_rest");
		} else if (snap.lp_mode === "normal") {
			setPowerMode("active");
		} // else: leave as-is (unknown until the board reports)

		const lastSeenMs = snap.last_seen
			? new Date(snap.last_seen).getTime()
			: lastSeen;
		const ageMs =
			lastSeenMs != null ? Date.now() - lastSeenMs : Number.POSITIVE_INFINITY;

		setFirmware((prev) => snap.firmware ?? prev);
		setLastSeen(lastSeenMs);
		if (snap.state === "deep_dormant") {
			setConnection("sleeping");
		} else if (snap.state === "online") {
			setConnection(
				ageMs < 15000 ? "online" : ageMs < 120000 ? "sleeping" : "offline",
			);
		}
	}, [resource.data]);

	useEffect(() => {
		const interval = setInterval(resource.refetch, POLL_MS);
		return () => clearInterval(interval);
	}, [resource.refetch]);

	// Decay connection state purely from elapsed time since lastSeen, on its
	// own 5s tick — independent of both the snapshot poll (which only fires
	// every 5s and only reflects server-side state) and live MQTT heartbeats
	// (sparse in PS-REST, ~97% duty-cycled asleep). online -> sleeping @15s,
	// sleeping -> offline @2min.
	useEffect(() => {
		const interval = setInterval(() => {
			if (!lastSeen) return;
			const elapsed = Date.now() - lastSeen;
			setConnection((prev) => {
				if (prev === "online" && elapsed > 15000) return "sleeping";
				if (prev === "sleeping" && elapsed > 120000) return "offline";
				return prev;
			});
		}, 5000);
		return () => clearInterval(interval);
	}, [lastSeen]);

	const setOptimisticPowerMode = useCallback((mode: PowerMode) => {
		setPowerMode(mode);
		powerModePendingUntil.current = Date.now() + POWER_COMMAND_SUPPRESS_MS;
	}, []);

	const applyHeartbeatPowerMode = useCallback(
		(data: { status?: string; sleep_mode?: number; lp_mode?: string }) => {
			if (Date.now() < powerModePendingUntil.current) return;
			if (data.status === "deep_dormant" || data.sleep_mode === 1) {
				setPowerMode("sleep");
			} else if (data.status === "awake") {
				setPowerMode("active");
			} else if (data.lp_mode === "ps_rest") {
				setPowerMode("ps_rest");
			} else if (data.lp_mode === "normal") {
				setPowerMode("active");
			}
		},
		[],
	);

	const applyHeartbeat = useCallback(
		(data: { firmware?: string; status?: string }) => {
			setLastSeen(Date.now());
			setConnection("online");
			if (data.firmware) setFirmware(data.firmware);
			if (data.status) setStatus(data.status);
		},
		[],
	);

	const markSleeping = useCallback(() => setConnection("sleeping"), []);

	return {
		firmware,
		lastSeen,
		connection,
		powerMode,
		status,
		error: resource.error,
		refetch: resource.refetch,
		setOptimisticPowerMode,
		applyHeartbeatPowerMode,
		applyHeartbeat,
		markSleeping,
	};
}
