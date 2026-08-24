"use client";

import mqtt from "mqtt";
import { useEffect, useRef, useState } from "react";
import type { BoardTelemetry } from "@/lib/types";

export type MessageHandler = (
	topic: string,
	// biome-ignore lint/suspicious/noExplicitAny: MQTT payloads are arbitrary JSON from firmware/server, shape varies per topic
	data: Record<string, any>,
	boardId: string,
) => void;

const OFFLINE_TIMEOUT_MS = 15_000;

function getMqttUrl(): string {
	if (typeof window === "undefined") return "ws://localhost:9001";
	return (
		process.env.NEXT_PUBLIC_MQTT_WS_URL ||
		`${window.location.protocol === "https:" ? "wss:" : "ws:"}//${window.location.host}/mqtt`
	);
}

// biome-ignore lint/suspicious/noExplicitAny: see MessageHandler
function extractBoardId(topic: string, data: Record<string, any>): string {
	let boardId = "stm32";
	if (topic.startsWith("device/")) {
		const parts = topic.split("/");
		if (parts.length >= 3) {
			boardId = parts[1];
		}
	}
	if (data.client_id || data.board_id) {
		boardId = data.client_id || data.board_id;
	}
	return boardId;
}

export function useMQTT(topics: string[], onMessage?: MessageHandler) {
	const [connectionStatus, setConnectionStatus] = useState<
		"connecting" | "connected" | "disconnected" | "error"
	>("connecting");
	const clientRef = useRef<mqtt.MqttClient | null>(null);
	const onMessageRef = useRef(onMessage);
	onMessageRef.current = onMessage;
	const topicsRef = useRef(topics);
	topicsRef.current = topics;

	// Stable key for reconnection — only reconnect if topic list actually changes
	const topicKey = topics.join(",");

	// biome-ignore lint/correctness/useExhaustiveDependencies: reconnect controlled by topicKey, topics accessed via ref
	useEffect(() => {
		// Read-only credentials — enforced by mosquitto's ACL (mosquitto/acl:
		// `hesperus-dashboard` has `topic read` only, never `readwrite`), not
		// by keeping this value out of the bundle. It's in the client JS,
		// visible to anyone via devtools; that's fine because the ACL means
		// the worst a leaked value enables is watching telemetry, never
		// publishing a board command. Do NOT reuse the board's own
		// (readwrite) credentials here.
		const client = mqtt.connect(getMqttUrl(), {
			reconnectPeriod: 3000,
			username: process.env.NEXT_PUBLIC_MQTT_USERNAME || undefined,
			password: process.env.NEXT_PUBLIC_MQTT_PASSWORD || undefined,
		});
		clientRef.current = client;

		client.on("connect", () => {
			setConnectionStatus("connected");
			for (const t of topicsRef.current) {
				client.subscribe(t, { qos: 0 });
			}
		});

		client.on("message", (topic, payload) => {
			const raw = payload.toString();
			// biome-ignore lint/suspicious/noExplicitAny: see MessageHandler
			let data: Record<string, any>;
			try {
				data = JSON.parse(raw);
			} catch {
				data = { raw: raw.trim() };
			}
			const boardId = extractBoardId(topic, data);
			onMessageRef.current?.(topic, data, boardId);
		});

		client.on("close", () => setConnectionStatus("disconnected"));
		client.on("error", () => setConnectionStatus("error"));

		return () => {
			client.end();
		};
	}, [topicKey]);

	return { connectionStatus, client: clientRef };
}

export function useBoardTracker(topics: string[]) {
	const [boards, setBoards] = useState<Record<string, BoardTelemetry>>({});

	const handleMessage: MessageHandler = (topic, data, boardId) => {
		setBoards((prev) => {
			const curr = prev[boardId] || {
				id: boardId,
				name: "B-U585I-IOT02A",
				firmware: null,
				lastSeen: null,
				status: "idle",
				captures: 0,
				lastImageSize: null,
				lastLatencyMs: null,
				isOnline: false,
				uptimeSeconds: null,
				wifiRssi: null,
			};

			const update = { ...curr, isOnline: true, lastSeen: Date.now() };
			if (data.firmware) update.firmware = data.firmware;
			if (data.status) update.status = data.status;
			if (data.uptime_s != null) update.uptimeSeconds = data.uptime_s;
			if (data.wifi_rssi != null) update.wifiRssi = data.wifi_rssi;
			if (data.status === "captured" || data.status === "uploaded") {
				update.captures += 1;
				if (data.size) update.lastImageSize = data.size;
				if (data.latency_ms) update.lastLatencyMs = data.latency_ms;
			}

			return { ...prev, [boardId]: update };
		});
	};

	const { connectionStatus } = useMQTT(topics, handleMessage);

	// Mark boards offline after timeout
	useEffect(() => {
		const interval = setInterval(() => {
			setBoards((prev) => {
				let changed = false;
				const next = { ...prev };
				const now = Date.now();
				for (const id in next) {
					const board = next[id];
					if (
						board.isOnline &&
						board.lastSeen &&
						now - board.lastSeen > OFFLINE_TIMEOUT_MS
					) {
						next[id] = { ...board, isOnline: false };
						changed = true;
					}
				}
				return changed ? next : prev;
			});
		}, 5000);
		return () => clearInterval(interval);
	}, []);

	return { boards, connectionStatus };
}
