"use client";

import { useCallback, useEffect, useState } from "react";
import { energySeedToTotals, getEnergySeed } from "@/lib/api-client";
import type { EnergyTotals } from "@/lib/types";
import { useAsyncResource } from "./useAsyncResource";

const EMPTY_TOTALS: EnergyTotals = {
	windows: 0,
	totalWindowMs: 0,
	totalPsRestMs: 0,
	totalCaptureMs: 0,
	lastWindowMs: 0,
	lastUpdate: null,
};

/** RQ3 energy telemetry — seeded from the server on mount, then accumulated
 * live from MQTT "energy" status windows (~60s each, reported while the
 * board is in PS-REST). */
export function useEnergy(apiBase: string) {
	const [energy, setEnergy] = useState<EnergyTotals>(EMPTY_TOTALS);

	const fetchSeed = useCallback(() => getEnergySeed(apiBase), [apiBase]);
	const seedResource = useAsyncResource(fetchSeed);

	useEffect(() => {
		if (!seedResource.data) return;
		setEnergy(energySeedToTotals(seedResource.data));
	}, [seedResource.data]);

	const applyWindow = useCallback(
		(winMs: number, psMs: number, capMs: number) => {
			setEnergy((prev) => ({
				windows: prev.windows + 1,
				totalWindowMs: prev.totalWindowMs + winMs,
				totalPsRestMs: prev.totalPsRestMs + psMs,
				totalCaptureMs: prev.totalCaptureMs + capMs,
				lastWindowMs: winMs,
				lastUpdate: Date.now(),
			}));
		},
		[],
	);

	const reset = useCallback(() => setEnergy(EMPTY_TOTALS), []);

	return {
		energy,
		error: seedResource.error,
		refetch: seedResource.refetch,
		applyWindow,
		reset,
	};
}
