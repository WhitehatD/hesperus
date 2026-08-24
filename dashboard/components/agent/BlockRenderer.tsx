"use client";

import { CheckIcon, XIcon } from "../icons/Icons";
import styles from "./BlockRenderer.module.css";
import chatImage from "./chatImage.module.css";
import { MarkdownContent } from "./Markdown";

export type Block =
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

const STEP_CLASS: Record<Extract<Block, { type: "step" }>["status"], string> = {
	running: styles.stepRunning,
	done: styles.stepDone,
	error: styles.stepError,
};

export default function BlockRenderer({ block }: { block: Block }) {
	if (block.type === "thinking") {
		return (
			<div className={styles.blockThinking}>
				<span className={styles.thinkingIcon}>&bull;</span>
				<span>{block.text}</span>
			</div>
		);
	}

	if (block.type === "step") {
		return (
			<div className={`${styles.blockStep} ${STEP_CLASS[block.status]}`}>
				<span className={styles.stepIcon}>
					{block.status === "running" ? (
						<span className={styles.spinner} />
					) : block.status === "done" ? (
						<CheckIcon size={13} />
					) : (
						<XIcon size={13} />
					)}
				</span>
				<div className={styles.stepContent}>
					<span className={styles.stepLabel}>
						{block.status === "done" && block.summary
							? block.summary
							: block.label}
					</span>
					{block.status === "done" && block.imageUrl && (
						<img
							src={block.imageUrl}
							alt="Captured"
							className={chatImage.chatImage}
							onClick={() => window.open(block.imageUrl, "_blank")}
						/>
					)}
				</div>
			</div>
		);
	}

	if (block.type === "error") {
		return (
			<div className={styles.blockError}>
				<MarkdownContent text={block.text} />
			</div>
		);
	}

	return (
		<div className={styles.blockText}>
			<MarkdownContent text={block.text} />
		</div>
	);
}
