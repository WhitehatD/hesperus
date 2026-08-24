"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { getImages } from "@/lib/api-client";
import type { ImageCapture } from "@/lib/types";
import { useAsyncResource } from "./useAsyncResource";

const JUST_ARRIVED_MS = 5000;

/** Board detail page's capture gallery: the image list resource plus the
 * "just arrived" flash indicator driven by dashboard/images/new MQTT
 * pushes (kept as separate transient state, not part of the fetched data,
 * so a failed/slow refetch can never desync it). */
export function useGallery(apiBase: string, boardId: string) {
	const [justArrivedFilename, setJustArrivedFilename] = useState<string | null>(
		null,
	);
	const timeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);

	const fetchImages = useCallback(
		() => getImages(apiBase, boardId),
		[apiBase, boardId],
	);
	const resource = useAsyncResource<ImageCapture[]>(fetchImages);

	useEffect(() => {
		return () => {
			if (timeoutRef.current) clearTimeout(timeoutRef.current);
		};
	}, []);

	const flashJustArrived = useCallback((filename: string) => {
		setJustArrivedFilename(filename);
		if (timeoutRef.current) clearTimeout(timeoutRef.current);
		timeoutRef.current = setTimeout(() => {
			setJustArrivedFilename(null);
		}, JUST_ARRIVED_MS);
	}, []);

	const applyAnalysis = useCallback(
		(filename: string, analysis: ImageCapture["analysis"]) => {
			resource.setData((prev) =>
				(prev ?? []).map((img) =>
					img.filename === filename ? { ...img, analysis } : img,
				),
			);
		},
		[resource.setData],
	);

	const removeImage = useCallback(
		(filename: string) => {
			resource.setData((prev) =>
				(prev ?? []).filter((i) => i.filename !== filename),
			);
		},
		[resource.setData],
	);

	return {
		images: resource.data ?? [],
		loading: resource.loading,
		error: resource.error,
		refetch: resource.refetch,
		justArrivedFilename,
		flashJustArrived,
		applyAnalysis,
		removeImage,
	};
}
