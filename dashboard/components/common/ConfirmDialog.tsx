"use client";

import buttons from "./buttons.module.css";
import styles from "./ConfirmDialog.module.css";

export interface ConfirmDialogProps {
	open: boolean;
	title: string;
	message: string;
	confirmLabel?: string;
	/** Styles the confirm button as destructive (red) vs. affirmative
	 * (accent). Every current use (delete image, delete schedule) is
	 * destructive, so this defaults to true. */
	danger?: boolean;
	onConfirm: () => void;
	onCancel: () => void;
}

/** Replaces native window.confirm() — a real, styled dialog instead of a
 * browser-chrome popup that can't be themed and reads as an unstyled
 * workaround in an otherwise polished UI. */
export default function ConfirmDialog({
	open,
	title,
	message,
	confirmLabel = "Confirm",
	danger = true,
	onConfirm,
	onCancel,
}: ConfirmDialogProps) {
	if (!open) return null;
	return (
		<div className={styles.overlay} onClick={onCancel}>
			<div className={styles.dialog} onClick={(e) => e.stopPropagation()}>
				<h3 className={styles.title}>{title}</h3>
				<p className={styles.message}>{message}</p>
				<div className={styles.actions}>
					<button
						type="button"
						className={`${buttons.btn} ${buttons.btnSecondary}`}
						onClick={onCancel}
					>
						Cancel
					</button>
					<button
						type="button"
						className={`${buttons.btn} ${danger ? buttons.btnDanger : buttons.btnPrimary}`}
						onClick={onConfirm}
					>
						{confirmLabel}
					</button>
				</div>
			</div>
		</div>
	);
}
