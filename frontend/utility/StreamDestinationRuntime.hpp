#pragma once

#include <obs.hpp>

#include <string>

/* Runtime state of one streaming destination.
 *
 * StreamingDestinations (slice 1) owns what is *stored*: the files and ini keys of a profile. This type
 * owns what is *running* for one of those entries: its service object, its output, the encoder group it
 * is fed by and the state machine the UI renders.
 *
 * Slice 2a keeps it as a mirror of the existing single-stream path, so nothing about today's behaviour
 * can change; slice 2b moves the real start/stop calls over to it and adds the per-destination delay and
 * reconnect state (Q14/Q15). A profile that has no destinations yet (the flag is off) is represented by
 * one unnamed runtime, which is what keeps the "n of N up" counters meaningful during the transition.
 */
class StreamDestinationRuntime {
public:
	/* Mirrors the points in the existing single-stream path where the UI learns that something changed;
	 * the names match the OBSBasic signal handlers so the two can be read side by side. */
	enum class State {
		Stopped,
		/* encoder/session setup, before the output is started */
		Preparing,
		/* output start called, not connected yet */
		Connecting,
		DelayStarting,
		Live,
		Stopping,
		DelayStopping,
		/* the output itself is retrying; today the retry lives inside the output */
		Reconnecting,
	};

	std::string id;      /* the immutable profile id; empty for the legacy single stream */
	std::string name;    /* display name, only for the UI and the logs */
	bool enabled = true; /* a disabled destination is never started */

	State state = State::Stopped;
	int reconnectAttempt = 0;

	/* Set when the skull button was pressed for this destination and cleared when its stop signal arrives.
	 * Per destination on purpose: the fork's kill path rewrites the stop code to success so that no failure
	 * dialog appears, and a kill of one destination must not silence the failure report of another. */
	bool killRequested = false;

	/* Filled in when the destination is actually started (slice 2b). */
	OBSService service;
	OBSOutput output;

	/* The encoder group this destination is fed by, for the Q25 sharing rules and the Q37/Q40 consumer
	 * counting. Empty while the destination is not running. */
	std::string encoderGroupId;

	bool isLive() const { return state == State::Live || state == State::DelayStarting; }
	bool isRunning() const { return state != State::Stopped; }

	const char *stateName() const;
};
