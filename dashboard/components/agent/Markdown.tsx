"use client";

import type { ReactNode } from "react";
import chatImage from "./chatImage.module.css";
import styles from "./Markdown.module.css";

export function MarkdownContent({ text }: { text: string }) {
	const parts = parseMarkdown(text);
	return <div className={styles.agentReply}>{parts}</div>;
}

function parseMarkdown(text: string): ReactNode[] {
	const nodes: ReactNode[] = [];
	const lines = text.split("\n");
	let tableRows: string[][] = [];

	const flushTable = () => {
		if (tableRows.length === 0) return;
		const rows = tableRows.filter(
			(row) => !row.every((cell) => /^-+$/.test(cell.trim())),
		);
		nodes.push(
			<table key={`t-${nodes.length}`} className={styles.agentTable}>
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
					className={`${chatImage.chatImage} ${chatImage.chatImageBlock}`}
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

function inlineFormat(text: string): ReactNode[] {
	const parts: ReactNode[] = [];
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
