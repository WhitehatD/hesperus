"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import styles from "./AgentChat.module.css";
import type { Block } from "./BlockRenderer";
import BlockRenderer from "./BlockRenderer";
import SessionTabs, { type DbSession } from "./SessionTabs";

/* Server-persisted block shape (ChatMessage.blocks_json — see
 * _mirror() in agent_routes.py). Snake_case, wire-format field names,
 * matching the raw SSE event data rather than the client's Block type —
 * image_url is a relative API path, not yet prefixed with apiBase. */
interface RawPersistedBlock {
	type: "step" | "text" | "error";
	id?: string;
	label?: string;
	status?: "running" | "done" | "error";
	summary?: string;
	image_url?: string;
	text?: string;
}

interface Message {
	role: "user" | "assistant";
	text?: string;
	blocks: Block[];
	streaming?: boolean;
}

/**
 * Reconstruct the same Block[] shape the live SSE stream renders, from a
 * persisted ChatMessage row. Sessions created before blocks_json existed
 * (or a row that failed to persist blocks for some reason) have no
 * `blocks` — fall back to wrapping the plain content string, same as
 * before this fix, so old history doesn't break.
 */
function reconstructBlocks(
	raw: RawPersistedBlock[] | null | undefined,
	content: string,
	apiBase: string,
): Block[] {
	if (!raw || raw.length === 0) {
		return [{ type: "text", text: content }];
	}
	return raw.map((b): Block => {
		if (b.type === "step") {
			return {
				type: "step",
				id: b.id || "",
				label: b.label || "",
				status: b.status || "done",
				summary: b.summary,
				imageUrl: b.image_url ? `${apiBase}${b.image_url}` : undefined,
			};
		}
		if (b.type === "error") {
			return { type: "error", text: b.text || "" };
		}
		return { type: "text", text: b.text || "" };
	});
}

interface AgentChatProps {
	boardId: string;
	apiBase: string;
	fullSize?: boolean;
}

const QUICK_ACTIONS = [
	"Take a picture now",
	"Monitor for 5 minutes",
	"What does the camera see?",
	"Enter setup mode",
	"Ping the board",
];

export default function AgentChat({
	boardId,
	apiBase,
	fullSize,
}: AgentChatProps) {
	const [sessions, setSessions] = useState<DbSession[]>([]);
	const [activeId, setActiveId] = useState<number | null>(null);
	const [messages, setMessages] = useState<Message[]>([]);
	const [input, setInput] = useState("");
	const [isStreaming, setIsStreaming] = useState(false);
	const [loading, setLoading] = useState(true);
	const messagesEndRef = useRef<HTMLDivElement>(null);
	const textareaRef = useRef<HTMLTextAreaElement>(null);
	const abortControllerRef = useRef<AbortController | null>(null);

	// biome-ignore lint/correctness/useExhaustiveDependencies: load sessions on mount and board change only
	useEffect(() => {
		fetchSessions();
	}, [boardId]);

	const fetchSessions = async () => {
		try {
			const res = await fetch(
				`${apiBase}/api/agent/sessions?board_id=${boardId}`,
			);
			if (!res.ok) return;
			const data: DbSession[] = await res.json();
			setSessions(data);
			if (data.length > 0 && !activeId) {
				setActiveId(data[0].id);
				loadMessages(data[0].id);
			} else {
				setLoading(false);
			}
		} catch {
			setLoading(false);
		}
	};

	const loadMessages = async (sessionId: number) => {
		setLoading(true);
		try {
			const res = await fetch(
				`${apiBase}/api/agent/sessions/${sessionId}/messages`,
			);
			if (!res.ok) {
				setMessages([]);
				return;
			}
			const data: {
				role: string;
				content: string;
				blocks?: RawPersistedBlock[] | null;
			}[] = await res.json();
			setMessages(
				data.map((m) => ({
					role: m.role as "user" | "assistant",
					text: m.role === "user" ? m.content : undefined,
					blocks:
						m.role === "assistant"
							? reconstructBlocks(m.blocks, m.content, apiBase)
							: [],
				})),
			);
		} catch {
			setMessages([]);
		} finally {
			setLoading(false);
		}
	};

	const createSession = async () => {
		try {
			const res = await fetch(`${apiBase}/api/agent/sessions`, {
				method: "POST",
				headers: { "Content-Type": "application/json" },
				body: JSON.stringify({
					boardId,
					name: `Session ${new Date().toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" })}`,
				}),
			});
			if (!res.ok) return;
			const session: DbSession = await res.json();
			setSessions((prev) => [session, ...prev]);
			setActiveId(session.id);
			setMessages([]);
		} catch {
			// network error
		}
	};

	const deleteSession = async (id: number) => {
		if (sessions.length <= 1) return;
		try {
			await fetch(`${apiBase}/api/agent/sessions/${id}`, { method: "DELETE" });
			setSessions((prev) => prev.filter((s) => s.id !== id));
			if (activeId === id) {
				const remaining = sessions.filter((s) => s.id !== id);
				if (remaining.length > 0) {
					setActiveId(remaining[0].id);
					loadMessages(remaining[0].id);
				}
			}
		} catch {
			// network error
		}
	};

	const clearMessages = async () => {
		if (!activeId) return;
		try {
			await fetch(`${apiBase}/api/agent/sessions/${activeId}/messages`, {
				method: "DELETE",
			});
			setMessages([]);
		} catch {
			// network error
		}
	};

	const switchSession = (id: number) => {
		if (id === activeId) return;
		setActiveId(id);
		loadMessages(id);
	};

	const scrollToBottom = useCallback(() => {
		messagesEndRef.current?.scrollIntoView({ behavior: "smooth" });
	}, []);

	// biome-ignore lint/correctness/useExhaustiveDependencies: intentional scroll
	useEffect(() => {
		scrollToBottom();
	}, [messages, scrollToBottom]);

	const handleSend = async (override?: string) => {
		const msg = (override || input).trim();
		if (!msg || isStreaming) return;

		// Slash commands
		if (msg === "/clear") {
			setInput("");
			clearMessages();
			return;
		}

		// Auto-create session if none exists
		if (!activeId) {
			try {
				const res = await fetch(`${apiBase}/api/agent/sessions`, {
					method: "POST",
					headers: { "Content-Type": "application/json" },
					body: JSON.stringify({
						boardId,
						name: `Session ${new Date().toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" })}`,
					}),
				});
				if (res.ok) {
					const session: DbSession = await res.json();
					setSessions((prev) => [session, ...prev]);
					setActiveId(session.id);
					await sendMessage(msg, session.id);
					return;
				}
			} catch {
				// fall through
			}
		}

		setInput("");
		await sendMessage(msg, activeId as number);
	};

	const sendMessage = async (msg: string, sessionId: number) => {
		setIsStreaming(true);

		const controller = new AbortController();
		abortControllerRef.current = controller;

		const userMsg: Message = { role: "user", text: msg, blocks: [] };
		const botMsg: Message = { role: "assistant", blocks: [], streaming: true };
		setMessages((prev) => [...prev, userMsg, botMsg]);

		try {
			const res = await fetch(`${apiBase}/api/agent/chat`, {
				method: "POST",
				headers: { "Content-Type": "application/json" },
				body: JSON.stringify({ message: msg, sessionId }),
				signal: controller.signal,
			});

			if (!res.ok || !res.body) {
				throw new Error(`Server error: ${res.status}`);
			}

			const reader = res.body.getReader();
			const decoder = new TextDecoder();
			let buffer = "";

			while (true) {
				const { done, value } = await reader.read();
				if (done) break;

				buffer += decoder.decode(value, { stream: true });
				const lines = buffer.split("\n");
				buffer = lines.pop() || "";

				for (const line of lines) {
					if (!line.startsWith("data: ")) continue;
					const jsonStr = line.slice(6).trim();
					if (!jsonStr) continue;

					try {
						const data = JSON.parse(jsonStr);
						const event = data.event;

						setMessages((prev) => {
							const updated = [...prev];
							const bot = {
								...updated[updated.length - 1],
								blocks: [...(updated[updated.length - 1].blocks || [])],
							};

							if (event === "thinking") {
								const thinkIdx = bot.blocks.findIndex(
									(b) => b.type === "thinking",
								);
								if (thinkIdx >= 0) {
									bot.blocks[thinkIdx] = { type: "thinking", text: data.text };
								} else {
									bot.blocks.push({ type: "thinking", text: data.text });
								}
							} else if (event === "tool_call") {
								bot.blocks.push({
									type: "step",
									id: data.id,
									label: data.label,
									status: "running",
								});
							} else if (event === "tool_result") {
								const idx = bot.blocks.findIndex(
									(b) =>
										b.type === "step" &&
										b.id === data.id &&
										b.status === "running",
								);
								if (idx >= 0) {
									bot.blocks[idx] = {
										...(bot.blocks[idx] as Extract<Block, { type: "step" }>),
										status: data.success ? "done" : "error",
										summary: data.summary,
										imageUrl: data.image_url
											? `${apiBase}${data.image_url}`
											: undefined,
									};
								}
							} else if (event === "tool_update") {
								// Heartbeat: update label of an in-progress step without changing its status
								const idx = bot.blocks.findIndex(
									(b) =>
										b.type === "step" &&
										b.id === data.id &&
										b.status === "running",
								);
								if (idx >= 0) {
									bot.blocks[idx] = {
										...(bot.blocks[idx] as Extract<Block, { type: "step" }>),
										label: data.label,
									};
								}
							} else if (event === "reply") {
								bot.blocks.push({ type: "text", text: data.text });
							} else if (event === "error") {
								bot.blocks.push({ type: "error", text: data.text });
								bot.streaming = false;
							} else if (event === "done") {
								bot.streaming = false;
								bot.blocks = bot.blocks.filter((b) => b.type !== "thinking");
							}

							updated[updated.length - 1] = bot;
							return updated;
						});
					} catch {
						// skip malformed JSON
					}
				}
			}

			// Mark streaming done
			setMessages((prev) => {
				const updated = [...prev];
				const last = updated[updated.length - 1];
				if (last?.streaming) {
					updated[updated.length - 1] = { ...last, streaming: false };
				}
				return updated;
			});
		} catch (err) {
			const aborted = err instanceof DOMException && err.name === "AbortError";
			setMessages((prev) => {
				const updated = [...prev];
				const bot = updated[updated.length - 1];
				if (bot) {
					updated[updated.length - 1] = {
						...bot,
						blocks: aborted
							? bot.blocks
							: [
									...bot.blocks,
									{
										type: "error",
										text: `${err instanceof Error ? err.message : err}`,
									},
								],
						streaming: false,
					};
				}
				return updated;
			});
		} finally {
			setIsStreaming(false);
			abortControllerRef.current = null;
		}
	};

	const stopStreaming = () => {
		abortControllerRef.current?.abort();
	};

	const handleKeyDown = (e: React.KeyboardEvent) => {
		if (e.key === "Enter" && !e.shiftKey) {
			e.preventDefault();
			handleSend();
		}
	};

	return (
		<div
			className={`${styles.agentChat} ${fullSize ? styles.agentChatFull : ""}`}
		>
			<SessionTabs
				sessions={sessions}
				activeId={activeId}
				fullSize={fullSize}
				onSwitch={switchSession}
				onDelete={deleteSession}
				onCreate={createSession}
			/>

			<div className={styles.agentMessages}>
				{loading && messages.length === 0 && (
					<div className={styles.agentWelcome}>
						<div className={styles.agentTyping}>
							<span className={styles.typingDot} />
							<span className={styles.typingDot} />
							<span className={styles.typingDot} />
						</div>
					</div>
				)}

				{!loading && messages.length === 0 && (
					<div className={styles.agentWelcome}>
						<p>What would you like to monitor?</p>
						<div className={styles.agentQuickActions}>
							{QUICK_ACTIONS.map((qa) => (
								<button
									type="button"
									key={qa}
									className={styles.agentQuickBtn}
									onClick={() => handleSend(qa)}
								>
									{qa}
								</button>
							))}
						</div>
					</div>
				)}

				{messages.map((msg, i) => (
					<div
						key={i}
						className={`${styles.agentMsg} ${msg.role === "user" ? styles.agentMsgUser : styles.agentMsgAssistant}`}
					>
						{msg.role === "user" ? (
							<div className={`${styles.agentBubble} ${styles.userBubble}`}>
								{msg.text}
							</div>
						) : (
							<div className={`${styles.agentBubble} ${styles.botBubble}`}>
								{msg.blocks.map((block, bi) => (
									<BlockRenderer key={`${i}-${bi}`} block={block} />
								))}
								{msg.streaming && msg.blocks.length === 0 && (
									<div className={styles.agentTyping}>
										<span className={styles.typingDot} />
										<span className={styles.typingDot} />
										<span className={styles.typingDot} />
									</div>
								)}
							</div>
						)}
					</div>
				))}
				<div ref={messagesEndRef} />
			</div>

			<div className={styles.agentInputArea}>
				<textarea
					ref={textareaRef}
					className={styles.agentTextarea}
					placeholder="Ask the agent... (/clear to reset)"
					value={input}
					onChange={(e) => setInput(e.target.value)}
					onKeyDown={handleKeyDown}
					rows={1}
					disabled={isStreaming}
				/>
				<button
					type="button"
					className={styles.agentSendBtn}
					onClick={() => (isStreaming ? stopStreaming() : handleSend())}
					disabled={!isStreaming && !input.trim()}
					title={isStreaming ? "Stop" : "Send"}
				>
					{isStreaming ? (
						<svg
							width="12"
							height="12"
							viewBox="0 0 12 12"
							fill="currentColor"
							aria-hidden="true"
						>
							<rect x="1" y="1" width="10" height="10" rx="1.5" />
						</svg>
					) : (
						<svg
							width="16"
							height="16"
							viewBox="0 0 16 16"
							fill="none"
							aria-hidden="true"
						>
							<path
								d="M8 13V3M8 3L3.5 7.5M8 3l4.5 4.5"
								stroke="currentColor"
								strokeWidth="1.8"
								strokeLinecap="round"
								strokeLinejoin="round"
							/>
						</svg>
					)}
				</button>
			</div>
		</div>
	);
}
