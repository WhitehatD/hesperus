"use client";

import { useCallback, useEffect, useRef, useState } from "react";

/* ── Block-based message model ───────────────────────────── */

type Block =
	| { type: "thinking"; text: string }
	| {
			type: "step";
			id: string;
			label: string;
			status: "running" | "done" | "error";
			summary?: string;
			imageUrl?: string;
	  }
	| { type: "text"; text: string }
	| { type: "error"; text: string };

interface Message {
	role: "user" | "assistant";
	text?: string;
	blocks: Block[];
	streaming?: boolean;
}

interface DbSession {
	id: number;
	name: string;
	boardId: string;
	createdAt: string;
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
			const data: { role: string; content: string }[] = await res.json();
			setMessages(
				data.map((m) => ({
					role: m.role as "user" | "assistant",
					text: m.role === "user" ? m.content : undefined,
					blocks:
						m.role === "assistant"
							? [{ type: "text" as const, text: m.content }]
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
			await fetch(`${apiBase}/api/agent/sessions/${id}`, {
				method: "DELETE",
			});
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
		await sendMessage(msg, activeId!);
	};

	const sendMessage = async (msg: string, sessionId: number) => {
		setIsStreaming(true);

		const controller = new AbortController();
		abortControllerRef.current = controller;

		const userMsg: Message = { role: "user", text: msg, blocks: [] };
		const botMsg: Message = {
			role: "assistant",
			blocks: [],
			streaming: true,
		};
		setMessages((prev) => [...prev, userMsg, botMsg]);

		try {
			const res = await fetch(`${apiBase}/api/agent/chat`, {
				method: "POST",
				headers: { "Content-Type": "application/json" },
				body: JSON.stringify({
					message: msg,
					sessionId: sessionId,
				}),
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
									bot.blocks[thinkIdx] = {
										type: "thinking",
										text: data.text,
									};
								} else {
									bot.blocks.push({
										type: "thinking",
										text: data.text,
									});
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
								bot.blocks.push({
									type: "text",
									text: data.text,
								});
							} else if (event === "error") {
								bot.blocks.push({
									type: "error",
									text: data.text,
								});
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
					updated[updated.length - 1] = {
						...last,
						streaming: false,
					};
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
		<div className={`agent-chat ${fullSize ? "agent-chat-full" : ""}`}>
			{/* Session tabs */}
			<div className="session-tabs">
				{sessions.map((s) => (
					<button
						key={s.id}
						className={`session-tab ${s.id === activeId ? "active" : ""}`}
						onClick={() => switchSession(s.id)}
					>
						<span className="session-tab-name">{s.name}</span>
						{sessions.length > 1 && (
							<span
								className="session-tab-close"
								onClick={(e) => {
									e.stopPropagation();
									deleteSession(s.id);
								}}
							>
								&times;
							</span>
						)}
					</button>
				))}
				<button
					className="session-tab session-tab-add"
					onClick={createSession}
					title="New session"
				>
					+
				</button>
			</div>

			{/* Messages */}
			<div className="agent-messages">
				{loading && messages.length === 0 && (
					<div className="agent-welcome">
						<div className="agent-typing">
							<span className="typing-dot" />
							<span className="typing-dot" />
							<span className="typing-dot" />
						</div>
					</div>
				)}

				{!loading && messages.length === 0 && (
					<div className="agent-welcome">
						<p>What would you like to monitor?</p>
						<div className="agent-quick-actions">
							{QUICK_ACTIONS.map((qa) => (
								<button
									key={qa}
									className="agent-quick-btn"
									onClick={() => handleSend(qa)}
								>
									{qa}
								</button>
							))}
						</div>
					</div>
				)}

				{messages.map((msg, i) => (
					<div key={i} className={`agent-msg agent-msg-${msg.role}`}>
						{msg.role === "user" ? (
							<div className="agent-bubble user-bubble">{msg.text}</div>
						) : (
							<div className="agent-bubble bot-bubble">
								{msg.blocks.map((block, bi) => (
									<BlockRenderer key={`${i}-${bi}`} block={block} />
								))}
								{msg.streaming && msg.blocks.length === 0 && (
									<div className="agent-typing">
										<span className="typing-dot" />
										<span className="typing-dot" />
										<span className="typing-dot" />
									</div>
								)}
							</div>
						)}
					</div>
				))}
				<div ref={messagesEndRef} />
			</div>

			{/* Input */}
			<div className="agent-input-area">
				<textarea
					ref={textareaRef}
					className="agent-textarea"
					placeholder="Ask the agent... (/clear to reset)"
					value={input}
					onChange={(e) => setInput(e.target.value)}
					onKeyDown={handleKeyDown}
					rows={1}
					disabled={isStreaming}
				/>
				<button
					className="agent-send-btn"
					onClick={() => (isStreaming ? stopStreaming() : handleSend())}
					disabled={!isStreaming && !input.trim()}
					title={isStreaming ? "Stop" : "Send"}
				>
					{isStreaming ? "\u25A0" : "\u2191"}
				</button>
			</div>
		</div>
	);
}

/* ── Block Renderer ─────────────────────────────────────── */

function BlockRenderer({ block }: { block: Block }) {
	if (block.type === "thinking") {
		return (
			<div className="block-thinking">
				<span className="thinking-icon">&bull;</span>
				<span className="thinking-text">{block.text}</span>
			</div>
		);
	}

	if (block.type === "step") {
		return (
			<div className={`block-step step-${block.status}`}>
				<span className="step-icon">
					{block.status === "running"
						? "\u23F3"
						: block.status === "done"
							? "\u2705"
							: "\u274C"}
				</span>
				<div className="step-content">
					<span className="step-label">
						{block.status === "done" && block.summary
							? block.summary
							: block.label}
					</span>
					{block.status === "done" && block.imageUrl && (
						<img
							src={block.imageUrl}
							alt="Captured"
							className="chat-image"
							onClick={() => window.open(block.imageUrl, "_blank")}
						/>
					)}
				</div>
			</div>
		);
	}

	if (block.type === "error") {
		return (
			<div className="block-error">
				<MarkdownContent text={block.text} />
			</div>
		);
	}

	return (
		<div className="block-text">
			<MarkdownContent text={block.text} />
		</div>
	);
}

/* ── Markdown Renderer ──────────────────────────────────── */

function MarkdownContent({ text }: { text: string }) {
	const parts = parseMarkdown(text);
	return <div className="agent-reply">{parts}</div>;
}

function parseMarkdown(text: string): React.ReactNode[] {
	const nodes: React.ReactNode[] = [];
	const lines = text.split("\n");
	let tableRows: string[][] = [];

	const flushTable = () => {
		if (tableRows.length === 0) return;
		const rows = tableRows.filter(
			(row) => !row.every((cell) => /^-+$/.test(cell.trim())),
		);
		nodes.push(
			<table key={`t-${nodes.length}`} className="agent-table">
				<tbody>
					{rows.map((row, ri) => (
						<tr key={ri}>
							{row.map((cell, ci) => (
								<td key={ci}>{inlineFormat(cell.trim())}</td>
							))}
						</tr>
					))}
				</tbody>
			</table>,
		);
		tableRows = [];
	};

	for (let i = 0; i < lines.length; i++) {
		const line = lines[i];

		if (line.startsWith("|") && line.endsWith("|")) {
			const cells = line
				.slice(1, -1)
				.split("|")
				.map((c) => c.trim());
			tableRows.push(cells);
			continue;
		}

		flushTable();

		// Markdown image: ![alt](url)
		const imgMatch = line.match(/^!\[([^\]]*)\]\(([^)]+)\)$/);
		if (imgMatch) {
			const imgSrc = imgMatch[2];
			nodes.push(
				<img
					key={`img-${i}`}
					src={imgSrc}
					alt={imgMatch[1] || "Captured"}
					className="chat-image chat-image-block"
					onClick={() => window.open(imgSrc, "_blank")}
				/>,
			);
			continue;
		}

		if (line.trim() === "") {
			nodes.push(<br key={`br-${i}`} />);
		} else {
			nodes.push(
				<span key={`l-${i}`}>
					{inlineFormat(line)}
					{i < lines.length - 1 && <br />}
				</span>,
			);
		}
	}

	flushTable();
	return nodes;
}

function inlineFormat(text: string): React.ReactNode[] {
	const parts: React.ReactNode[] = [];
	const regex = /(\*\*(.+?)\*\*|\*(.+?)\*|`(.+?)`)/g;
	let lastIndex = 0;
	let match: RegExpExecArray | null = regex.exec(text);

	while (match !== null) {
		if (match.index > lastIndex) {
			parts.push(text.slice(lastIndex, match.index));
		}

		if (match[2]) {
			parts.push(<strong key={`b-${match.index}`}>{match[2]}</strong>);
		} else if (match[3]) {
			parts.push(<em key={`i-${match.index}`}>{match[3]}</em>);
		} else if (match[4]) {
			parts.push(<code key={`c-${match.index}`}>{match[4]}</code>);
		}

		lastIndex = regex.lastIndex;
		match = regex.exec(text);
	}

	if (lastIndex < text.length) {
		parts.push(text.slice(lastIndex));
	}

	return parts.length > 0 ? parts : [text];
}
