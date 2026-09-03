#include "MultitrackVideoError.hpp"
#include "MultitrackVideoOutput.hpp"
#include "models/multitrack-video.hpp"
#include "GoLiveAPI_Network.hpp"
#include "GoLiveAPI_PostData.hpp"

#include <OBSApp.hpp>

#include <bpm.h>
#include <util/dstr.hpp>
#include <libavformat/avformat.h>

#include <QPushButton>
#include <QMessageBox>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <cinttypes>
#include <stdio.h>

// Codec profile strings
static const char *h264_main = "Main";
static const char *h264_high = "High";
static const char *h264_cb = "Constrained Baseline";
static const char *hevc_main = "Main";
static const char *hevc_main10 = "Main 10";
static const char *av1_main = "Main";

// Maximum reconnect attempts with an invalid key error before giving up (roughly 30 seconds with default start value)
static constexpr uint8_t MAX_RECONNECT_ATTEMPTS = 5;

extern bool EncoderAvailable(const char *encoder);

// Resolves a codec name from the Go-Live JSON config to an OBS audio encoder ID.
// Falls back to the provided default (typically CoreAudio_AAC or ffmpeg_aac) if the
// requested codec's encoder is not available.
static const char *resolve_audio_encoder_id(const char *codec, const char *default_id)
{
	if (!codec || codec[0] == '\0')
		return default_id;

	if (strcmp(codec, "flac") == 0) {
		if (EncoderAvailable("ffmpeg_flac"))
			return "ffmpeg_flac";
		blog(LOG_WARNING, "MultitrackVideoOutput: FLAC encoder not available, falling back to %s", default_id);
		return default_id;
	}

	if (strcmp(codec, "opus") == 0) {
		if (EncoderAvailable("ffmpeg_opus"))
			return "ffmpeg_opus";
		blog(LOG_WARNING, "MultitrackVideoOutput: Opus encoder not available, falling back to %s", default_id);
		return default_id;
	}

	// "aac" or any unrecognized codec: use the platform default
	return default_id;
}

Qt::ConnectionType BlockingConnectionTypeFor(QObject *object)
{
	return object->thread() == QThread::currentThread() ? Qt::DirectConnection : Qt::BlockingQueuedConnection;
}

bool MultitrackVideoDeveloperModeEnabled()
{
	// Developer mode is always enabled. The --enable-multitrack-video-dev command-line argument
	// is no longer required (and is ignored). The function is kept so existing call sites in the
	// settings UI continue to work unchanged.
	return true;
}

static OBSServiceAutoRelease create_service(const GoLiveApi::Config &go_live_config,
					    const std::optional<std::string> &rtmp_url, const QString &in_stream_key,
					    std::optional<bool> use_rtmps)
{
	const char *url = nullptr;
	QString stream_key = in_stream_key;

	const auto &ingest_endpoints = go_live_config.ingest_endpoints;

	for (auto &endpoint : ingest_endpoints) {
		if (qstrnicmp("RTMP", endpoint.protocol.c_str(), 4))
			continue;

		if (use_rtmps.has_value() && *use_rtmps != (qstricmp("RTMPS", endpoint.protocol.c_str()) == 0))
			continue;

		url = endpoint.url_template.c_str();
		if (endpoint.authentication && !endpoint.authentication->empty()) {
			blog(LOG_INFO, "Using stream key supplied by autoconfig");
			stream_key = QString::fromStdString(*endpoint.authentication);
		}
		break;
	}

	if (rtmp_url.has_value()) {
		// Despite being set by user, it was set to a ""
		if (rtmp_url->empty()) {
			throw MultitrackVideoError::warning(QTStr("FailedToStartStream.NoCustomRTMPURLInSettings"));
		}

		url = rtmp_url->c_str();
		blog(LOG_INFO, "Using custom RTMP URL: '%s'", url);
	} else {
		if (!url) {
			blog(LOG_ERROR, "No RTMP URL in go live config");
			throw MultitrackVideoError::warning(QTStr("FailedToStartStream.NoRTMPURLInConfig"));
		}

		blog(LOG_INFO, "Using URL template: '%s'", url);
	}

	DStr str;
	dstr_cat(str, url);

	// dstr_find does not protect against null, and dstr_cat will
	// not initialize str if cat'ing with a null url
	if (!dstr_is_empty(str)) {
		auto found = dstr_find(str, "/{stream_key}");
		if (found)
			dstr_remove(str, found - str->array, str->len - (found - str->array));
	}

	/* The stream key itself may contain query parameters, such as
	 * "bandwidthtest" that need to be carried over. */
	QUrl parsed_user_key{in_stream_key};
	QUrlQuery user_key_query{parsed_user_key};

	QUrl parsed_key{stream_key};

	QUrl parsed_url{url};
	QUrlQuery parsed_query{parsed_url};

	for (const auto &[key, value] : user_key_query.queryItems())
		parsed_query.addQueryItem(key, value);

	if (!go_live_config.meta.config_id.empty()) {
		parsed_query.addQueryItem("clientConfigId", QString::fromStdString(go_live_config.meta.config_id));
	}

	parsed_key.setQuery(parsed_query);

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "server", str->array);
	obs_data_set_string(settings, "key", parsed_key.toString().toUtf8().constData());

	auto service = obs_service_create("rtmp_custom", "multitrack video service", settings, nullptr);

	if (!service) {
		blog(LOG_WARNING, "Failed to create multitrack video service");
		throw MultitrackVideoError::warning(QTStr("FailedToStartStream.FailedToCreateMultitrackVideoService"));
	}

	return service;
}

static OBSOutputAutoRelease create_output()
{
	OBSOutputAutoRelease output = obs_output_create("rtmp_output", "rtmp multitrack video", nullptr, nullptr);

	if (!output) {
		blog(LOG_ERROR, "Failed to create multitrack video rtmp output");
		throw MultitrackVideoError::warning(QTStr("FailedToStartStream.FailedToCreateMultitrackVideoOutput"));
	}

	return output;
}

static OBSOutputAutoRelease create_recording_output(obs_data_t *settings)
{
	OBSOutputAutoRelease output;
	bool useMP4 = obs_data_get_bool(settings, "use_mp4");

	if (useMP4) {
		output = obs_output_create("mp4_output", "mp4 multitrack video", settings, nullptr);
	} else {
		output = obs_output_create("flv_output", "flv multitrack video", settings, nullptr);
	}

	if (!output) {
		blog(LOG_ERROR, "Failed to create multitrack video %s output", useMP4 ? "mp4" : "flv");
	}

	return output;
}

static void adjust_video_encoder_scaling(const obs_video_info &ovi, obs_encoder_t *video_encoder,
					 const GoLiveApi::VideoEncoderConfiguration &encoder_config,
					 size_t encoder_index)
{
	auto requested_width = encoder_config.width;
	auto requested_height = encoder_config.height;

	if (ovi.base_width < requested_width || ovi.base_height < requested_height) {
		blog(LOG_WARNING,
		     "Requested resolution exceeds canvas/available resolution for encoder %zu: %" PRIu32 "x%" PRIu32
		     " > %" PRIu32 "x%" PRIu32,
		     encoder_index, requested_width, requested_height, ovi.base_width, ovi.base_height);
	}

	obs_encoder_set_scaled_size(video_encoder, requested_width, requested_height);
	obs_encoder_set_gpu_scale_type(video_encoder, encoder_config.gpu_scale_type.value_or(OBS_SCALE_BICUBIC));
	obs_encoder_set_preferred_video_format(video_encoder, encoder_config.format.value_or(VIDEO_FORMAT_NV12));
	obs_encoder_set_preferred_color_space(video_encoder, encoder_config.colorspace.value_or(VIDEO_CS_709));
	obs_encoder_set_preferred_range(video_encoder, encoder_config.range.value_or(VIDEO_RANGE_PARTIAL));
}

static uint32_t closest_divisor(const obs_video_info &ovi, const media_frames_per_second &target_fps)
{
	auto target = (uint64_t)target_fps.numerator * ovi.fps_den;
	auto source = (uint64_t)ovi.fps_num * target_fps.denominator;
	return std::max(1u, static_cast<uint32_t>(source / target));
}

static void adjust_encoder_frame_rate_divisor(const obs_video_info &ovi, obs_encoder_t *video_encoder,
					      const GoLiveApi::VideoEncoderConfiguration &encoder_config,
					      const size_t encoder_index)
{
	if (!encoder_config.framerate) {
		blog(LOG_WARNING, "`framerate` not specified for encoder %zu", encoder_index);
		return;
	}
	media_frames_per_second requested_fps = *encoder_config.framerate;

	if (ovi.fps_num == requested_fps.numerator && ovi.fps_den == requested_fps.denominator)
		return;

	auto divisor = closest_divisor(ovi, requested_fps);
	if (divisor <= 1)
		return;

	blog(LOG_INFO, "Setting frame rate divisor to %u for encoder %zu", divisor, encoder_index);
	obs_encoder_set_frame_rate_divisor(video_encoder, divisor);
}

static bool encoder_available(const char *type)
{
	const char *id = nullptr;

	for (size_t idx = 0; obs_enum_encoder_types(idx, &id); idx++) {
		if (strcmp(id, type) == 0)
			return true;
	}

	return false;
}

static OBSEncoderAutoRelease create_video_encoder(DStr &name_buffer, size_t encoder_index,
						  const GoLiveApi::VideoEncoderConfiguration &encoder_config,
						  const OBSCanvasAutoRelease &canvas)
{
	auto encoder_type = encoder_config.type.c_str();
	if (!encoder_available(encoder_type)) {
		blog(LOG_ERROR, "Encoder type '%s' not available", encoder_type);
		throw MultitrackVideoError::warning(QTStr("FailedToStartStream.EncoderNotAvailable").arg(encoder_type));
	}

	dstr_printf(name_buffer, "multitrack video video encoder %zu", encoder_index);

	OBSDataAutoRelease encoder_settings = obs_data_create_from_json(encoder_config.settings.dump().c_str());

	/* VAAPI-based encoders unfortunately use an integer for "profile". Until a string-based "profile" can be used with
	 * VAAPI, find the corresponding integer value and update the settings with an integer-based "profile".
	 */
	if (strstr(encoder_type, "vaapi")) {
		// Move the "profile" string to "profile_str".
		const char *profile_str = obs_data_get_string(encoder_settings, "profile");
		obs_data_set_string(encoder_settings, "profile_str", profile_str);
		obs_data_item_t *profile_item = obs_data_item_byname(encoder_settings, "profile");
		obs_data_item_remove(&profile_item);
		obs_data_item_release(&profile_item);

		// Find the vaapi_profile integer based on codec type and "profile" string.
		int vaapi_profile;
		const char *codec = obs_get_encoder_codec(encoder_type);
		if (strcmp(codec, "h264") == 0) {
			if (astrcmpi(profile_str, h264_main) == 0) {
				vaapi_profile = AV_PROFILE_H264_MAIN;
			} else if (astrcmpi(profile_str, h264_high) == 0) {
				vaapi_profile = AV_PROFILE_H264_HIGH;
			} else if (astrcmpi(profile_str, h264_cb) == 0) {
				vaapi_profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
			} else {
				blog(LOG_WARNING, "Unsupported H264 profile '%s', setting to Main profile",
				     profile_str);
				vaapi_profile = AV_PROFILE_H264_MAIN;
			}
		} else if (strcmp(codec, "hevc") == 0) {
			if (astrcmpi(profile_str, hevc_main) == 0) {
				vaapi_profile = AV_PROFILE_HEVC_MAIN;
			} else if (astrcmpi(profile_str, hevc_main10) == 0) {
				vaapi_profile = AV_PROFILE_HEVC_MAIN_10;
			} else {
				blog(LOG_WARNING, "Unsupported HEVC profile '%s', setting to Main profile",
				     profile_str);
				vaapi_profile = AV_PROFILE_HEVC_MAIN;
			}
		} else if (strcmp(codec, "av1") == 0) {
			if (astrcmpi(profile_str, av1_main) == 0) {
				vaapi_profile = AV_PROFILE_AV1_MAIN;
			} else {
				blog(LOG_WARNING, "Unsupported AV1 profile '%s', setting to Main profile", profile_str);
				vaapi_profile = AV_PROFILE_AV1_MAIN;
			}
		} else {
			vaapi_profile = AV_PROFILE_UNKNOWN;
			blog(LOG_WARNING, "Unsupported codec '%s', setting profile to unknown", codec);
		}
		obs_data_set_int(encoder_settings, "profile", vaapi_profile);
	}
	obs_data_set_bool(encoder_settings, "disable_scenecut", true);

	OBSEncoderAutoRelease video_encoder =
		obs_video_encoder_create(encoder_type, name_buffer, encoder_settings, nullptr);
	if (!video_encoder) {
		blog(LOG_ERROR, "Failed to create video encoder '%s'", name_buffer->array);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FailedToCreateVideoEncoder").arg(name_buffer->array, encoder_type));
	}
	obs_encoder_set_video(video_encoder, obs_canvas_get_video(canvas));

	obs_video_info ovi;
	if (!obs_canvas_get_video_info(canvas, &ovi)) {
		blog(LOG_WARNING, "Failed to get obs_video_info while creating encoder %zu", encoder_index);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FailedToGetOBSVideoInfo").arg(name_buffer->array, encoder_type));
	}

	adjust_video_encoder_scaling(ovi, video_encoder, encoder_config, encoder_index);
	adjust_encoder_frame_rate_divisor(ovi, video_encoder, encoder_config, encoder_index);

	return video_encoder;
}

static OBSEncoderAutoRelease create_audio_encoder(const char *name, const char *audio_encoder_id, obs_data_t *settings,
						  size_t mixer_idx)
{
	OBSEncoderAutoRelease audio_encoder =
		obs_audio_encoder_create(audio_encoder_id, name, settings, mixer_idx, nullptr);
	if (!audio_encoder) {
		blog(LOG_ERROR, "Failed to create audio encoder");
		throw MultitrackVideoError::warning(QTStr("FailedToStartStream.FailedToCreateAudioEncoder"));
	}
	obs_encoder_set_audio(audio_encoder, obs_get_audio());
	return audio_encoder;
}

struct OBSOutputs {
	OBSOutputAutoRelease output, recording_output;
};

static OBSOutputs SetupOBSOutput(QWidget *parent, const QString &multitrack_video_name,
				 obs_data_t *dump_stream_to_file_config, const GoLiveApi::Config &go_live_config,
				 std::vector<OBSEncoderAutoRelease> &audio_encoders,
				 std::shared_ptr<obs_encoder_group_t> &video_encoder_group,
				 const char *audio_encoder_id, size_t main_audio_mixer,
				 std::optional<size_t> vod_track_mixer,
				 const std::vector<OBSCanvasAutoRelease> &canvases);
static void SetupSignalHandlers(bool recording, MultitrackVideoOutput *self, obs_output_t *output, OBSSignal &start,
				OBSSignal &stop);

void MultitrackVideoOutput::PrepareStreaming(
	QWidget *parent, const char *service_name, obs_service_t *service, const std::optional<std::string> &rtmp_url,
	const QString &stream_key, const char *audio_encoder_id, std::optional<uint32_t> maximum_aggregate_bitrate,
	std::optional<uint32_t> maximum_video_tracks, bool request_max_tracks, std::optional<std::string> custom_config,
	obs_data_t *dump_stream_to_file_config, size_t main_audio_mixer, std::optional<size_t> vod_track_mixer,
	std::optional<bool> use_rtmps, std::optional<QString> extra_canvas)
{
	{
		const std::lock_guard<std::mutex> current_lock{current_mutex};
		const std::lock_guard<std::mutex> current_stream_dump_lock{current_stream_dump_mutex};
		if (current || current_stream_dump) {
			blog(LOG_WARNING, "Tried to prepare multitrack video output while it's already active");
			return;
		}
	}

	restart_on_error = false;

	std::optional<GoLiveApi::Config> go_live_config;
	std::optional<GoLiveApi::Config> custom;
	bool is_custom_config = custom_config.has_value();
	auto auto_config_url = MultitrackVideoAutoConfigURL(service);

	OBSDataAutoRelease service_settings = obs_service_get_settings(service);
	auto multitrack_video_name = QTStr("Basic.Settings.Stream.MultitrackVideoLabel");
	if (obs_data_has_user_value(service_settings, "multitrack_video_name")) {
		multitrack_video_name = obs_data_get_string(service_settings, "multitrack_video_name");
	}

	auto auto_config_url_data = auto_config_url.toUtf8();

	std::vector<OBSCanvasAutoRelease> canvases;

	canvases.emplace_back(obs_get_main_canvas());
	if (extra_canvas) {
		obs_canvas_t *canvas = obs_get_canvas_by_uuid(extra_canvas->toUtf8().constData());
		if (!canvas) {
			throw MultitrackVideoError::critical(QTStr("FailedToStartStream.MissingCanvas"));
		}
		canvases.emplace_back(canvas);
	}

	std::string canvasNames;
	for (const auto &canvas : canvases) {
		if (!canvasNames.empty())
			canvasNames += ", ";

		canvasNames += obs_canvas_get_name(canvas);
	}

	DStr vod_track_info_storage;
	if (vod_track_mixer.has_value())
		dstr_printf(vod_track_info_storage, "Yes (mixer: %zu)", vod_track_mixer.value());

	blog(LOG_INFO,
	     "Preparing enhanced broadcasting stream for:\n"
	     "    custom config:  %s\n"
	     "    config url:     %s\n"
	     "  settings:\n"
	     "    service:               %s\n"
	     "    request max tracks:    %s\n"
	"    max aggregate bitrate: %s (\"%" PRIu32 "\")\n"
	"    max video tracks:      %s (\"%" PRIu32 "\")\n"
	     "    custom rtmp url:       %s ('%s')\n"
	     "    vod track:             %s\n"
	     "    canvases:              %s",
	     is_custom_config ? "Yes" : "No", !auto_config_url.isEmpty() ? auto_config_url_data.constData() : "(null)",
	     service_name, request_max_tracks ? "Yes" : "No",
	     maximum_aggregate_bitrate.has_value() ? "Set" : "Auto",
	     maximum_aggregate_bitrate.value_or(0), maximum_video_tracks.has_value() ? "Set" : "Auto",
	     maximum_video_tracks.value_or(0), rtmp_url.has_value() ? "Yes" : "No",
	     rtmp_url.has_value() ? rtmp_url->c_str() : "",
	     vod_track_info_storage->array ? vod_track_info_storage->array : "No", canvasNames.c_str());

	const bool custom_config_only = auto_config_url.isEmpty() && MultitrackVideoDeveloperModeEnabled() &&
					custom_config.has_value() &&
					strcmp(obs_service_get_id(service), "rtmp_custom") == 0;

	if (!custom_config_only) {
		auto go_live_post =
			constructGoLivePost(stream_key, maximum_aggregate_bitrate, maximum_video_tracks, request_max_tracks,
					     vod_track_mixer.has_value(), canvases);

		go_live_config = DownloadGoLiveConfig(parent, auto_config_url, go_live_post, multitrack_video_name);
	}

	if (custom_config.has_value()) {
		GoLiveApi::Config parsed_custom;
		try {
			parsed_custom = nlohmann::json::parse(*custom_config);
		} catch (const nlohmann::json::exception &exception) {
			blog(LOG_WARNING, "Failed to parse custom config: %s", exception.what());
			throw MultitrackVideoError::critical(QTStr("FailedToStartStream.InvalidCustomConfig"));
		}

		// copy unique ID from go live request
		if (go_live_config.has_value()) {
			parsed_custom.meta.config_id = go_live_config->meta.config_id;
			blog(LOG_INFO, "Using config_id from go live config with custom config: %s",
			     parsed_custom.meta.config_id.c_str());
		}

		nlohmann::json custom_data = parsed_custom;
		blog(LOG_INFO, "Using custom go live config: %s", custom_data.dump(4).c_str());

		custom.emplace(std::move(parsed_custom));
	}

	if (go_live_config.has_value()) {
		blog(LOG_INFO, "Enhanced broadcasting config_id: '%s'", go_live_config->meta.config_id.c_str());
	}

	if (!go_live_config && !custom) {
		blog(LOG_ERROR, "MultitrackVideoOutput: no config set, this should never happen");
		throw MultitrackVideoError::warning(QTStr("FailedToStartStream.NoConfig"));
	}

	const auto &output_config = custom ? *custom : *go_live_config;
	const auto &service_config = go_live_config ? *go_live_config : *custom;

	// Stored with the output objects so live applies can diff against what is actually running.
	std::string active_override_json = nlohmann::json(output_config).dump();

	std::vector<OBSEncoderAutoRelease> audio_encoders;
	std::shared_ptr<obs_encoder_group_t> video_encoder_group;
	auto outputs = SetupOBSOutput(parent, multitrack_video_name, dump_stream_to_file_config, output_config,
				      audio_encoders, video_encoder_group, audio_encoder_id, main_audio_mixer,
				      vod_track_mixer, canvases);
	auto output = std::move(outputs.output);
	auto recording_output = std::move(outputs.recording_output);
	if (!output)
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FallbackToDefault").arg(multitrack_video_name));

	auto multitrack_video_service = create_service(service_config, rtmp_url, stream_key, use_rtmps);
	if (!multitrack_video_service)
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FallbackToDefault").arg(multitrack_video_name));

	obs_output_set_service(output, multitrack_video_service);

	// Register the BPM (Broadcast Performance Metrics) callback
	obs_output_add_packet_callback(output, bpm_inject, NULL);

	// Set callback to prevent reconnection attempts once the stream key has become invalid
	static auto reconnect_cb = [](void *param, obs_output_t *, int code) -> bool {
		auto _this = static_cast<MultitrackVideoOutput *>(param);
		return code != OBS_OUTPUT_INVALID_STREAM || (_this->reconnect_attempts++ < MAX_RECONNECT_ATTEMPTS);
	};
	obs_output_set_reconnect_callback(output, reconnect_cb, this);

	OBSSignal start_streaming;
	OBSSignal stop_streaming;
	SetupSignalHandlers(false, this, output, start_streaming, stop_streaming);

	if (dump_stream_to_file_config && recording_output) {
		OBSSignal start_recording;
		OBSSignal stop_recording;
		SetupSignalHandlers(true, this, recording_output, start_recording, stop_recording);

		decltype(audio_encoders) recording_audio_encoders;
		recording_audio_encoders.reserve(audio_encoders.size());
		for (auto &encoder : audio_encoders) {
			recording_audio_encoders.emplace_back(obs_encoder_get_ref(encoder));
		}

		std::vector<OBSCanvasAutoRelease> recording_canvases(canvases.size());
		for (const auto &canvas : canvases) {
			recording_canvases.emplace_back(obs_canvas_get_ref(canvas));
		}

		{
			const std::lock_guard current_stream_dump_lock{current_stream_dump_mutex};
			current_stream_dump.emplace(OBSOutputObjects{
				std::move(recording_output),
				video_encoder_group,
				std::move(recording_audio_encoders),
				nullptr,
				std::move(start_recording),
				std::move(stop_recording),
				std::move(recording_canvases),
				active_override_json,
			});
		}
	}

	const std::lock_guard current_lock{current_mutex};
	current.emplace(OBSOutputObjects{
		std::move(output),
		video_encoder_group,
		std::move(audio_encoders),
		std::move(multitrack_video_service),
		std::move(start_streaming),
		std::move(stop_streaming),
		std::move(canvases),
		active_override_json,
	});
}

namespace {

// Collects all audio encoder configurations in output order (live tracks first, then VOD), mirroring the way
// encoders are attached to the multitrack video output at start time.
std::vector<GoLiveApi::AudioEncoderConfiguration> collect_audio_configurations(const GoLiveApi::Config &config)
{
	std::vector<GoLiveApi::AudioEncoderConfiguration> configs;
	configs.insert(configs.end(), config.audio_configurations.live.begin(), config.audio_configurations.live.end());
	if (config.audio_configurations.vod.has_value()) {
		configs.insert(configs.end(), config.audio_configurations.vod->begin(),
			      config.audio_configurations.vod->end());
	}
	return configs;
}

// If the audio encoder is CoreAudio AAC, override settings to use TVBR at maximum quality (127).
// Any bitrate value from the JSON config is ignored in that case. Non-AAC encoders are left unchanged.
static nlohmann::json apply_audio_vbr_override(const GoLiveApi::AudioEncoderConfiguration &config,
                                               const char *encoder_id)
{
	auto settings = config.settings;

	if (strcmp(encoder_id, "CoreAudio_AAC") != 0)
		return settings;

	settings["vbr"] = true;
	settings["quality_target"] = 127;
	return settings;
}

// Logs which fields of an encoder's settings blob changed between the stored and new configuration.
void log_changed_settings_fields(const char *kind, size_t index, const nlohmann::json &old_settings,
				 const nlohmann::json &new_settings)
{
	std::string changes;
	for (const auto &item : new_settings.items()) {
		const auto &key = item.key();
		if (!old_settings.contains(key))
			continue;

		if (item.value() == old_settings[key])
			continue;

		if (!changes.empty())
			changes += ", ";

		changes += key + " (" + old_settings[key].dump() + " -> " + item.value().dump() + ")";
	}

	if (changes.empty()) {
		blog(LOG_INFO, "MultitrackVideoOutput: %s encoder %zu refreshed live settings", kind, index);
		return;
	}

	std::string line = " changed fields: " + changes;
	blog(LOG_INFO, "MultitrackVideoOutput: %s encoder %zu updated live:%s", kind, index, line.c_str());
}

} // namespace

static const char *scale_type_to_string(enum obs_scale_type type)
{
	switch (type) {
	case OBS_SCALE_DISABLE:
		return "OBS_SCALE_DISABLE";
	case OBS_SCALE_POINT:
		return "OBS_SCALE_POINT";
	case OBS_SCALE_BICUBIC:
		return "OBS_SCALE_BICUBIC";
	case OBS_SCALE_BILINEAR:
		return "OBS_SCALE_BILINEAR";
	case OBS_SCALE_LANCZOS:
		return "OBS_SCALE_LANCZOS";
	case OBS_SCALE_AREA:
		return "OBS_SCALE_AREA";
	case OBS_SCALE_BLERP:
		return "OBS_SCALE_BLERP";
	case OBS_SCALE_BILINEAR_LOWRES:
		return "OBS_SCALE_BILINEAR_LOWRES";
	case OBS_SCALE_INTEGER_AREA:
		return "OBS_SCALE_INTEGER_AREA";
	}

	return "unknown";
}

static const char *colorspace_to_string(enum video_colorspace space)
{
	switch (space) {
	case VIDEO_CS_DEFAULT:
		return "VIDEO_CS_DEFAULT";
	case VIDEO_CS_601:
		return "VIDEO_CS_601";
	case VIDEO_CS_709:
		return "VIDEO_CS_709";
	case VIDEO_CS_SRGB:
		return "VIDEO_CS_SRGB";
	case VIDEO_CS_2100_PQ:
		return "VIDEO_CS_2100_PQ";
	case VIDEO_CS_2100_HLG:
		return "VIDEO_CS_2100_HLG";
	}

	return "unknown";
}

static const char *range_to_string(enum video_range_type range)
{
	switch (range) {
	case VIDEO_RANGE_DEFAULT:
		return "VIDEO_RANGE_DEFAULT";
	case VIDEO_RANGE_PARTIAL:
		return "VIDEO_RANGE_PARTIAL";
	case VIDEO_RANGE_FULL:
		return "VIDEO_RANGE_FULL";
	}

	return "unknown";
}

bool MultitrackVideoOutput::ApplyConfigOverride(const std::string &custom_config_json, std::string *failure_reason)
{
	auto fail = [&](const char *reason) {
		blog(LOG_ERROR, "MultitrackVideoOutput: failed to apply config override live: %s", reason);
		if (failure_reason)
			*failure_reason = reason;
		return false;
	};

	if (custom_config_json.empty())
		return fail("the config override text is empty");

	GoLiveApi::Config new_config;
	try {
		new_config = nlohmann::json::parse(custom_config_json);
	} catch (const nlohmann::json::exception &e) {
		std::string reason = "invalid JSON: ";
		reason += e.what();
		return fail(reason.c_str());
	}

	if (new_config.encoder_configurations.empty())
		return fail("the config contains no video encoder configurations");
	for (const auto &encoder_config : new_config.encoder_configurations) {
		if (!encoder_config.settings.is_object() || encoder_config.settings.empty())
			return fail("a video encoder configuration has no settings object");
	}

	size_t canvas_count;
	{
		const std::lock_guard lock{current_mutex};
		canvas_count = current ? current->canvases.size() : 0u;
	}
	for (const auto &encoder_config : new_config.encoder_configurations) {
		if (encoder_config.canvas_index >= canvas_count)
			return fail("the config references a canvas that is not in use");
	}

	const std::lock_guard lock{current_mutex};
	if (!current || !current->output_)
		return fail("no multitrack video output has been prepared yet");
	if (!obs_output_active(current->output_))
		return fail("the stream is not active yet");

	auto &objects = *current;
	if (objects.config_json_.empty())
		return fail("the active session has no stored config to compare against");

	GoLiveApi::Config old_config;
	try {
		old_config = nlohmann::json::parse(objects.config_json_);
	} catch (const nlohmann::json::exception &e) {
		std::string reason = "the stored session config is corrupted: ";
		reason += e.what();
		return fail(reason.c_str());
	}

	// Nothing changed, nothing to do.
	if (objects.config_json_ == nlohmann::json(new_config).dump()) {
		blog(LOG_INFO, "MultitrackVideoOutput: config override unchanged, nothing to apply");
		return true;
	}

	const auto &new_videos = new_config.encoder_configurations;
	const auto &old_videos = old_config.encoder_configurations;

	if (old_videos.size() != new_videos.size()) {
		char line[128];
		snprintf(line, sizeof(line), "video track count %zu -> %zu", old_videos.size(), new_videos.size());
		blog(LOG_WARNING, "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts",
		     line);
	}

	for (size_t i = 0; old_videos.size() > i && new_videos.size() > i; i++) {
		const auto &old_encoder_config = old_videos[i];
		const auto &new_encoder_config = new_videos[i];

		if (old_encoder_config.type != new_encoder_config.type) {
			char line[192];
			snprintf(line, sizeof(line), "video encoder %zu codec (%s -> %s)", i,
				 old_encoder_config.type.c_str(), new_encoder_config.type.c_str());
			blog(LOG_WARNING,
			     "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts", line);
			continue;
		}

		const bool framerate_changed =
		    old_encoder_config.framerate.has_value() != new_encoder_config.framerate.has_value() ||
		    (old_encoder_config.framerate && new_encoder_config.framerate &&
		     ((*old_encoder_config.framerate).numerator != (*new_encoder_config.framerate).numerator ||
		      (*old_encoder_config.framerate).denominator != (*new_encoder_config.framerate).denominator));

		const bool structural_video_change =
		    old_encoder_config.width != new_encoder_config.width ||
		    old_encoder_config.height != new_encoder_config.height ||
		    old_encoder_config.gpu_scale_type != new_encoder_config.gpu_scale_type ||
		    old_encoder_config.colorspace != new_encoder_config.colorspace ||
		    old_encoder_config.range != new_encoder_config.range ||
		    old_encoder_config.format != new_encoder_config.format;

		const bool is_nvenc = strstr(new_encoder_config.type.c_str(), "nvenc") != nullptr;
		const bool resolution_changed = old_encoder_config.width != new_encoder_config.width ||
		    old_encoder_config.height != new_encoder_config.height;
		const bool scale_type_changed = old_encoder_config.gpu_scale_type != new_encoder_config.gpu_scale_type;
		const bool colorspace_changed = old_encoder_config.colorspace != new_encoder_config.colorspace;
		const bool range_changed = old_encoder_config.range != new_encoder_config.range;
		const bool format_changed = old_encoder_config.format != new_encoder_config.format;

		obs_encoder_t *encoder = obs_output_get_video_encoder2(objects.output_, i);
		if (!encoder) {
			blog(LOG_ERROR, "MultitrackVideoOutput: failed to get video encoder %zu for live update", i);
			continue;
		}

		if (is_nvenc) {
			// NVENC applies scale type and resolution changes live: the libobs setter re-points the
			// GPU rescale mix right away (see live_rebind_encoder_mix in obs-encoder.c), and obs-nvenc
			// detects input size changes on its encode side, resizing the driver session in place. The
			// forced keyframe realignment happens at the next shared GOP boundary so the multitrack
			// tracks stay keyframe-aligned (see nvenc_maybe_resize).
			// Colorspace/range/format changes stay deferred until a stream restart: live rebinding does
			// not update the NVENC session's color parameters mid-stream, so the encoded output would not
			// reflect them reliably.

			if (scale_type_changed && new_encoder_config.gpu_scale_type.has_value()) {
				char line[160];
				snprintf(line, sizeof(line), "video encoder %zu gpu_scale_type -> %s", i,
					 scale_type_to_string(*new_encoder_config.gpu_scale_type));
				blog(LOG_INFO, "MultitrackVideoOutput: applied live:%s", line);
				obs_encoder_set_gpu_scale_type(encoder, *new_encoder_config.gpu_scale_type);
			}

			if (colorspace_changed) {
				char line[160];
				snprintf(line, sizeof(line), "video encoder %zu colorspace (%s -> %s)", i,
					 colorspace_to_string(obs_encoder_get_preferred_color_space(encoder)),
					 new_encoder_config.colorspace.has_value() ? colorspace_to_string(*new_encoder_config.colorspace)
					                                                      : "default");
				blog(LOG_WARNING,
				     "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts", line);
			}

			if (range_changed) {
				char line[160];
				snprintf(line, sizeof(line), "video encoder %zu range (%s -> %s)", i,
					 range_to_string(obs_encoder_get_preferred_range(encoder)),
					 new_encoder_config.range.has_value() ? range_to_string(*new_encoder_config.range) : "default");
				blog(LOG_WARNING,
				     "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts", line);
			}

			if (format_changed) {
				char line[160];
				snprintf(line, sizeof(line), "video encoder %zu format (%s -> %s)", i,
					 get_video_format_name(obs_encoder_get_preferred_video_format(encoder)),
					 new_encoder_config.format.has_value() ? get_video_format_name(*new_encoder_config.format) : "default");
				blog(LOG_WARNING,
				     "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts", line);
			}

			if (resolution_changed) {
				char line[160];
				snprintf(line, sizeof(line), "video encoder %zu resolution %" PRIu32 "x%" PRIu32 " -> %" PRIu32
				             "x%" PRIu32,
				         i, old_encoder_config.width, old_encoder_config.height, new_encoder_config.width,
				         new_encoder_config.height);
				blog(LOG_INFO, "MultitrackVideoOutput: applied live:%s (NVENC resizes at the next frame)", line);
				obs_encoder_set_scaled_size(encoder, new_encoder_config.width, new_encoder_config.height);
			}
		} else if (structural_video_change) {
			char line[160];
			snprintf(line, sizeof(line), "video encoder %zu resolution/scale/color settings", i);
			blog(LOG_WARNING,
			     "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts", line);
		}

		nlohmann::json new_settings = new_encoder_config.settings;
		if (strstr(new_encoder_config.type.c_str(), "vaapi")) {
			// VAAPI encoders take an integer profile that is derived at create time only.
			new_settings.erase("profile");
			new_settings.erase("profile_str");
		}

		OBSDataAutoRelease settings = obs_data_create_from_json(new_settings.dump().c_str());

		log_changed_settings_fields("video", i, old_encoder_config.settings, new_settings);
		obs_encoder_update(encoder, settings);

		if (framerate_changed) {
			uint32_t new_divisor = 1u; // base frame rate when `framerate` is not specified
			bool divisor_ok = true;

			if (new_encoder_config.framerate.has_value()) {
				obs_video_info canvas_ovi;
				if (!obs_canvas_get_video_info(objects.canvases[new_encoder_config.canvas_index], &canvas_ovi)) {
					blog(LOG_ERROR, "MultitrackVideoOutput: failed to get canvas video info for encoder %zu", i);
					divisor_ok = false;
				} else {
					new_divisor = closest_divisor(canvas_ovi, *new_encoder_config.framerate);
				}
			}

			if (divisor_ok && obs_encoder_update_frame_rate_divisor(encoder, new_divisor)) {
				char line[160];
				snprintf(line, sizeof(line), "video encoder %zu frame-rate divisor -> %u", i, new_divisor);
				blog(LOG_INFO, "MultitrackVideoOutput: applied live:%s", line);
			} else if (!divisor_ok) {
				char line[160];
				snprintf(line, sizeof(line), "video encoder %zu framerate", i);
				blog(LOG_WARNING,
				     "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts",
				     line);
			} else {
				blog(LOG_ERROR,
				     "MultitrackVideoOutput: failed to update frame-rate divisor for video encoder %zu,"
				     " the old rate stays active until streaming restarts",
				     i);
			}
		}
	}

	auto old_audios = collect_audio_configurations(old_config);
	auto new_audios = collect_audio_configurations(new_config);

	if (old_audios.size() != new_audios.size()) {
		char line[128];
		snprintf(line, sizeof(line), "audio track count %zu -> %zu", old_audios.size(), new_audios.size());
		blog(LOG_WARNING, "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts",
		     line);
	}

	for (size_t i = 0; old_audios.size() > i && new_audios.size() > i; i++) {
		if (old_audios[i].codec != new_audios[i].codec || old_audios[i].channels != new_audios[i].channels) {
			char line[192];
			snprintf(line, sizeof(line), "audio encoder %zu codec/channels (%s -> %s)", i,
				 old_audios[i].codec.c_str(), new_audios[i].codec.c_str());
			blog(LOG_WARNING,
			     "MultitrackVideoOutput: deferred change (%s) - applies when streaming restarts", line);
			continue;
		}

		if (i >= objects.audio_encoders_.size()) {
			blog(LOG_ERROR, "MultitrackVideoOutput: missing audio encoder %zu for live update", i);
			break;
		}

		nlohmann::json track_settings =
			apply_audio_vbr_override(new_audios[i], obs_encoder_get_id(objects.audio_encoders_[i]));
		log_changed_settings_fields("audio", i, old_audios[i].settings, track_settings);
		OBSDataAutoRelease settings = obs_data_create_from_json(track_settings.dump().c_str());
		obs_encoder_update(objects.audio_encoders_[i], settings);
	}

	objects.config_json_ = nlohmann::json(new_config).dump();
	{
		const std::lock_guard dump_lock{current_stream_dump_mutex};
		if (current_stream_dump)
			current_stream_dump->config_json_ = objects.config_json_;
	}

	blog(LOG_INFO, "MultitrackVideoOutput: applied live config override changes to the active stream");
	return true;
}

signal_handler_t *MultitrackVideoOutput::StreamingSignalHandler()
{
	const std::lock_guard current_lock{current_mutex};
	return current.has_value() ? obs_output_get_signal_handler(current->output_) : nullptr;
}

void MultitrackVideoOutput::StartedStreaming()
{
	OBSOutputAutoRelease dump_output;
	{
		const std::lock_guard current_stream_dump_lock{current_stream_dump_mutex};
		if (current_stream_dump && current_stream_dump->output_) {
			dump_output = obs_output_get_ref(current_stream_dump->output_);
		}
	}

	if (!dump_output)
		return;

	auto result = obs_output_start(dump_output);
	blog(LOG_INFO, "MultitrackVideoOutput: starting recording%s", result ? "" : " failed");
}

void MultitrackVideoOutput::StopStreaming()
{
	restart_on_error = false;

	OBSOutputAutoRelease current_output;
	{
		const std::lock_guard current_lock{current_mutex};
		if (current && current->output_)
			current_output = obs_output_get_ref(current->output_);
	}
	if (current_output)
		obs_output_stop(current_output);

	OBSOutputAutoRelease dump_output;
	{
		const std::lock_guard current_stream_dump_lock{current_stream_dump_mutex};
		if (current_stream_dump && current_stream_dump->output_)
			dump_output = obs_output_get_ref(current_stream_dump->output_);
	}
	if (dump_output)
		obs_output_stop(dump_output);
}

bool MultitrackVideoOutput::HandleIncompatibleSettings(QWidget *parent, config_t *config, obs_service_t *service,
						       bool &enableDynBitrate)
{
	QString incompatible_settings;
	QString where_to_disable;
	QString incompatible_settings_list;

	size_t num = 1;

	auto check_setting = [&](bool setting, const char *name, const char *section) {
		if (!setting)
			return;

		incompatible_settings += QString(" %1. %2\n").arg(num).arg(QTStr(name));

		where_to_disable += QString(" %1. [%2 → %3 → %4]\n")
					    .arg(num)
					    .arg(QTStr("Settings"))
					    .arg(QTStr("Basic.Settings.Advanced"))
					    .arg(QTStr(section));

		incompatible_settings_list += QString("%1, ").arg(name);

		num += 1;
	};

	check_setting(enableDynBitrate, "Basic.Settings.Output.DynamicBitrate.Beta", "Basic.Settings.Advanced.Network");

	if (incompatible_settings.isEmpty())
		return true;

	OBSDataAutoRelease service_settings = obs_service_get_settings(service);

	QMessageBox mb(parent);
	mb.setIcon(QMessageBox::Critical);
	mb.setWindowTitle(QTStr("MultitrackVideo.IncompatibleSettings.Title"));
	mb.setText(QString(QTStr("MultitrackVideo.IncompatibleSettings.Text"))
			   .arg(obs_data_get_string(service_settings, "multitrack_video_name"))
			   .arg(incompatible_settings)
			   .arg(where_to_disable));
	auto this_stream = mb.addButton(QTStr("MultitrackVideo.IncompatibleSettings.DisableAndStartStreaming"),
					QMessageBox::AcceptRole);
	auto all_streams = mb.addButton(QString(QTStr("MultitrackVideo.IncompatibleSettings.UpdateAndStartStreaming")),
					QMessageBox::AcceptRole);
	mb.setStandardButtons(QMessageBox::StandardButton::Cancel);

	mb.exec();

	const char *action = "cancel";
	if (mb.clickedButton() == this_stream) {
		action = "DisableAndStartStreaming";
	} else if (mb.clickedButton() == all_streams) {
		action = "UpdateAndStartStreaming";
	}

	blog(LOG_INFO,
	     "MultitrackVideoOutput: attempted to start stream with incompatible"
	     "settings (%s); action taken: %s",
	     incompatible_settings_list.toUtf8().constData(), action);

	if (mb.clickedButton() == this_stream || mb.clickedButton() == all_streams) {
		enableDynBitrate = false;

		if (mb.clickedButton() == all_streams) {
			config_set_bool(config, "Output", "DynamicBitrate", false);
		}

		return true;
	}

	MultitrackVideoOutput::ReleaseOnMainThread(take_current());
	MultitrackVideoOutput::ReleaseOnMainThread(take_current_stream_dump());

	return false;
}

static bool create_video_encoders(const GoLiveApi::Config &go_live_config,
				  std::shared_ptr<obs_encoder_group_t> &video_encoder_group, obs_output_t *output,
				  obs_output_t *recording_output, const std::vector<OBSCanvasAutoRelease> &canvases)
{
	DStr video_encoder_name_buffer;
	if (go_live_config.encoder_configurations.empty()) {
		blog(LOG_WARNING, "MultitrackVideoOutput: Missing video encoder configurations");
		throw MultitrackVideoError::warning(QTStr("FailedToStartStream.MissingEncoderConfigs"));
	}

	std::shared_ptr<obs_encoder_group_t> encoder_group(obs_encoder_group_create(), obs_encoder_group_destroy);
	if (!encoder_group)
		return false;

	auto max_canvas_idx = canvases.size() - 1;

	for (size_t i = 0; i < go_live_config.encoder_configurations.size(); i++) {
		auto &config = go_live_config.encoder_configurations[i];
		if (config.canvas_index > max_canvas_idx) {
			blog(LOG_ERROR, "MultitrackVideoOutput: Invalid canvas index: %u", config.canvas_index);
			throw MultitrackVideoError::warning(QTStr("FailedToStartStream.InvalidEncoderConfig"));
		}

		auto &canvas = canvases[config.canvas_index];
		auto encoder = create_video_encoder(video_encoder_name_buffer, i, config, canvas);
		if (!encoder)
			return false;

		if (!obs_encoder_set_group(encoder, encoder_group.get()))
			return false;

		obs_output_set_video_encoder2(output, encoder, i);
		if (recording_output)
			obs_output_set_video_encoder2(recording_output, encoder, i);
	}

	video_encoder_group = encoder_group;
	return true;
}

static void create_audio_encoders(const GoLiveApi::Config &go_live_config,
				  std::vector<OBSEncoderAutoRelease> &audio_encoders, obs_output_t *output,
				  obs_output_t *recording_output, const char *audio_encoder_id, size_t main_audio_mixer,
				  std::optional<size_t> vod_track_mixer, std::vector<speaker_layout> &speaker_layouts,
				  speaker_layout &current_layout)
{
	speaker_layout speakers = SPEAKERS_UNKNOWN;
	obs_audio_info oai = {};
	if (obs_get_audio_info(&oai))
		speakers = oai.speakers;

	current_layout = speakers;

	auto sanitize_audio_channels = [&](obs_encoder_t *encoder, uint32_t channels) {
		speaker_layout target_speakers = SPEAKERS_UNKNOWN;
		for (size_t i = 0; i <= (size_t)SPEAKERS_7POINT1; i++) {
			if (get_audio_channels((speaker_layout)i) != channels)
				continue;

			target_speakers = (speaker_layout)i;
			break;
		}
		if (target_speakers == SPEAKERS_UNKNOWN) {
			blog(LOG_WARNING,
			     "MultitrackVideoOutput: Could not find "
			     "speaker layout for %" PRIu32 "channels "
			     "while configuring encoder '%s'",
			     channels, obs_encoder_get_name(encoder));
			return;
		}
		if (speakers != SPEAKERS_UNKNOWN &&
		    (channels > get_audio_channels(speakers) || speakers == target_speakers))
			return;

		auto it = std::find(std::begin(speaker_layouts), std::end(speaker_layouts), target_speakers);
		if (it == std::end(speaker_layouts))
			speaker_layouts.push_back(target_speakers);
	};

	using encoder_configs_type = decltype(go_live_config.audio_configurations.live);
	DStr encoder_name_buffer;
	size_t output_encoder_index = 0;

	auto create_encoders = [&](const char *name_prefix, const encoder_configs_type &configs, size_t mixer_idx) {
		if (configs.empty()) {
			blog(LOG_WARNING, "MultitrackVideoOutput: Missing audio encoder configurations (for '%s')",
			     name_prefix);
			throw MultitrackVideoError::warning(QTStr("FailedToStartStream.MissingEncoderConfigs"));
		}

		for (size_t i = 0; i < configs.size(); i++) {
			dstr_printf(encoder_name_buffer, "%s %zu", name_prefix, i);
			const char *track_encoder_id = resolve_audio_encoder_id(configs[i].codec.c_str(), audio_encoder_id);
			nlohmann::json track_settings = apply_audio_vbr_override(configs[i], track_encoder_id);
			OBSDataAutoRelease settings = obs_data_create_from_json(track_settings.dump().c_str());
			OBSEncoderAutoRelease audio_encoder =
				create_audio_encoder(encoder_name_buffer->array, track_encoder_id, settings, mixer_idx);

			sanitize_audio_channels(audio_encoder, configs[i].channels);

			obs_output_set_audio_encoder(output, audio_encoder, output_encoder_index);
			if (recording_output)
				obs_output_set_audio_encoder(recording_output, audio_encoder, output_encoder_index);
			output_encoder_index += 1;
			audio_encoders.emplace_back(std::move(audio_encoder));
		}
	};

	create_encoders("multitrack video live audio", go_live_config.audio_configurations.live, main_audio_mixer);

	if (!vod_track_mixer.has_value())
		return;

	// we already check for empty inside of `create_encoders`
	encoder_configs_type empty = {};
	create_encoders("multitrack video vod audio", go_live_config.audio_configurations.vod.value_or(empty),
			*vod_track_mixer);

	return;
}

static const char *speaker_layout_to_string(speaker_layout layout)
{
	switch (layout) {
	case SPEAKERS_MONO:
		return "Mono";
	case SPEAKERS_2POINT1:
		return "2.1";
	case SPEAKERS_4POINT0:
		return "4.0";
	case SPEAKERS_4POINT1:
		return "4.1";
	case SPEAKERS_5POINT1:
		return "5.1";
	case SPEAKERS_7POINT1:
		return "7.1";
	case SPEAKERS_UNKNOWN:
	case SPEAKERS_STEREO:
		return "Stereo";
	}

	return "Stereo";
}

static void handle_speaker_layout_issues(QWidget *parent, const QString &multitrack_video_name,
					 const std::vector<speaker_layout> &requested_layouts, speaker_layout layout)
{
	if (requested_layouts.empty())
		return;

	QString message;
	if (requested_layouts.size() == 1) {
		message = QTStr("MultitrackVideo.IncompatibleSettings.AudioChannelsSingle")
				  .arg(QTStr(speaker_layout_to_string(requested_layouts.front())));
	} else {
		message =
			QTStr("MultitrackVideo.IncompatibleSettings.AudioChannelsMultiple").arg(multitrack_video_name);
	}

	QMetaObject::invokeMethod(
		parent,
		[&] {
			QMessageBox mb(parent);
			mb.setIcon(QMessageBox::Critical);
			mb.setWindowTitle(QTStr("MultitrackVideo.IncompatibleSettings.Title"));
			mb.setText(QTStr("MultitrackVideo.IncompatibleSettings.AudioChannels")
					   .arg(multitrack_video_name)
					   .arg(QTStr(speaker_layout_to_string(layout)))
					   .arg(message));

			mb.setStandardButtons(QMessageBox::StandardButton::Cancel);

			mb.exec();
		},
		BlockingConnectionTypeFor(parent));

	blog(LOG_INFO, "MultitrackVideoOutput: Attempted to start stream with incompatible "
		       "audio channel setting. Action taken: cancel");

	throw MultitrackVideoError::cancel();
}

static OBSOutputs SetupOBSOutput(QWidget *parent, const QString &multitrack_video_name,
				 obs_data_t *dump_stream_to_file_config, const GoLiveApi::Config &go_live_config,
				 std::vector<OBSEncoderAutoRelease> &audio_encoders,
				 std::shared_ptr<obs_encoder_group_t> &video_encoder_group,
				 const char *audio_encoder_id, size_t main_audio_mixer,
				 std::optional<size_t> vod_track_mixer,
				 const std::vector<OBSCanvasAutoRelease> &canvases)
{
	auto output = create_output();
	OBSOutputAutoRelease recording_output;
	if (dump_stream_to_file_config)
		recording_output = create_recording_output(dump_stream_to_file_config);

	if (!create_video_encoders(go_live_config, video_encoder_group, output, recording_output, canvases))
		return {nullptr, nullptr};

	std::vector<speaker_layout> requested_speaker_layouts;
	speaker_layout current_layout = SPEAKERS_UNKNOWN;
	create_audio_encoders(go_live_config, audio_encoders, output, recording_output, audio_encoder_id,
			      main_audio_mixer, vod_track_mixer, requested_speaker_layouts, current_layout);

	handle_speaker_layout_issues(parent, multitrack_video_name, requested_speaker_layouts, current_layout);

	return {std::move(output), std::move(recording_output)};
}

void SetupSignalHandlers(bool recording, MultitrackVideoOutput *self, obs_output_t *output, OBSSignal &start,
			 OBSSignal &stop)
{
	auto handler = obs_output_get_signal_handler(output);

	start.Connect(handler, "start", !recording ? StreamStartHandler : RecordingStartHandler, self);

	stop.Connect(handler, "stop", !recording ? StreamStopHandler : RecordingStopHandler, self);
}

std::optional<MultitrackVideoOutput::OBSOutputObjects> MultitrackVideoOutput::take_current()
{
	const std::lock_guard<std::mutex> current_lock{current_mutex};
	auto val = std::move(current);
	current.reset();
	return val;
}

std::optional<MultitrackVideoOutput::OBSOutputObjects> MultitrackVideoOutput::take_current_stream_dump()
{
	const std::lock_guard<std::mutex> current_stream_dump_lock{current_stream_dump_mutex};
	auto val = std::move(current_stream_dump);
	current_stream_dump.reset();
	return val;
}

void MultitrackVideoOutput::ReleaseOnMainThread(std::optional<OBSOutputObjects> objects)
{

	if (!objects.has_value())
		return;

	QMetaObject::invokeMethod(
		QApplication::instance()->thread(), [objects = std::move(objects)] {}, Qt::QueuedConnection);
}

void StreamStartHandler(void *arg, calldata_t *)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	self->restart_on_error = true;
	self->reconnect_attempts = 0;
}

void StreamStopHandler(void *arg, calldata_t *data)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);

	OBSOutputAutoRelease stream_dump_output;
	{
		const std::lock_guard<std::mutex> current_stream_dump_lock{self->current_stream_dump_mutex};
		if (self->current_stream_dump && self->current_stream_dump->output_)
			stream_dump_output = obs_output_get_ref(self->current_stream_dump->output_);
	}
	if (stream_dump_output)
		obs_output_stop(stream_dump_output);

	/* Unregister the BPM (Broadcast Performance Metrics) callback and destroy the allocated metrics data. */
	obs_output_remove_packet_callback(static_cast<obs_output_t *>(calldata_ptr(data, "output")), bpm_inject, NULL);
	bpm_destroy(static_cast<obs_output_t *>(calldata_ptr(data, "output")));

	MultitrackVideoOutput::ReleaseOnMainThread(self->take_current());
}

void RecordingStartHandler(void * /* arg */, calldata_t * /* data */)
{
	blog(LOG_INFO, "MultitrackVideoOutput: recording started");
}

void RecordingStopHandler(void *arg, calldata_t *)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	blog(LOG_INFO, "MultitrackVideoOutput: recording stopped");
	MultitrackVideoOutput::ReleaseOnMainThread(self->take_current_stream_dump());
}
