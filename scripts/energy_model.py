"""RQ3 energy model -- state-time accounting for scheduled vs continuous capture.

Methodology (no power meter required): the energy of the deployable IoT node
(STM32U585 + OV5640 camera + EMW3080 WiFi) is computed as the sum over power
states of (time-in-state x current-in-state x voltage). The dominant difference
between the two operating modes is the DUTY CYCLE -- how long the node spends in
the active capture burst vs the STOP2 sleep floor -- which the firmware measures
directly. Absolute currents come from component datasheets with a stated
tolerance band; the headline saving ratio is robust to datasheet error because
it is dominated by the time difference, not the absolute current.

This is "energy modeling from state-time accounting", an established methodology
in wireless-sensor-network / IoT energy research where per-trace power profiling
hardware is unavailable. It models the DEPLOYABLE system (MCU + peripherals),
deliberately excluding the B-U585I-IOT02A Discovery board's ST-LINK debugger and
support circuitry, which would not exist in a fielded product.

Two configurations compared:
  - CONTINUOUS: camera + WiFi held active, capturing back-to-back. Node is
    effectively always in the active-burst power state.
  - SCHEDULED:  node sleeps in STOP2 (RTC-wake) between capture bursts; one
    burst per capture interval, sleep for the remainder.

Usage:
  python scripts/energy_model.py                       # default 30-min interval
  python scripts/energy_model.py --interval-min 60     # hourly schedule
  python scripts/energy_model.py --burst-s 7.0 --interval-min 30 --battery-mah 2000
"""
from __future__ import annotations

import argparse


# -----------------------------------------------------------------------------
# Datasheet current figures (typical @ ~3.0-3.3 V). CITE THESE IN THE THESIS.
# Each value carries a tolerance used for the sensitivity band.
# -----------------------------------------------------------------------------

# STM32U585 (DS13086, ultra-low-power Cortex-M33). Source: ST datasheet tables.
#   Run mode @ 160 MHz, peripherals active: ~ tens of mA (range-mode dependent).
#   STOP2 with RTC + RAM retention: ~1.6-3 uA typ. We bench-verified ~2 uA.
I_MCU_RUN_MA = 19.0        # STM32U585 Run @160MHz, representative (datasheet typ.)
I_MCU_STOP2_UA = 2.0       # STOP2 + RTC, BENCH-VERIFIED on this board (~2 uA)

# OV5640 5 MP camera (OmniVision datasheet).
#   Active (streaming/capturing): ~140-200 mA. Power-down (between captures): ~uA.
I_CAM_ACTIVE_MA = 150.0    # OV5640 active capture (datasheet typ.)
I_CAM_OFF_UA = 20.0        # OV5640 power-down / hardware standby

# EMW3080 (MXCHIP, the WiFi module on B-U585I-IOT02A). Module datasheet.
#   TX active burst: ~200-300 mA. Associated/RX idle: ~50-80 mA. Off: ~uA.
#   During an upload the module is dominated by TX. We model the burst-average.
#   802.11 power-save (DTIM-sleep, still associated): ~1-5 mA; 3 mA nominal.
I_WIFI_ACTIVE_MA = 230.0   # EMW3080 connected + uploading, burst-average (datasheet)
I_WIFI_PS_MA = 3.0         # EMW3080 in 802.11 power-save / DTIM-sleep (still associated)
I_WIFI_OFF_UA = 10.0       # EMW3080 powered down (DEEP_DORMANT mode)

VOLTAGE_V = 3.3            # system rail

# Tolerance applied to every ACTIVE current for the sensitivity band (+/-).
ACTIVE_TOLERANCE_PCT = 30.0


# -----------------------------------------------------------------------------
# Derived power states (mA) -- what the node actually draws in each phase.
# -----------------------------------------------------------------------------

def active_burst_current_ma(scale: float = 1.0) -> float:
    """Total node current during a capture burst (MCU run + camera + WiFi all on).

    scale lets the sensitivity analysis perturb all active currents together.
    """
    return scale * (I_MCU_RUN_MA + I_CAM_ACTIVE_MA + I_WIFI_ACTIVE_MA)


def sleep_current_ma() -> float:
    """Total node current in DEEP_DORMANT (MCU STOP2 + camera off + WiFi fully off)."""
    return (I_MCU_STOP2_UA + I_CAM_OFF_UA + I_WIFI_OFF_UA) / 1000.0


def wifi_ps_rest_current_ma() -> float:
    """Total node current in WIFI_PS_REST (MCU STOP2 + WiFi-PS associated + camera off).

    This is the DEFAULT rest state: WiFi stays associated so the agent can reach the
    board sub-second via the NOTIFY/PD14 line. MCU and camera are off; WiFi radio
    DTIM-sleeps between beacons (~3 mA dominated by WiFi-PS).
    """
    return I_WIFI_PS_MA + (I_MCU_STOP2_UA + I_CAM_OFF_UA) / 1000.0


# -----------------------------------------------------------------------------
# Energy computations (Joules). E = I[A] * V * t[s].
# -----------------------------------------------------------------------------

def burst_energy_j(burst_s: float, active_scale: float = 1.0) -> float:
    i_a = active_burst_current_ma(active_scale) / 1000.0
    return i_a * VOLTAGE_V * burst_s


def wifi_ps_rest_daily_j(burst_s: float, interval_min: float, active_scale: float = 1.0) -> dict:
    """Energy per 24 h in WIFI_PS_REST mode: burst + WiFi-PS rest between captures.

    The default operating mode: board rests with WiFi associated (sub-second
    wake-on-ping possible). Between bursts the dominant consumer is WiFi-PS (~3 mA).
    """
    interval_s = interval_min * 60.0
    bursts_per_day = 86400.0 / interval_s
    e_burst_total = bursts_per_day * burst_energy_j(burst_s, active_scale)

    rest_s_per_day = 86400.0 - bursts_per_day * burst_s
    i_rest_a = wifi_ps_rest_current_ma() / 1000.0
    e_rest_total = i_rest_a * VOLTAGE_V * rest_s_per_day

    return {
        "bursts_per_day": bursts_per_day,
        "active_j": e_burst_total,
        "rest_j": e_rest_total,
        "total_j": e_burst_total + e_rest_total,
    }


def scheduled_daily_j(burst_s: float, interval_min: float, active_scale: float = 1.0) -> dict:
    """Energy per 24 h in DEEP_DORMANT mode: burst + WiFi-off STOP2 between captures.

    Opt-in mode: WiFi fully deinitialized between captures, ~2 uA floor.
    No agent responsiveness during dormancy; board wakes only on scheduled RTC alarm.
    """
    interval_s = interval_min * 60.0
    bursts_per_day = 86400.0 / interval_s
    e_burst_total = bursts_per_day * burst_energy_j(burst_s, active_scale)

    sleep_s_per_day = 86400.0 - bursts_per_day * burst_s
    i_sleep_a = sleep_current_ma() / 1000.0
    e_sleep_total = i_sleep_a * VOLTAGE_V * sleep_s_per_day

    return {
        "bursts_per_day": bursts_per_day,
        "active_j": e_burst_total,
        "sleep_j": e_sleep_total,
        "total_j": e_burst_total + e_sleep_total,
    }


def continuous_daily_j(active_scale: float = 1.0) -> dict:
    """Energy per 24 h in CONTINUOUS mode: node held in active-burst state all day."""
    i_a = active_burst_current_ma(active_scale) / 1000.0
    total = i_a * VOLTAGE_V * 86400.0
    return {"total_j": total}


def battery_life_days(daily_j: float, battery_mah: float) -> float:
    """Days of runtime on a battery of given mAh at the system voltage."""
    battery_j = (battery_mah / 1000.0) * VOLTAGE_V * 3600.0  # mAh -> Ah -> C -> J
    daily_j = max(daily_j, 1e-9)
    return battery_j / daily_j


# -----------------------------------------------------------------------------
# Reporting
# -----------------------------------------------------------------------------

def fmt_j(j: float) -> str:
    if j >= 1000:
        return f"{j/1000:.2f} kJ"
    return f"{j:.2f} J"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--burst-s", type=float, default=7.0,
                    help="Measured active-burst duration (s). Firmware [PERF] log ~7s. Default 7.0")
    ap.add_argument("--interval-min", type=float, default=30.0,
                    help="Scheduled capture interval (min). Default 30.")
    ap.add_argument("--battery-mah", type=float, default=2000.0,
                    help="Battery capacity for runtime projection (mAh). Default 2000 (e.g. 18650-ish).")
    args = ap.parse_args()

    # Validate at the CLI boundary: the duty-cycle model is undefined for a
    # non-positive interval (division by zero) or a burst that meets/exceeds the
    # interval (a node cannot capture longer than its own schedule period).
    interval_s = args.interval_min * 60.0
    if args.interval_min <= 0:
        ap.error(f"--interval-min must be > 0 (got {args.interval_min})")
    if args.burst_s <= 0:
        ap.error(f"--burst-s must be > 0 (got {args.burst_s})")
    if args.burst_s >= interval_s:
        ap.error(
            f"--burst-s ({args.burst_s:.0f}s) must be < the capture interval "
            f"({interval_s:.0f}s = {args.interval_min:.0f} min); a burst cannot "
            f"exceed its own schedule period."
        )

    print("=" * 72)
    print("RQ3 ENERGY MODEL -- scheduled vs continuous capture")
    print("State-time accounting; deployable node = STM32U585 + OV5640 + EMW3080")
    print("=" * 72)
    print(f"\nParameters:")
    print(f"  capture burst duration : {args.burst_s:.1f} s   (firmware [PERF] log)")
    print(f"  scheduled interval     : {args.interval_min:.0f} min")
    print(f"  system voltage         : {VOLTAGE_V} V")
    print(f"  battery for projection : {args.battery_mah:.0f} mAh")

    print(f"\nPower states (modeled, datasheet-derived):")
    print(f"  active burst           : {active_burst_current_ma():.1f} mA "
          f"(MCU {I_MCU_RUN_MA} + cam {I_CAM_ACTIVE_MA} + wifi {I_WIFI_ACTIVE_MA})")
    print(f"  WIFI_PS_REST rest      : {wifi_ps_rest_current_ma():.2f} mA "
          f"(WiFi-PS {I_WIFI_PS_MA:.1f} mA + MCU {I_MCU_STOP2_UA} uA + cam {I_CAM_OFF_UA} uA)")
    print(f"  DEEP_DORMANT sleep     : {sleep_current_ma()*1000:.1f} uA "
          f"(MCU {I_MCU_STOP2_UA} + cam {I_CAM_OFF_UA} + wifi {I_WIFI_OFF_UA} uA)")

    ps  = wifi_ps_rest_daily_j(args.burst_s, args.interval_min)
    sched = scheduled_daily_j(args.burst_s, args.interval_min)
    cont = continuous_daily_j()
    ratio_ps   = cont["total_j"] / ps["total_j"]
    ratio_sched = cont["total_j"] / sched["total_j"]

    print(f"\n-- 24-hour energy ({args.burst_s:.0f} s burst, {args.interval_min:.0f} min interval) --")
    print(f"  {'Mode':<20}  {'Energy/day':>12}  {'Battery life':>14}  {'vs continuous':>14}  Wake latency")
    print(f"  {'-'*20}  {'-'*12}  {'-'*14}  {'-'*14}  {'-'*20}")

    cont_days = battery_life_days(cont["total_j"], args.battery_mah)
    ps_days   = battery_life_days(ps["total_j"],   args.battery_mah)
    sched_days = battery_life_days(sched["total_j"], args.battery_mah)

    def batt(d: float) -> str:
        if d >= 30:
            return f"{d/30.4:.1f} months"
        return f"{d:.1f} days"

    print(f"  {'CONTINUOUS':<20}  {fmt_j(cont['total_j']):>12}/day  "
          f"{batt(cont_days):>14}  {'baseline':>14}  n/a (always on)")
    print(f"  {'WIFI_PS_REST':<20}  {fmt_j(ps['total_j']):>12}/day  "
          f"{batt(ps_days):>14}  {ratio_ps:>12.0f}x  0.1-2.6 s (NOTIFY)")
    print(f"  {'DEEP_DORMANT':<20}  {fmt_j(sched['total_j']):>12}/day  "
          f"{batt(sched_days):>14}  {ratio_sched:>12.0f}x  until RTC alarm / B3")

    # Sensitivity band: perturb all active currents +/- tolerance; the ratio is
    # robust because sleep energy is negligible vs active in BOTH numerator and
    # denominator -- the ratio is dominated by the duty cycle (burst/interval).
    print(f"\n-- Sensitivity (active currents +/-{ACTIVE_TOLERANCE_PCT:.0f}%, DEEP_DORMANT ratio) --")
    lo = 1.0 - ACTIVE_TOLERANCE_PCT / 100.0
    hi = 1.0 + ACTIVE_TOLERANCE_PCT / 100.0
    for label, scale in (("low", lo), ("nominal", 1.0), ("high", hi)):
        s = scheduled_daily_j(args.burst_s, args.interval_min, scale)
        c = continuous_daily_j(scale)
        r = c["total_j"] / s["total_j"]
        print(f"  active x{scale:.2f} ({label:<7}): DEEP_DORMANT ratio = {r:.1f}x")

    duty_pct = args.burst_s / (args.interval_min * 60) * 100
    print(f"\n-- Interpretation --")
    print(f"  Duty cycle: {args.burst_s:.0f} s burst / {args.interval_min:.0f} min = {duty_pct:.2f}% active.")
    print(f"  WIFI_PS_REST  (~{ratio_ps:.0f}x): WiFi stays associated; sub-second wake-on-ping.")
    print(f"  DEEP_DORMANT  (~{ratio_sched:.0f}x): WiFi off; maximum savings; latency until scheduled wake.")
    print(f"  The DEEP_DORMANT ratio is dominated by duty cycle; robust to +/-{ACTIVE_TOLERANCE_PCT:.0f}% current error.")
    print(f"  All Joule figures carry +/-{ACTIVE_TOLERANCE_PCT:.0f}% datasheet uncertainty; the ratio does not.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
