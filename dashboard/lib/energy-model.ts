// Energy model — relocated verbatim from app/board/[id]/page.tsx. These
// constants and formulas are mirrored EXACTLY from scripts/energy_model.py
// (the thesis RQ3 authoritative model); do not change the numbers here
// without updating that script too, or the two will silently disagree.

// ── Energy model constants — mirrored exactly from scripts/energy_model.py ──
// Datasheet current figures (typical @ ~3.0-3.3 V)
export const I_MCU_RUN_MA = 19.0; // STM32U585 Run @160MHz
export const I_MCU_STOP2_UA = 2.0; // STOP2 + RTC (bench-verified ~2 uA)
export const I_CAM_ACTIVE_MA = 150.0; // OV5640 active capture
export const I_CAM_OFF_UA = 20.0; // OV5640 power-down
export const I_WIFI_ACTIVE_MA = 230.0; // EMW3080 connected + uploading
export const I_WIFI_PS_MA = 3.0; // EMW3080 802.11 power-save / DTIM-sleep
// Note: I_WIFI_OFF_UA (10.0 uA) is used by DEEP_DORMANT mode only; not in measured path
export const VOLTAGE_V = 3.3;
export const BATTERY_MAH = 2000.0;

export function activeBurstCurrentMa(): number {
	return I_MCU_RUN_MA + I_CAM_ACTIVE_MA + I_WIFI_ACTIVE_MA;
}

export function wifiPsRestCurrentMa(): number {
	return I_WIFI_PS_MA + (I_MCU_STOP2_UA + I_CAM_OFF_UA) / 1000.0;
}

export function continuousDailyJ(): number {
	const iA = activeBurstCurrentMa() / 1000.0;
	return iA * VOLTAGE_V * 86400.0;
}

export interface MeasuredEnergy {
	fRest: number;
	fCap: number;
	fOther: number;
	avgMa: number;
	dailyJ: number;
	batteryDays: number;
	savingsRatio: number;
}

export function measuredDailyEnergy(
	totalWindowMs: number,
	totalPsRestMs: number,
	totalCaptureMs: number,
): MeasuredEnergy | null {
	if (totalWindowMs <= 0) return null;
	const fRest = totalPsRestMs / totalWindowMs;
	const fCap = totalCaptureMs / totalWindowMs;
	const fOther = Math.max(1.0 - fRest - fCap, 0.0);
	const iRest = wifiPsRestCurrentMa();
	const iCap = activeBurstCurrentMa();
	const iOther = I_MCU_RUN_MA + I_WIFI_PS_MA; // awake but not capturing (camera off)
	const avgMa = fRest * iRest + fCap * iCap + fOther * iOther;
	const dailyJ = (avgMa / 1000.0) * VOLTAGE_V * 86400.0;
	const batteryJ = (BATTERY_MAH / 1000.0) * VOLTAGE_V * 3600.0;
	const batteryDays = batteryJ / Math.max(dailyJ, 1e-9);
	const savingsRatio = continuousDailyJ() / Math.max(dailyJ, 1e-9);
	return { fRest, fCap, fOther, avgMa, dailyJ, batteryDays, savingsRatio };
}
