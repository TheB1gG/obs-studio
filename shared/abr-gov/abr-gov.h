/******************************************************************************
    Copyright (C) 2025 by OBS Project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tunable parameters for the shared ABR governor. The law is unit-agnostic: every rate value passed to
 * abr_gov_tick()/abr_gov_decide() must be in the same units (all three encoders use bits/sec at the
 * shared boundary; x265 converts its kbps settings to bps there). */
typedef struct abr_gov_config {
	double deadband_frac;        /* band around target before correcting, e.g. 0.005 = +/-0.5% */
	double overshoot_floor_frac; /* floor setpoint as a fraction of target, e.g. 0.975 (target-2.5%) */
	double overshoot_full_n;     /* deadbands of overshoot at which the pull-down reaches the floor */
	double undershoot_max_ratio; /* max multiplicative rate restore per tick on undershoot, e.g. 2.0 */
	double tick_sec;             /* seconds of accumulated stream time between decisions */
	double min_history_sec;      /* cumulative history required before the first decision */
} abr_gov_config;

/* Per-session governor state. Embed one in each encoder's struct. The totals are a stream-session
 * quantity: they survive libobs destroying/recreating the encoder across stop/start via
 * abr_gov_session_load()/abr_gov_session_save(). cur_rate is in the same units as target/ceiling. */
typedef struct abr_gov_state {
	double total_bits; /* cumulative coded bits (session) */
	double total_time; /* cumulative encoded seconds (session) */
	double acc_time;   /* stream seconds accumulated since the last tick (per-open) */
	int64_t cur_rate;  /* current governed rate (same units as target/ceiling) */
} abr_gov_state;

/* Sane defaults shared by x265, NVENC and AMF: +/-0.5% band, floor at target-2.5%, full overshoot
 * correction at 10*deadband, undershoot chase capped at 2x per tick, a 0.5 s decision cadence with a
 * 1.0 s history gate. */
abr_gov_config abr_gov_default_config(void);

/* One governor step: feed a coded frame. Accumulates the frame into the session totals and, when a
 * tick is due (tick_sec of stream time since the last decision) and enough cumulative history exists
 * (min_history_sec), applies the control law to st->cur_rate. Returns true if cur_rate changed (the
 * caller must then push it to the codec). If out_cum_avg is non-NULL it receives the cumulative
 * average in bits/sec at the moment a decision was made (for logging). */
bool abr_gov_tick(abr_gov_state *st, const abr_gov_config *cfg, int64_t target_rate, int64_t ceiling_rate,
                  double dt_sec, uint32_t frame_bytes, double *out_cum_avg);

/* Session persistence (keyed by encoder) so the cumulative budget survives libobs destroying and
 * recreating the encoder object across stop/start:
 *   - load: call once at session start. Restores total_bits/total_time and cur_rate (clamped to
 *     [target*floor_frac, ceiling]) if the previous stop was an ungraceful kill or a short disconnect;
 *     otherwise starts fresh (totals 0, cur_rate = target).
 *   - save: call at teardown. Persists the current state for the next load.
 * Both are no-ops when !active. */
void abr_gov_session_load(obs_encoder_t *encoder, bool active, int64_t target_rate, int64_t ceiling_rate,
                          const abr_gov_config *cfg, abr_gov_state *st);
void abr_gov_session_save(obs_encoder_t *encoder, bool active, const abr_gov_state *st);

/* Decide the new governed rate from the session-cumulative average (the quantity the streaming platform
 * measures over the whole stream) and the current rate. The law is a continuous controller that drives the
 * rate back toward the target in both directions, so it can always recover after an overshoot:
 *   overshoot  (cum_avg > target*(1+db)): pull the rate down toward target*overshoot_floor_frac on a
 *     self-scaling sqrt curve (full correction at overshoot_full_n deadbands); only ever lowers;
 *   undershoot (0 < cum_avg < target*(1-db)): restore headroom toward ceiling_rate, at most
 *     undershoot_max_ratio times the current rate per tick;
 *   in-band: snap to exactly target_rate.
 * Returns cur_rate unchanged when no correction is warranted. All rate arguments and the return value are
 * in the same units. */
int64_t abr_gov_decide(double cum_avg, int64_t cur_rate, int64_t target_rate, int64_t ceiling_rate,
                       const abr_gov_config *cfg);

#ifdef __cplusplus
}
#endif
