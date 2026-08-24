"use client";

import {
	activeBurstCurrentMa,
	BATTERY_MAH,
	I_MCU_RUN_MA,
	I_WIFI_PS_MA,
	measuredDailyEnergy,
	wifiPsRestCurrentMa,
} from "@/lib/energy-model";
import { fmtMs } from "@/lib/format";
import type { ActionFeedbackState, EnergyTotals, PowerMode } from "@/lib/types";
import ActionFeedback from "../common/ActionFeedback";
import buttons from "../common/buttons.module.css";
import ErrorBanner from "../common/ErrorBanner";
import styles from "./EnergyPanel.module.css";

export interface EnergyPanelProps {
	energy: EnergyTotals;
	powerMode: PowerMode;
	hasSchedule: boolean;
	actionLoading: string | null;
	onSetPowerMode: (mode: "active" | "ps_rest" | "sleep") => void;
	onReset: () => void;
	energyError: string | null;
	onRetryEnergy: () => void;
	feedbackPower?: ActionFeedbackState;
	feedbackReset?: ActionFeedbackState;
}

const POWER_MODES = [
	{ id: "active", label: "Active", current: "~22 mA" },
	{ id: "ps_rest", label: "PS-REST", current: "~3 mA" },
	{ id: "sleep", label: "Sleep", current: "~2 µA" },
] as const;

export default function EnergyPanel({
	energy,
	powerMode,
	hasSchedule,
	actionLoading,
	onSetPowerMode,
	onReset,
	energyError,
	onRetryEnergy,
	feedbackPower,
	feedbackReset,
}: EnergyPanelProps) {
	const m = measuredDailyEnergy(
		energy.totalWindowMs,
		energy.totalPsRestMs,
		energy.totalCaptureMs,
	);
	// Per-state current draws (mA) used by the model — shown inline so each
	// duty-split slice is self-explanatory.
	const iRest = wifiPsRestCurrentMa(); // STOP2 + Wi-Fi power-save
	const iCap = activeBurstCurrentMa(); // MCU + camera + Wi-Fi upload
	const iOther = I_MCU_RUN_MA + I_WIFI_PS_MA; // awake but not capturing (camera off)

	const modeDesc: Record<PowerMode, string> = {
		unknown: "Syncing power mode from the board\u2026",
		active:
			"Always awake & connected. Highest power — use for setup and live work.",
		ps_rest:
			"MCU sleeps in STOP2 between ~3 s polls; Wi-Fi stays connected in 802.11 power-save (~3 mA). Stays reachable. This is the RQ3 energy-measurement mode — a window is reported every ~60 s.",
		sleep:
			"Deep dormant: Wi-Fi + camera off (~2 µA). The board is UNREACHABLE and wakes only at the next scheduled capture or a physical B3 press.",
	};

	return (
		<div className={styles.energyPanel}>
			{energyError && (
				<ErrorBanner
					compact
					message={`Couldn't load energy totals: ${energyError}`}
					onRetry={onRetryEnergy}
				/>
			)}
			{/* Power mode — mutually exclusive segmented control */}
			<fieldset className={styles.powerModeControl}>
				<legend className={styles.energyLabel}>Power mode</legend>
				<div className={styles.powerModeSegments}>
					{POWER_MODES.map((opt) => (
						<button
							type="button"
							key={opt.id}
							className={`${styles.powerSeg}${powerMode === opt.id ? ` ${styles.active}` : ""}`}
							onClick={() => onSetPowerMode(opt.id)}
							disabled={actionLoading !== null || powerMode === opt.id}
							aria-pressed={powerMode === opt.id}
						>
							<span className={styles.powerSegLabel}>{opt.label}</span>
							<span className={styles.powerSegCurrent}>{opt.current}</span>
						</button>
					))}
				</div>
				<div className={styles.powerModeDesc}>
					{actionLoading === "power"
						? "Switching power mode\u2026"
						: modeDesc[powerMode]}
				</div>
				<ActionFeedback feedback={feedbackPower} />
				{powerMode === "sleep" && !hasSchedule && (
					<div className={styles.powerModeWarn}>
						⚠ No schedule set — the board will stay dormant until you press the
						physical B3 button. Add a schedule so it wakes itself to capture.
					</div>
				)}
			</fieldset>

			{energy.windows > 0 && (
				<div className={styles.energyActionsRow}>
					<span className={`${styles.energySectionSub} ${styles.rqLabel}`}>
						RQ3 energy — measured on-device while in PS-REST.
					</span>
					<button
						type="button"
						className={buttons.btnAction}
						onClick={onReset}
						disabled={actionLoading !== null}
						title="Clear stored energy windows and start a fresh measurement run"
					>
						{actionLoading === "energy-reset" ? "Resetting…" : "Reset run"}
					</button>
					<ActionFeedback feedback={feedbackReset} />
				</div>
			)}

			{energy.windows === 0 ? (
				<div className={styles.energyEmpty}>
					No energy telemetry yet — enable PS-REST to start the run.
				</div>
			) : (
				<>
					{/* Summary row */}
					<div className={styles.energySummaryRow}>
						<div className={styles.energyStat}>
							<span className={styles.energyStatValue}>{energy.windows}</span>
							<span className={styles.energyStatLabel}>windows</span>
							<span className={styles.energyHelp}>
								60&nbsp;s wall-clock windows the board has reported this run
							</span>
						</div>
						<div className={styles.energyStat}>
							<span className={styles.energyStatValue}>
								{fmtMs(energy.totalWindowMs)}
							</span>
							<span className={styles.energyStatLabel}>observed</span>
							<span className={styles.energyHelp}>
								total real time measured (sum of all windows)
							</span>
						</div>
						<div className={styles.energyStat}>
							<span className={styles.energyStatValue}>
								{energy.lastUpdate
									? new Date(energy.lastUpdate).toLocaleTimeString("en-GB", {
											hour: "2-digit",
											minute: "2-digit",
											second: "2-digit",
										})
									: "\u2014"}
							</span>
							<span className={styles.energyStatLabel}>last window</span>
							<span className={styles.energyHelp}>
								clock time the newest window arrived
							</span>
						</div>
					</div>

					{/* Duty split */}
					{m && (
						<>
							<div className={styles.energySectionTitle}>
								Measured duty split
							</div>
							<div className={styles.energySectionSub}>
								Share of real (wall-clock) time the board spent in each power
								state — measured on-device via the RTC, not estimated. These
								three add up to 100%.
							</div>
							<div className={styles.energyDutyBar}>
								<div
									className={`${styles.energyDutySeg} ${styles.energyDutyRest}`}
									style={{ width: `${m.fRest * 100}%` }}
									title={`PS-REST ${(m.fRest * 100).toFixed(1)}% — STOP2 sleep + Wi-Fi power-save, ~${iRest.toFixed(1)} mA`}
								/>
								<div
									className={`${styles.energyDutySeg} ${styles.energyDutyCap}`}
									style={{ width: `${m.fCap * 100}%` }}
									title={`Capture ${(m.fCap * 100).toFixed(2)}% — camera + Wi-Fi upload active, ~${iCap.toFixed(0)} mA`}
								/>
								<div
									className={`${styles.energyDutySeg} ${styles.energyDutyOther}`}
									style={{ width: `${m.fOther * 100}%` }}
									title={`Other ${(m.fOther * 100).toFixed(1)}% — awake & idle, camera off, ~${iOther.toFixed(0)} mA`}
								/>
							</div>
							<div className={styles.energyDutyLegend}>
								<span
									className={`${styles.energyLegendItem} ${styles.energyLegendRest}`}
								>
									PS-REST {(m.fRest * 100).toFixed(1)}%
									<span className={styles.energyLegendHelp}>
										asleep (STOP2) + Wi-Fi power-save · ~{iRest.toFixed(1)} mA
									</span>
								</span>
								<span
									className={`${styles.energyLegendItem} ${styles.energyLegendCap}`}
								>
									Capture {(m.fCap * 100).toFixed(2)}%
									<span className={styles.energyLegendHelp}>
										camera + Wi-Fi upload · ~{iCap.toFixed(0)} mA
									</span>
								</span>
								<span
									className={`${styles.energyLegendItem} ${styles.energyLegendOther}`}
								>
									Other {(m.fOther * 100).toFixed(1)}%
									<span className={styles.energyLegendHelp}>
										awake & idle, camera off · ~{iOther.toFixed(0)} mA
									</span>
								</span>
							</div>

							{/* Indicative projection */}
							<div className={styles.energySectionTitle}>
								Indicative projection{" "}
								<span className={styles.energySectionNote}>
									(authoritative: <code>energy_model.py</code>)
								</span>
							</div>
							<div className={styles.energySectionSub}>
								The measured duty split above, extrapolated to a full day using
								datasheet component currents at 3.3&nbsp;V. Indicative only —
								the thesis figures come from <code>energy_model.py</code>.
							</div>
							<div className={styles.energyProjectionGrid}>
								<div className={styles.energyProjItem}>
									<span className={styles.energyProjValue}>
										{m.avgMa.toFixed(2)} mA
									</span>
									<span className={styles.energyProjLabel}>avg current</span>
									<span className={styles.energyHelp}>
										duty-weighted mean draw at 3.3&nbsp;V
									</span>
								</div>
								<div className={styles.energyProjItem}>
									<span className={styles.energyProjValue}>
										{m.dailyJ.toFixed(1)} J
									</span>
									<span className={styles.energyProjLabel}>energy / day</span>
									<span className={styles.energyHelp}>
										= avg current × 3.3&nbsp;V × 24&nbsp;h
									</span>
								</div>
								<div className={styles.energyProjItem}>
									<span className={styles.energyProjValue}>
										{m.batteryDays >= 30
											? `${(m.batteryDays / 30.4).toFixed(1)} mo`
											: `${m.batteryDays.toFixed(1)} d`}
									</span>
									<span className={styles.energyProjLabel}>
										battery ({BATTERY_MAH.toFixed(0)} mAh)
									</span>
									<span className={styles.energyHelp}>
										projected runtime on one {BATTERY_MAH.toFixed(0)}&nbsp;mAh
										cell
									</span>
								</div>
								<div className={styles.energyProjItem}>
									<span
										className={`${styles.energyProjValue} ${styles.energySavings}`}
									>
										{m.savingsRatio.toFixed(0)}x
									</span>
									<span className={styles.energyProjLabel}>vs continuous</span>
									<span className={styles.energyHelp}>
										longer than always-on capture (no sleep)
									</span>
								</div>
							</div>
						</>
					)}
				</>
			)}
		</div>
	);
}
