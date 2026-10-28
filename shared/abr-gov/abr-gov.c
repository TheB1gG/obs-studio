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

#include "abr-gov.h"
#include <math.h>
#include <string.h>
#include <util/threading.h>
#include <util/platform.h>

#define ABR_GOV_MAX_ENTRIES 16
#define ABR_GOV_DISCONNECT_KEEP_NS (INT64_C(60) * INT64_C(1000000000))

abr_gov_config abr_gov_default_config(void)
{
	abr_gov_config cfg = {
	    .deadband_frac = 0.005,        /* +/-0.5% band around target */
	    .overshoot_floor_frac = 0.975, /* floor at target - 2.5% */
	    .overshoot_full_n = 10.0,      /* full correction at 10*deadband overshoot */
	    .undershoot_max_ratio = 2.0,   /* restore at most 2x the current rate per tick */
	    .tick_sec = 0.5,               /* one decision every 0.5 s */
	    .min_history_sec = 1.0,        /* cumulative history required before deciding */
	};
	return cfg;
}

int64_t abr_gov_decide(double cum_avg, int64_t cur_rate, int64_t target_rate, int64_t ceiling_rate,
                       const abr_gov_config *cfg)
{
	if (target_rate <= 0 || cum_avg <= 0.0)
		return cur_rate;

	const double db = cfg->deadband_frac;
	const double t = (double)target_rate;
	const int64_t floor_rate = (int64_t)(t * cfg->overshoot_floor_frac);
	int64_t new_rate = cur_rate;

	if (cum_avg > t * (1.0 + db)) {
		/* OVERSHOOT: pull the rate down toward the floor along a self-scaling sqrt curve. The ramp spans
		 * N deadbands, so full correction is reached at an overshoot of N*deadband and the whole curve
		 * rescales with the deadband setting. Only lowers the rate (never raises on overshoot). */
		const double over_frac = cum_avg / t - 1.0; /* fractional overshoot above target (> db here) */
		const double full_frac = cfg->overshoot_full_n * db;
		double u = 1.0;
		if (full_frac > db)
			u = (over_frac - db) / (full_frac - db);
		if (u < 0.0)
			u = 0.0;
		else if (u > 1.0)
			u = 1.0;
		const double setpoint = t - sqrt(u) * (t - (double)floor_rate);
		int64_t sp = (int64_t)setpoint;
		if (sp < floor_rate)
			sp = floor_rate;
		if (sp < new_rate)
			new_rate = sp; /* only lower */
	} else if (cum_avg > 1.0 && cum_avg < t * (1.0 - db)) {
		/* UNDERSHOOT: restore headroom toward the ceiling, at most undershoot_max_ratio per tick. */
		double ratio = t / cum_avg;
		if (ratio > cfg->undershoot_max_ratio)
			ratio = cfg->undershoot_max_ratio;
		new_rate = (int64_t)((double)cur_rate * ratio);
		if (new_rate > ceiling_rate)
			new_rate = ceiling_rate;
	} else {
		/* IN-BAND: run at the target bitrate. */
		new_rate = target_rate;
	}

	if (new_rate <= 0 || new_rate == cur_rate)
		return cur_rate;
	return new_rate;
}

bool abr_gov_tick(abr_gov_state *st, const abr_gov_config *cfg, int64_t target_rate, int64_t ceiling_rate,
                  double dt_sec, uint32_t frame_bytes, double *out_cum_avg)
{
	if (target_rate <= 0 || dt_sec <= 0.0)
		return false;

	st->total_bits += (double)frame_bytes * 8.0;
	st->total_time += dt_sec;
	st->acc_time += dt_sec;

	if (st->acc_time < cfg->tick_sec)
		return false;
	st->acc_time = 0.0;

	const double cum_avg = st->total_time > 0.0 ? st->total_bits / st->total_time : 0.0;

	if (st->total_time < cfg->min_history_sec)
		return false;

	if (out_cum_avg)
		*out_cum_avg = cum_avg; /* bits/sec, at the moment a decision is made */

	const int64_t new_rate = abr_gov_decide(cum_avg, st->cur_rate, target_rate, ceiling_rate, cfg);
	if (new_rate <= 0 || new_rate == st->cur_rate)
		return false;

	st->cur_rate = new_rate;
	return true;
}

/* -------------------------------------------------------------------------- */
/* Session persistence                                                         */
/*                                                                             */
/* libobs destroys the encoder object on every stop (obs_encoder_stop ->       */
/* info.destroy) and recreates it on the next start, so a stream-session        */
/* budget lives in a per-module registry keyed by the stable obs_encoder_t.     */
/* Whether the previous session's budget is kept or reset is decided at load    */
/* time from how the previous stop ended (the stop reason libobs stamps onto   */
/* the encoder):                                                                */
/*   graceful stop / encode error / first start -> fresh                        */
/*   ungraceful kill                            -> keep                         */
/*   disconnect within 60 s                     -> keep                         */
/*   disconnect longer than 60 s                -> fresh                        */
/* An OBJECT library's statics are per-DLL, so each encoder plugin gets its own */
/* registry - exactly what each encoder type needs (its sessions never cross   */
/* types).                                                                      */
/* -------------------------------------------------------------------------- */

struct abr_gov_budget {
	double total_bits;     /* cumulative coded bits for this stream session */
	double total_time;    /* cumulative seconds of encoded time */
	int64_t cur_rate;     /* last applied governed rate (same units as target/ceiling) */
	char encoder_name[64]; /* identity guard against stale pointer reuse */
};

struct abr_gov_entry {
	obs_encoder_t *encoder;
	struct abr_gov_budget budget;
};

static struct abr_gov_entry abr_gov_registry[ABR_GOV_MAX_ENTRIES];
static pthread_mutex_t abr_gov_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Find the registry entry for an encoder, matched by pointer and name. Caller holds abr_gov_mutex. */
static struct abr_gov_entry *abr_gov_find_locked(const char *name, obs_encoder_t *encoder)
{
	for (size_t i = 0; i < ABR_GOV_MAX_ENTRIES; i++) {
		struct abr_gov_entry *e = &abr_gov_registry[i];
		if (e->encoder == encoder &&
		    strncmp(e->budget.encoder_name, name, sizeof(e->budget.encoder_name) - 1) == 0)
			return e;
	}
	return NULL;
}

/* Find the entry for an encoder, creating one in a free slot (or evicting the least-tracked entry) when none
 * exists. Caller holds abr_gov_mutex. A pointer reuse with a different name is treated as a stale entry and
 * evicted like any other. */
static struct abr_gov_entry *abr_gov_get_locked(const char *name, obs_encoder_t *encoder)
{
	struct abr_gov_entry *e = abr_gov_find_locked(name, encoder);
	if (e)
		return e;

	/* No match: take a free slot, else evict the least-tracked entry. */
	struct abr_gov_entry *victim = NULL;
	for (size_t i = 0; i < ABR_GOV_MAX_ENTRIES; i++) {
		struct abr_gov_entry *cand = &abr_gov_registry[i];
		if (!cand->encoder) {
			victim = cand;
			break;
		}
		if (!victim || cand->budget.total_time < victim->budget.total_time)
			victim = cand;
	}
	e = victim ? victim : &abr_gov_registry[0];
	e->encoder = encoder;
	memset(&e->budget, 0, sizeof(e->budget));
	if (name) {
		strncpy(e->budget.encoder_name, name, sizeof(e->budget.encoder_name) - 1);
		e->budget.encoder_name[sizeof(e->budget.encoder_name) - 1] = '\0';
	}
	return e;
}

void abr_gov_session_load(obs_encoder_t *encoder, bool active, int64_t target_rate, int64_t ceiling_rate,
                          const abr_gov_config *cfg, abr_gov_state *st)
{
	st->total_bits = 0.0;
	st->total_time = 0.0;
	st->acc_time = 0.0;
	st->cur_rate = target_rate; /* fresh: start at target (headroom is earned, not assumed) */

	if (!active || !encoder || target_rate <= 0)
		return;

	/* Snapshot the stored budget under the lock so we never dereference registry memory after unlocking. */
	struct abr_gov_budget b = {0};
	bool have = false;
	const char *name = obs_encoder_get_name(encoder);
	pthread_mutex_lock(&abr_gov_mutex);
	struct abr_gov_entry *e = abr_gov_find_locked(name, encoder);
	if (e) {
		b = e->budget;
		have = true;
	}
	pthread_mutex_unlock(&abr_gov_mutex);

	int code = obs_encoder_get_last_stop_code(encoder);
	int64_t stop_ns = obs_encoder_get_last_stop_time(encoder);
	bool keep = false;
	switch (code) {
	case OBS_OUTPUT_KILLED:
		keep = true; /* one continuous session across the kill */
		break;
	case OBS_OUTPUT_DISCONNECTED:
		/* the core auto-reconnects with backoff; treat short gaps as the same session */
		keep = stop_ns != 0 && os_gettime_ns() - stop_ns <= ABR_GOV_DISCONNECT_KEEP_NS;
		break;
	default: /* first start, graceful stop, encode error, or unknown */
		keep = false;
		break;
	}

	if (keep && have && b.total_time > 0.0) {
		st->total_bits = b.total_bits;
		st->total_time = b.total_time;
		int64_t rate = b.cur_rate;
		const int64_t floor_rate = (int64_t)((double)target_rate * cfg->overshoot_floor_frac);
		if (rate < floor_rate)
			rate = floor_rate;
		if (rate > ceiling_rate)
			rate = ceiling_rate;
		st->cur_rate = rate;
	}
}

void abr_gov_session_save(obs_encoder_t *encoder, bool active, const abr_gov_state *st)
{
	if (!active || !encoder)
		return;
	const char *name = obs_encoder_get_name(encoder);
	pthread_mutex_lock(&abr_gov_mutex);
	struct abr_gov_entry *e = abr_gov_get_locked(name, encoder);
	e->budget.total_bits = st->total_bits;
	e->budget.total_time = st->total_time;
	e->budget.cur_rate = st->cur_rate;
	pthread_mutex_unlock(&abr_gov_mutex);
}
