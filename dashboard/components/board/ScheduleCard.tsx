"use client";

import type {
	ActionFeedbackState,
	Schedule,
	TaskLiveStatus,
} from "@/lib/types";
import ActionFeedback from "../common/ActionFeedback";
import buttons from "../common/buttons.module.css";
import { CheckIcon, CircleIcon, XIcon } from "../icons/Icons";
import styles from "./ScheduleCard.module.css";

const RUNNING_STATUSES = [
	"executing",
	"camera_init",
	"capturing",
	"captured",
	"uploading",
];

export interface ScheduleCardProps {
	schedule: Schedule;
	taskStatuses: Record<number, TaskLiveStatus>;
	actionLoading: string | null;
	feedback?: ActionFeedbackState;
	onActivate: () => void;
	onDeactivate: () => void;
	onEdit: () => void;
	onDelete: () => void;
}

export default function ScheduleCard({
	schedule,
	taskStatuses,
	actionLoading,
	feedback,
	onActivate,
	onDeactivate,
	onEdit,
	onDelete,
}: ScheduleCardProps) {
	const tasks = schedule.tasks || [];
	const done = tasks.filter((t) => t.completed_at).length;
	const running = tasks.filter(
		(t) =>
			!t.completed_at &&
			taskStatuses[t.id] &&
			RUNNING_STATUSES.includes(taskStatuses[t.id].status),
	).length;
	const allDone = tasks.length > 0 && done === tasks.length;

	return (
		<div
			className={`${styles.card} ${schedule.is_active ? styles.active : ""} ${allDone ? styles.completed : ""}`}
		>
			<div className={styles.header}>
				<span className={styles.name}>{schedule.name}</span>
				{allDone ? (
					<span className={`${styles.badge} ${styles.done}`}>Completed</span>
				) : running > 0 ? (
					<span className={`${styles.badge} ${styles.running}`}>Running</span>
				) : schedule.is_active ? (
					<span className={styles.badge}>Active</span>
				) : (
					<span className={`${styles.badge} ${styles.inactive}`}>Inactive</span>
				)}
			</div>
			<div className={styles.progress}>
				<div
					className={styles.progressBar}
					style={{
						width: tasks.length ? `${(done / tasks.length) * 100}%` : "0%",
					}}
				/>
			</div>
			<div className={styles.tasks}>
				{tasks.map((task) => {
					const live = taskStatuses[task.id];
					const isRunning =
						!task.completed_at &&
						live &&
						RUNNING_STATUSES.includes(live.status);
					const isFailed =
						!task.completed_at &&
						live &&
						(live.status.includes("error") || live.status.includes("fail"));
					return (
						<div
							key={task.id}
							className={`${styles.task} ${task.completed_at ? styles.taskDone : ""} ${isRunning ? styles.taskRunning : ""} ${isFailed ? styles.taskFailed : ""}`}
						>
							<span className={styles.check}>
								{task.completed_at ? (
									<CheckIcon size={12} />
								) : isRunning ? (
									<span className={styles.spinner} title={live.status} />
								) : isFailed ? (
									<XIcon size={12} />
								) : (
									<CircleIcon size={12} />
								)}
							</span>
							<span className={styles.time}>{task.time}</span>
							<span className={styles.obj}>
								{task.objective || task.action}
							</span>
							{isRunning && (
								<span className={styles.statusLabel}>
									{live.status.replace("_", " ")}
								</span>
							)}
						</div>
					);
				})}
			</div>
			<div className={styles.actions}>
				{!schedule.is_active && !allDone && (
					<button
						type="button"
						className={`${buttons.btnSchedAction} ${buttons.accent}`}
						disabled={actionLoading !== null}
						onClick={onActivate}
					>
						{actionLoading === `activate-${schedule.id}` ? "…" : "Activate"}
					</button>
				)}
				{schedule.is_active && (
					<button
						type="button"
						className={buttons.btnSchedAction}
						disabled={actionLoading !== null}
						onClick={onDeactivate}
					>
						{actionLoading === `deactivate-${schedule.id}` ? "…" : "Deactivate"}
					</button>
				)}
				<button
					type="button"
					className={buttons.btnSchedAction}
					disabled={actionLoading !== null}
					onClick={onEdit}
				>
					Edit
				</button>
				<button
					type="button"
					className={`${buttons.btnSchedAction} ${buttons.danger}`}
					disabled={actionLoading !== null}
					onClick={onDelete}
				>
					{actionLoading === `delete-${schedule.id}` ? "…" : "Delete"}
				</button>
			</div>
			<ActionFeedback feedback={feedback} />
		</div>
	);
}
