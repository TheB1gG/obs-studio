#pragma once

#include <obs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

/* Frame counters of the video path that feeds a video encoder.
 *
 * These are the numbers libobs labels "skipped frames due to encoding lag"
 * (video_output_get_total_frames()/video_output_get_skipped_frames()). They are
 * properties of a video_output, i.e. of a *delivery mix* - not of the encoder
 * and not of the base canvas. Because this fork renders the base canvas as a
 * high-fidelity master (RGBA16F), every video encoder is bound to its own
 * encoder-only mix (libobs/obs-encoder.c: acquire_encoder_only_mix()), so the
 * base canvas mix returned by obs_get_video() has no consumers at all and its
 * counters stay at zero. Anything that wants per-encoder frame statistics has to
 * read the mix the encoder is actually fed from, which
 * obs_encoder_parent_video() returns (the unwrapped mix, so the fps_override
 * frame-rate-divisor wrapper does not hide it).
 *
 * lagSkipped counts only frames a consumer was too slow to pick up
 * (libobs/media-io/video-io.c: cache[].skipped / skipped_frames). Deliberate
 * frame-rate-divisor (decimation) skips are per-input and are not included, so
 * this keeps its "due to encoding lag" meaning.
 *
 * The counters restart when a mix gains its first consumer
 * (video_output_connect2() -> reset_frames(), video-io.c) or when an encoder is
 * rebound to a different mix (live re-scale, encoder re-init). Callers must
 * therefore re-baseline whenever a value decreases.
 */
namespace OBS {

struct EncoderFrameCounters {
	/* frames pushed to the encoder's delivery mix */
	uint64_t delivered = 0;
	/* frames dropped on that path because an encoder did not keep up */
	uint64_t lagSkipped = 0;
};

/** Frame counters of a single video encoder's own delivery mix. */
inline EncoderFrameCounters encoderFrameCounters(const obs_encoder_t *encoder)
{
	EncoderFrameCounters counters;

	const video_t *mix = encoder ? obs_encoder_parent_video(encoder) : nullptr;
	if (!mix)
		return counters;

	counters.delivered = video_output_get_total_frames(mix);
	counters.lagSkipped = video_output_get_skipped_frames(mix);

	return counters;
}

/**
 * Frame counters summed over all video encoders of the given outputs.
 *
 * Streaming and recording can share one encoder, and renditions can share one
 * delivery mix (mix reuse), so encoders and mixes are each counted once to keep
 * the sum honest.
 */
inline EncoderFrameCounters outputFrameCounters(std::initializer_list<const obs_output_t *> outputs)
{
	/* Encoder slots of an output (libobs/obs-output.h: MAX_OUTPUT_VIDEO_ENCODERS = 10).
	 * The private header is not part of the public API, and obs_output_get_video_encoder2()
	 * bounds-checks the index, so over-reading a slot is harmless. Scanning every slot
	 * instead of stopping at the first empty one keeps the sum correct if an output ever
	 * fills its encoder slots non-contiguously. */
	static constexpr size_t encoderSlots = 16;

	EncoderFrameCounters counters;
	std::vector<const obs_encoder_t *> seenEncoders;
	std::vector<const video_t *> seenMixes;

	for (const obs_output_t *output : outputs) {
		if (!output)
			continue;

		for (size_t idx = 0; idx < encoderSlots; idx++) {
			obs_encoder_t *encoder = obs_output_get_video_encoder2(output, idx);
			if (!encoder)
				continue;

			if (std::find(seenEncoders.begin(), seenEncoders.end(), encoder) != seenEncoders.end())
				continue;
			seenEncoders.push_back(encoder);

			const video_t *mix = obs_encoder_parent_video(encoder);
			if (!mix)
				continue;
			if (std::find(seenMixes.begin(), seenMixes.end(), mix) != seenMixes.end())
				continue;
			seenMixes.push_back(mix);

			counters.delivered += video_output_get_total_frames(mix);
			counters.lagSkipped += video_output_get_skipped_frames(mix);
		}
	}

	return counters;
}

} /* namespace OBS */
