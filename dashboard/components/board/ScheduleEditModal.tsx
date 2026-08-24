"use client";

import type { ActionFeedbackState, EditingSchedule } from "@/lib/types";
import ActionFeedback from "../common/ActionFeedback";
import buttons from "../common/buttons.module.css";
import { PlusIcon } from "../icons/Icons";
import styles from "./ScheduleEditModal.module.css";

export default function ScheduleEditModal({
	draft,
	onChange,
	onSave,
	onCancel,
	saving,
	feedback,
}: {
	draft: EditingSchedule;
	onChange: (updater: (prev: EditingSchedule) => EditingSchedule) => void;
	onSave: () => void;
	onCancel: () => void;
	saving: boolean;
	feedback?: ActionFeedbackState;
}) {
	return (
		<div className={styles.overlay} onClick={onCancel}>
			<div className={styles.modal} onClick={(e) => e.stopPropagation()}>
				<div className={styles.header}>
					<h3>Edit Schedule</h3>
					<button type="button" className={styles.closeBtn} onClick={onCancel}>
						&times;
					</button>
				</div>

				<div className={styles.field}>
					<label className={styles.label} htmlFor="sched-edit-name">
						Name
					</label>
					<input
						id="sched-edit-name"
						className={styles.input}
						value={draft.name}
						onChange={(e) =>
							onChange((prev) => ({ ...prev, name: e.target.value }))
						}
					/>
				</div>

				<div className={styles.tasks}>
					<div className={styles.tasksHeader}>
						<span className={styles.label}>Tasks</span>
						<button
							type="button"
							className={`${buttons.btnSchedAction} ${buttons.accent}`}
							onClick={() =>
								onChange((prev) => ({
									...prev,
									tasks: [...prev.tasks, { time: "09:00", objective: "" }],
								}))
							}
						>
							<PlusIcon size={10} /> Add
						</button>
					</div>
					{draft.tasks.map((task, idx) => (
						<div key={idx} className={styles.taskRow}>
							<input
								className={`${styles.input} ${styles.timeInput}`}
								type="time"
								value={task.time.substring(0, 5)}
								onChange={(e) =>
									onChange((prev) => {
										const tasks = [...prev.tasks];
										tasks[idx] = { ...tasks[idx], time: e.target.value };
										return { ...prev, tasks };
									})
								}
							/>
							<input
								className={`${styles.input} ${styles.flexOne}`}
								placeholder="Objective (e.g. Check if door is open)"
								value={task.objective}
								onChange={(e) =>
									onChange((prev) => {
										const tasks = [...prev.tasks];
										tasks[idx] = { ...tasks[idx], objective: e.target.value };
										return { ...prev, tasks };
									})
								}
							/>
							<button
								type="button"
								className={`${buttons.btnSchedAction} ${buttons.danger}`}
								onClick={() =>
									onChange((prev) => ({
										...prev,
										tasks: prev.tasks.filter((_, i) => i !== idx),
									}))
								}
							>
								×
							</button>
						</div>
					))}
				</div>

				<div className={styles.footer}>
					<ActionFeedback feedback={feedback} />
					<button
						type="button"
						className={`${buttons.btn} ${buttons.btnSecondary}`}
						onClick={onCancel}
					>
						Cancel
					</button>
					<button
						type="button"
						className={`${buttons.btn} ${buttons.btnPrimary}`}
						disabled={saving}
						onClick={onSave}
					>
						{saving ? "Saving…" : "Save Schedule"}
					</button>
				</div>
			</div>
		</div>
	);
}
