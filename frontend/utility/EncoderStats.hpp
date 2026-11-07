#pragma once

#include <obs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
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

/* Encoder slots of an output (libobs/obs-output.h: MAX_OUTPUT_VIDEO_ENCODERS = 10). That
 * header is not part of the public API, and obs_output_get_video_encoder2() bounds-checks
 * the index, so over-reading a slot is harmless. Scanning every slot instead of stopping at
 * the first empty one keeps the result correct if an output ever fills its encoder slots
 * non-contiguously. */
inline constexpr size_t maxVideoEncoderSlots = 16;

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
	EncoderFrameCounters counters;
	std::vector<const obs_encoder_t *> seenEncoders;
	std::vector<const video_t *> seenMixes;

	for (const obs_output_t *output : outputs) {
		if (!output)
			continue;

		for (size_t idx = 0; idx < maxVideoEncoderSlots; idx++) {
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

/** A video encoder of an output and the encoder slot it sits in. */
struct EncoderSlot {
	obs_encoder_t *encoder = nullptr;
	size_t slot = 0;
};

/**
 * The video encoders attached to an output, in slot order: a multitrack stream carries one
 * encoder per rendition, a plain recording output usually exactly one. Audio encoders and
 * empty slots are skipped.
 */
inline std::vector<EncoderSlot> outputVideoEncoders(const obs_output_t *output)
{
	/* Not named "slots": Qt #defines that word (see QT_NO_KEYWORDS). */
	std::vector<EncoderSlot> found;

	if (!output)
		return found;

	for (size_t idx = 0; idx < maxVideoEncoderSlots; idx++) {
		obs_encoder_t *encoder = obs_output_get_video_encoder2(output, idx);
		if (!encoder)
			continue;
		found.push_back(EncoderSlot{encoder, idx});
	}

	return found;
}

/** The delivery mix feeding an encoder, unwrapped (never the fps_override divisor shell). */
inline const video_t *deliveryMix(const obs_encoder_t *encoder)
{
	return encoder ? obs_encoder_parent_video(encoder) : nullptr;
}

/**
 * Counters of a single encoder: the delivery-mix pair (mix_frames/mix_lag_skipped, shared
 * with every other encoder fed from that mix) plus the pair libobs attributes to this
 * encoder alone (frames_offered/frames_decimated), its encode count and the payload bytes
 * of its packets (per-rendition bitrate).
 */
inline obs_encoder_frame_counters encoderCounters(const obs_encoder_t *encoder)
{
	obs_encoder_frame_counters counters = {};
	obs_encoder_get_frame_counters(encoder, &counters);
	return counters;
}

/* ------------------------------------------------------------------------- */
/* Display names - the object names themselves are left alone, so the log, the
 * bpm SEI and the encoder hotkey/scene references keep speaking the real name. */
/* ------------------------------------------------------------------------- */

/**
 * Table label of a rendition. The multitrack encoder names are boilerplate-heavy
 * ("multitrack video video encoder 0", "multitrack video live audio 1"; see
 * MultitrackVideoOutput.cpp), which costs a lot of width in a narrow dock.
 */
inline std::string renditionName(const obs_encoder_t *encoder)
{
	const std::string_view name = (encoder && obs_encoder_get_name(encoder)) ? obs_encoder_get_name(encoder) : "";

	struct PrefixRule {
		std::string_view prefix;
		const char *replacement;
	};
	static constexpr PrefixRule rules[] = {
		{"multitrack video video encoder ", "MT video enc "},
		{"multitrack video live audio ", "MT live audio "},
		{"multitrack video vod audio ", "MT vod audio "},
		{"multitrack video ", "MT "},
	};

	for (const PrefixRule &rule : rules) {
		if (name.size() <= rule.prefix.size() || name.compare(0, rule.prefix.size(), rule.prefix) != 0)
			continue;
		std::string out(rule.replacement);
		out.append(name.substr(rule.prefix.size()));
		return out;
	}

	return std::string(name);
}

/**
 * Implementation behind an encoder ("QSV", "NVENC", "x264"), derived from the encoder type id
 * (obs_encoder_get_id()), which is stable across locales - unlike the plugin display name
 * ("QuickSync H.264", "NVIDIA NVENC H.264 (new)") that obs_encoder_get_display_name() returns.
 */
inline std::string encoderImplementation(const obs_encoder_t *encoder)
{
	const char *id = encoder ? obs_encoder_get_id(encoder) : nullptr;
	if (!id)
		return {};

	struct IdRule {
		const char *needle;
		const char *label;
	};
	/* First substring match wins; the ids are the plugin ones (obs-nvenc, obs-qsv11,
	 * obs-ffmpeg/AMF, obs-x264/x265, SVT-AV1, VideoToolbox). */
	static constexpr IdRule rules[] = {
		{"nvenc", "NVENC"},   {"qsv", "QSV"},          {"amf", "AMF"},
		{"videotoolbox", "VideoToolbox"}, {"obs_x264", "x264"},    {"obs_x265", "x265"},
		{"svt_av1", "SVT-AV1"},   {"netint", "NetINT"},
	};
	for (const IdRule &rule : rules) {
		if (strstr(id, rule.needle))
			return rule.label;
	}

	if (const char *pretty = obs_encoder_get_display_name(id))
		return pretty;
	return id;
}

inline void asciiUpper(std::string &s)
{
	for (char &c : s) {
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
	}
}

inline void asciiLower(std::string &s)
{
	for (char &c : s) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
}

/**
 * "QSV H264", "NVENC HEVC", "x264" ... - implementation plus codec, with the codec dropped
 * when the implementation label already names it (x264/x265/SVT-AV1 are their own codec
 * families, so "x264 H264" would just waste the column).
 */
inline std::string encoderLabel(const obs_encoder_t *encoder)
{
	std::string impl = encoderImplementation(encoder);

	std::string codec = encoder && obs_encoder_get_codec(encoder) ? obs_encoder_get_codec(encoder) : "";
	asciiUpper(codec);

	struct CodecRule {
		const char *codec;
		const char *alsoNamedAs; /* substring that means the impl label says it already */
	};
	static constexpr CodecRule rules[] = {
		{"H264", "264"}, {"H265", "265"}, {"HEVC", "265"}, {"AV1", "AV1"}, {"VP9", "VP9"},
	};
	std::string implLower = impl;
	asciiLower(implLower);
	for (const CodecRule &rule : rules) {
		if (strcmp(codec.c_str(), rule.codec) != 0)
			continue;
		std::string needleLower = rule.alsoNamedAs;
		asciiLower(needleLower);
		if (implLower.find(needleLower) != std::string::npos)
			codec.clear();
		break;
	}

	if (codec.empty())
		return impl;
	if (impl.empty())
		return codec;
	return impl + " " + codec;
}

} /* namespace OBS */
