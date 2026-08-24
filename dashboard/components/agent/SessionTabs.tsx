"use client";

import { PlusIcon, XIcon } from "../icons/Icons";
import styles from "./SessionTabs.module.css";

export interface DbSession {
	id: number;
	name: string;
	boardId: string;
	createdAt: string;
}

export default function SessionTabs({
	sessions,
	activeId,
	fullSize,
	onSwitch,
	onDelete,
	onCreate,
}: {
	sessions: DbSession[];
	activeId: number | null;
	fullSize?: boolean;
	onSwitch: (id: number) => void;
	onDelete: (id: number) => void;
	onCreate: () => void;
}) {
	return (
		<div className={`${styles.sessionTabs} ${fullSize ? styles.full : ""}`}>
			{sessions.map((s) => (
				<button
					type="button"
					key={s.id}
					className={`${styles.sessionTab} ${s.id === activeId ? styles.active : ""}`}
					onClick={() => onSwitch(s.id)}
				>
					<span className={styles.sessionTabName}>{s.name}</span>
					{sessions.length > 1 && (
						<span
							className={styles.sessionTabClose}
							onClick={(e) => {
								e.stopPropagation();
								onDelete(s.id);
							}}
						>
							<XIcon size={10} />
						</span>
					)}
				</button>
			))}
			<button
				type="button"
				className={`${styles.sessionTab} ${styles.sessionTabAdd}`}
				onClick={onCreate}
				title="New session"
			>
				<PlusIcon size={12} />
			</button>
		</div>
	);
}
