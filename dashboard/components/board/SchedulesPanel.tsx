"use client";

import type {
	ActionFeedbackState,
	EditingSchedule,
	Schedule,
	TaskLiveStatus,
} from "@/lib/types";
import ErrorBanner from "../common/ErrorBanner";
import ScheduleCard from "./ScheduleCard";
import styles from "./SchedulesPanel.module.css";

export default function SchedulesPanel({
	schedules,
	loading,
	error,
	onRetry,
	taskStatuses,
	actionLoading,
	feedback,
	onActivate,
	onDeactivate,
	onEdit,
	onDelete,
}: {
	schedules: Schedule[];
	loading: boolean;
	error: string | null;
	onRetry: () => void;
	taskStatuses: Record<number, TaskLiveStatus>;
	actionLoading: string | null;
	feedback: Record<string, ActionFeedbackState>;
	onActivate: (scheduleId: number) => void;
	onDeactivate: (scheduleId: number) => void;
	onEdit: (draft: EditingSchedule) => void;
	onDelete: (scheduleId: number, name: string) => void;
}) {
	return (
		<div className={styles.schedulesList}>
			{error && (
				<ErrorBanner
					compact
					message={`Couldn't load schedules: ${error}`}
					onRetry={onRetry}
				/>
			)}
			{schedules.length === 0 ? (
				<div className={styles.emptyStateSm}>
					{loading
						? "Loading schedules\u2026"
						: "No schedules yet. Ask the agent to create one."}
				</div>
			) : (
				schedules.map((sched) => (
					<ScheduleCard
						key={sched.id}
						schedule={sched}
						taskStatuses={taskStatuses}
						actionLoading={actionLoading}
						feedback={feedback[`schedule-${sched.id}`]}
						onActivate={() => onActivate(sched.id)}
						onDeactivate={() => onDeactivate(sched.id)}
						onEdit={() =>
							onEdit({
								id: sched.id,
								name: sched.name,
								description: sched.description || "",
								tasks: (sched.tasks || []).map((t) => ({
									time: t.time,
									objective: t.objective || t.action || "",
								})),
							})
						}
						onDelete={() => onDelete(sched.id, sched.name)}
					/>
				))
			)}
		</div>
	);
}
