#include "AdvancedOutput.hpp"

#include <utility/audio-encoders.hpp>
#include <utility/StartMultiTrackVideoStreamingGuard.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

using namespace std;

extern bool EncoderAvailable(const char *encoder);

static bool AdvTrackVBRActive(config_t *config, size_t track)
{
	string key = "Track";
	key += to_string(track + 1);
	key += "BitrateMode";
	if (strcmp(config_get_string(config, "AdvOut", key.c_str()), "VBR") != 0)
		return false;

	return EncoderAvailable("CoreAudio_AAC");
}

static int AdvTrackQualityTarget(config_t *config, size_t track)
{
	string key = "Track";
	key += to_string(track + 1);
	key += "QualityTarget";
	int quality = (int)config_get_int(config, "AdvOut", key.c_str());
	if (quality < 0)
		quality = 0;
	if (quality > 127)
		quality = 127;
	return quality;
}

static OBSData GetDataFromJsonFile(const char *jsonFile)
{
	const OBSBasic *basic = OBSBasic::Get();

	const OBSProfile &currentProfile = basic->GetCurrentProfile();

	const std::filesystem::path jsonFilePath = currentProfile.path / std::filesystem::u8path(jsonFile);

	OBSDataAutoRelease data = nullptr;

	if (!jsonFilePath.empty()) {
		BPtr<char> jsonData = os_quick_read_utf8_file(jsonFilePath.u8string().c_str());

		if (!!jsonData) {
			data = obs_data_create_from_json(jsonData);
		}
	}

	if (!data) {
		data = obs_data_create();
	}

	return data.Get();
}

static void ApplyEncoderDefaults(OBSData &settings, const obs_encoder_t *encoder)
{
	OBSData dataRet = obs_encoder_get_defaults(encoder);
	obs_data_release(dataRet);

	if (!!settings)
		obs_data_apply(dataRet, settings);
	settings = std::move(dataRet);
}

#define ADV_ARCHIVE_NAME "adv_archive_audio"

#ifdef __APPLE__
static void translate_macvth264_encoder(const char *&encoder)
{
	if (strcmp(encoder, "vt_h264_hw") == 0) {
		encoder = "com.apple.videotoolbox.videoencoder.h264.gva";
	} else if (strcmp(encoder, "vt_h264_sw") == 0) {
		encoder = "com.apple.videotoolbox.videoencoder.h264";
	}
}
#endif

AdvancedOutput::AdvancedOutput(OBSBasic *main_) : BasicOutputHandler(main_)
{
	const char *recType = config_get_string(main->Config(), "AdvOut", "RecType");
	const char *streamEncoder = config_get_string(main->Config(), "AdvOut", "Encoder");
	const char *streamAudioEncoder = config_get_string(main->Config(), "AdvOut", "AudioEncoder");
	const char *recordEncoder = config_get_string(main->Config(), "AdvOut", "RecEncoder");
	const char *recAudioEncoder = config_get_string(main->Config(), "AdvOut", "RecAudioEncoder");
	const char *recFormat = config_get_string(main->Config(), "AdvOut", "RecFormat2");
#ifdef __APPLE__
	translate_macvth264_encoder(streamEncoder);
	translate_macvth264_encoder(recordEncoder);
#endif

	ffmpegOutput = astrcmpi(recType, "FFmpeg") == 0;
	ffmpegRecording = ffmpegOutput && config_get_bool(main->Config(), "AdvOut", "FFOutputToFile");
	useStreamEncoder = astrcmpi(recordEncoder, "none") == 0;
	useStreamAudioEncoder = astrcmpi(recAudioEncoder, "none") == 0;

	OBSData streamEncSettings = GetDataFromJsonFile("streamEncoder.json");
	OBSData recordEncSettings = GetDataFromJsonFile("recordEncoder.json");

	if (ffmpegOutput) {
		fileOutput = obs_output_create("ffmpeg_output", "adv_ffmpeg_output", nullptr, nullptr);
		if (!fileOutput)
			throw "Failed to create recording FFmpeg output "
			      "(advanced output)";
	} else {
		bool useReplayBuffer = config_get_bool(main->Config(), "AdvOut", "RecRB");
		if (useReplayBuffer) {
			OBSDataAutoRelease hotkey;
			const char *str = config_get_string(main->Config(), "Hotkeys", "ReplayBuffer");
			if (str)
				hotkey = obs_data_create_from_json(str);
			else
				hotkey = nullptr;

			replayBuffer = obs_output_create("replay_buffer", Str("ReplayBuffer"), nullptr, hotkey);

			if (!replayBuffer)
				throw "Failed to create replay buffer output "
				      "(simple output)";

			signal_handler_t *signal = obs_output_get_signal_handler(replayBuffer);

			startReplayBuffer.Connect(signal, "start", OBSStartReplayBuffer, this);
			stopReplayBuffer.Connect(signal, "stop", OBSStopReplayBuffer, this);
			replayBufferStopping.Connect(signal, "stopping", OBSReplayBufferStopping, this);
			replayBufferSaved.Connect(signal, "saved", OBSReplayBufferSaved, this);
		}

		const char *mux = "ffmpeg_muxer";
		if (strcmp(recFormat, "hybrid_mp4") == 0)
			mux = "mp4_output";
		else if (strcmp(recFormat, "hybrid_mov") == 0)
			mux = "mov_output";

		fileOutput = obs_output_create(mux, "adv_file_output", nullptr, nullptr);
		if (!fileOutput)
			throw "Failed to create recording output "
			      "(advanced output)";

		if (!useStreamEncoder) {
			videoRecording = obs_video_encoder_create(recordEncoder, "advanced_video_recording",
								  recordEncSettings, nullptr);
			if (!videoRecording)
				throw "Failed to create recording video "
				      "encoder (advanced output)";
			obs_encoder_release(videoRecording);
		}
	}

	videoStreaming = obs_video_encoder_create(streamEncoder, "advanced_video_stream", streamEncSettings, nullptr);
	if (!videoStreaming)
		throw "Failed to create streaming video encoder "
		      "(advanced output)";
	obs_encoder_release(videoStreaming);

	const char *rate_control =
		obs_data_get_string(useStreamEncoder ? streamEncSettings : recordEncSettings, "rate_control");
	if (!rate_control)
		rate_control = "";
	usesBitrate = astrcmpi(rate_control, "CBR") == 0 || astrcmpi(rate_control, "VBR") == 0 ||
		      astrcmpi(rate_control, "ABR") == 0;

	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		char name[19];
		snprintf(name, sizeof(name), "adv_record_audio_%d", i);

		recordTrack[i] = obs_audio_encoder_create(useStreamAudioEncoder ? streamAudioEncoder : recAudioEncoder,
							  name, nullptr, i, nullptr);

		if (!recordTrack[i]) {
			throw "Failed to create audio encoder "
			      "(advanced output)";
		}

		obs_encoder_release(recordTrack[i]);

		snprintf(name, sizeof(name), "adv_stream_audio_%d", i);
		streamTrack[i] = obs_audio_encoder_create(streamAudioEncoder, name, nullptr, i, nullptr);

		if (!streamTrack[i]) {
			throw "Failed to create streaming audio encoders "
			      "(advanced output)";
		}

		obs_encoder_release(streamTrack[i]);
	}

	std::string id;
	int streamTrackIndex = config_get_int(main->Config(), "AdvOut", "TrackIndex") - 1;
	streamAudioEnc =
		obs_audio_encoder_create(streamAudioEncoder, "adv_stream_audio", nullptr, streamTrackIndex, nullptr);
	if (!streamAudioEnc)
		throw "Failed to create streaming audio encoder "
		      "(advanced output)";
	obs_encoder_release(streamAudioEnc);

	id = "";
	int vodTrack = config_get_int(main->Config(), "AdvOut", "VodTrackIndex") - 1;
	streamArchiveEnc = obs_audio_encoder_create(streamAudioEncoder, ADV_ARCHIVE_NAME, nullptr, vodTrack, nullptr);
	if (!streamArchiveEnc)
		throw "Failed to create archive audio encoder "
		      "(advanced output)";
	obs_encoder_release(streamArchiveEnc);

	startRecording.Connect(obs_output_get_signal_handler(fileOutput), "start", OBSStartRecording, this);
	stopRecording.Connect(obs_output_get_signal_handler(fileOutput), "stop", OBSStopRecording, this);
	recordStopping.Connect(obs_output_get_signal_handler(fileOutput), "stopping", OBSRecordStopping, this);
	recordFileChanged.Connect(obs_output_get_signal_handler(fileOutput), "file_changed", OBSRecordFileChanged,
				  this);
}

void AdvancedOutput::UpdateStreamSettings()
{
	bool applyServiceSettings = config_get_bool(main->Config(), "AdvOut", "ApplyServiceSettings");
	bool enforceBitrate = !config_get_bool(main->Config(), "Stream1", "IgnoreRecommended");
	bool dynBitrate = config_get_bool(main->Config(), "Output", "DynamicBitrate");
	const char *streamEncoder = config_get_string(main->Config(), "AdvOut", "Encoder");

	OBSData settings = GetDataFromJsonFile("streamEncoder.json");
	ApplyEncoderDefaults(settings, videoStreaming);

	if (applyServiceSettings) {
		int bitrate = (int)obs_data_get_int(settings, "bitrate");
		int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
		obs_service_apply_encoder_settings(main->GetService(), settings, nullptr);
		if (!enforceBitrate) {
			int maxVideoBitrate;
			int maxAudioBitrate;
			obs_service_get_max_bitrate(main->GetService(), &maxVideoBitrate, &maxAudioBitrate);

			std::string videoBitRateLogString = maxVideoBitrate > 0 ? std::to_string(maxVideoBitrate)
										: "None";
			std::string audioBitRateLogString = maxAudioBitrate > 0 ? std::to_string(maxAudioBitrate)
										: "None";

			blog(LOG_INFO,
			     "User is ignoring service bitrate limits.\n"
			     "Service Recommendations:\n"
			     "\tvideo bitrate: %s\n"
			     "\taudio bitrate: %s",
			     videoBitRateLogString.c_str(), audioBitRateLogString.c_str());

			obs_data_set_int(settings, "bitrate", bitrate);
		}

		int enforced_keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
		if (keyint_sec != 0 && keyint_sec < enforced_keyint_sec)
			obs_data_set_int(settings, "keyint_sec", keyint_sec);
	} else {
		blog(LOG_WARNING, "User is ignoring service settings.");
	}

	if (dynBitrate && strstr(streamEncoder, "nvenc") != nullptr)
		obs_data_set_bool(settings, "lookahead", false);



	// Update frame rate divisor BEFORE obs_encoder_update so that nvenc_update can
	// detect the new effective FPS and update its internal frameRateNum/Den params.
	uint32_t stream_fps_divisor = config_get_uint(main->Config(), "AdvOut", "StreamFpsDivisor");
	if (stream_fps_divisor < 1)
		stream_fps_divisor = 1;
	obs_encoder_update_frame_rate_divisor(videoStreaming, stream_fps_divisor);

	obs_encoder_update(videoStreaming, settings);
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut", "RescaleRes");
	int rescaleFilter = config_get_int(main->Config(), "AdvOut", "RescaleFilter");
	uint32_t cx = 0, cy = 0;
	if (rescaleFilter != OBS_SCALE_DISABLE && rescaleRes && *rescaleRes) {
		if (sscanf(rescaleRes, "%ux%u", &cx, &cy) != 2) {
			cx = 0;
			cy = 0;
		}
	}
	obs_encoder_set_scaled_size(videoStreaming, cx, cy);
}

inline void AdvancedOutput::UpdateRecordingSettings()
{
	OBSData settings = GetDataFromJsonFile("recordEncoder.json");

	// Update frame rate divisor BEFORE obs_encoder_update so that nvenc_update can
	// detect the new effective FPS and update its internal frameRateNum/Den params.
	uint32_t rec_fps_divisor = config_get_uint(main->Config(), "AdvOut", "RecFpsDivisor");
	if (rec_fps_divisor < 1)
		rec_fps_divisor = 1;
	obs_encoder_update_frame_rate_divisor(videoRecording, rec_fps_divisor);

	obs_encoder_update(videoRecording, settings);

	// Apply live rescale resolution and filter changes while recording
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut", "RecRescaleRes");
	int rescaleFilter = config_get_int(main->Config(), "AdvOut", "RecRescaleFilter");
	uint32_t cx = 0, cy = 0;
	if (rescaleFilter != OBS_SCALE_DISABLE && rescaleRes && *rescaleRes) {
		if (sscanf(rescaleRes, "%ux%u", &cx, &cy) != 2) {
			cx = 0;
			cy = 0;
		}
	}
	obs_encoder_set_scaled_size(videoRecording, cx, cy);
	obs_encoder_set_gpu_scale_type(videoRecording, (obs_scale_type)rescaleFilter);
}

void AdvancedOutput::Update()
{
	UpdateStreamSettings();
	if (!useStreamEncoder && !ffmpegOutput)
		UpdateRecordingSettings();
	UpdateAudioSettings();
}

inline bool AdvancedOutput::allowsMultiTrack()
{
	const char *protocol = nullptr;
	obs_service_t *service_obj = main->GetService();
	protocol = obs_service_get_protocol(service_obj);
	if (!protocol)
		return false;
	return astrcmpi_n(protocol, SRT_PROTOCOL, strlen(SRT_PROTOCOL)) == 0 ||
	       astrcmpi_n(protocol, RIST_PROTOCOL, strlen(RIST_PROTOCOL)) == 0;
}

inline void AdvancedOutput::SetupStreaming()
{
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut", "RescaleRes");
	int rescaleFilter = config_get_int(main->Config(), "AdvOut", "RescaleFilter");
	int multiTrackAudioMixes = config_get_int(main->Config(), "AdvOut", "StreamMultiTrackAudioMixes");
	unsigned int cx = 0;
	unsigned int cy = 0;
	int idx = 0;
	bool is_multitrack_output = allowsMultiTrack();

	if (rescaleFilter != OBS_SCALE_DISABLE && rescaleRes && *rescaleRes) {
		if (sscanf(rescaleRes, "%ux%u", &cx, &cy) != 2) {
			cx = 0;
			cy = 0;
		}
	}

	if (!is_multitrack_output) {
		obs_output_set_audio_encoder(streamOutput, streamAudioEnc, 0);
	} else {
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if ((multiTrackAudioMixes & (1 << i)) != 0) {
				obs_output_set_audio_encoder(streamOutput, streamTrack[i], idx);
				idx++;
			}
		}
	}

	obs_encoder_set_scaled_size(videoStreaming, cx, cy);
	obs_encoder_set_gpu_scale_type(videoStreaming, (obs_scale_type)rescaleFilter);

	uint32_t stream_fps_divisor = config_get_uint(main->Config(), "AdvOut", "StreamFpsDivisor");
	if (stream_fps_divisor < 1)
		stream_fps_divisor = 1;
	obs_encoder_set_frame_rate_divisor(videoStreaming, stream_fps_divisor);

	const char *id = obs_service_get_id(main->GetService());
	if (strcmp(id, "rtmp_custom") == 0) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_service_apply_encoder_settings(main->GetService(), settings, nullptr);
		obs_encoder_update(videoStreaming, settings);
	}
}

inline void AdvancedOutput::SetupRecording()
{
	const char *path = config_get_string(main->Config(), "AdvOut", "RecFilePath");
	const char *mux = config_get_string(main->Config(), "AdvOut", "RecMuxerCustom");
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut", "RecRescaleRes");
	int rescaleFilter = config_get_int(main->Config(), "AdvOut", "RecRescaleFilter");
	int tracks;

	const char *recFormat = config_get_string(main->Config(), "AdvOut", "RecFormat2");

	bool is_fragmented = strncmp(recFormat, "fragmented", 10) == 0;
	bool flv = strcmp(recFormat, "flv") == 0;

	if (flv)
		tracks = config_get_int(main->Config(), "AdvOut", "FLVTrack");
	else
		tracks = config_get_int(main->Config(), "AdvOut", "RecTracks");

	OBSDataAutoRelease settings = obs_data_create();
	unsigned int cx = 0;
	unsigned int cy = 0;
	int idx = 0;

	/* Hack to allow recordings without any audio tracks selected. It is no
	 * longer possible to select such a configuration in settings, but legacy
	 * configurations might still have this configured and we don't want to
	 * just break them. */
	if (tracks == 0)
		tracks = config_get_int(main->Config(), "AdvOut", "TrackIndex");

	if (useStreamEncoder) {
		obs_output_set_video_encoder(fileOutput, videoStreaming);
		if (replayBuffer)
			obs_output_set_video_encoder(replayBuffer, videoStreaming);
	} else {
		if (rescaleFilter != OBS_SCALE_DISABLE && rescaleRes && *rescaleRes) {
			if (sscanf(rescaleRes, "%ux%u", &cx, &cy) != 2) {
				cx = 0;
				cy = 0;
			}
		}

		obs_encoder_set_scaled_size(videoRecording, cx, cy);
		obs_encoder_set_gpu_scale_type(videoRecording, (obs_scale_type)rescaleFilter);

		uint32_t rec_fps_divisor = config_get_uint(main->Config(), "AdvOut", "RecFpsDivisor");
		if (rec_fps_divisor < 1)
			rec_fps_divisor = 1;
		obs_encoder_set_frame_rate_divisor(videoRecording, rec_fps_divisor);

		obs_output_set_video_encoder(fileOutput, videoRecording);
		if (replayBuffer)
			obs_output_set_video_encoder(replayBuffer, videoRecording);
	}

	if (!flv) {
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if ((tracks & (1 << i)) != 0) {
				obs_output_set_audio_encoder(fileOutput, recordTrack[i], idx);
				if (replayBuffer)
					obs_output_set_audio_encoder(replayBuffer, recordTrack[i], idx);
				idx++;
			}
		}
	} else if (flv && tracks != 0) {
		obs_output_set_audio_encoder(fileOutput, recordTrack[tracks - 1], idx);

		if (replayBuffer)
			obs_output_set_audio_encoder(replayBuffer, recordTrack[tracks - 1], idx);
	}

	// Use fragmented MOV/MP4 if user has not already specified custom movflags
	if (is_fragmented && (!mux || strstr(mux, "movflags") == NULL)) {
		string mux_frag = "movflags=frag_keyframe+empty_moov+delay_moov";
		if (mux) {
			mux_frag += " ";
			mux_frag += mux;
		}
		obs_data_set_string(settings, "muxer_settings", mux_frag.c_str());
	} else {
		if (is_fragmented)
			blog(LOG_WARNING, "User enabled fragmented recording, "
					  "but custom muxer settings contained movflags.");
		obs_data_set_string(settings, "muxer_settings", mux);
	}

	obs_data_set_string(settings, "path", path);
	obs_output_update(fileOutput, settings);
	if (replayBuffer)
		obs_output_update(replayBuffer, settings);
}

inline void AdvancedOutput::SetupFFmpeg()
{
	const char *url = config_get_string(main->Config(), "AdvOut", "FFURL");
	int vBitrate = config_get_int(main->Config(), "AdvOut", "FFVBitrate");
	int gopSize = config_get_int(main->Config(), "AdvOut", "FFVGOPSize");
	bool rescale = config_get_bool(main->Config(), "AdvOut", "FFRescale");
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut", "FFRescaleRes");
	const char *formatName = config_get_string(main->Config(), "AdvOut", "FFFormat");
	const char *mimeType = config_get_string(main->Config(), "AdvOut", "FFFormatMimeType");
	const char *muxCustom = config_get_string(main->Config(), "AdvOut", "FFMCustom");
	const char *vEncoder = config_get_string(main->Config(), "AdvOut", "FFVEncoder");
	int vEncoderId = config_get_int(main->Config(), "AdvOut", "FFVEncoderId");
	const char *vEncCustom = config_get_string(main->Config(), "AdvOut", "FFVCustom");
	int aBitrate = config_get_int(main->Config(), "AdvOut", "FFABitrate");
	int aMixes = config_get_int(main->Config(), "AdvOut", "FFAudioMixes");
	const char *aEncoder = config_get_string(main->Config(), "AdvOut", "FFAEncoder");
	int aEncoderId = config_get_int(main->Config(), "AdvOut", "FFAEncoderId");
	const char *aEncCustom = config_get_string(main->Config(), "AdvOut", "FFACustom");

	OBSDataArrayAutoRelease audio_names = obs_data_array_create();

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		string cfg_name = "Track";
		cfg_name += to_string((int)i + 1);
		cfg_name += "Name";

		const char *audioName = config_get_string(main->Config(), "AdvOut", cfg_name.c_str());

		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", audioName);
		obs_data_array_push_back(audio_names, item);
	}

	OBSDataAutoRelease settings = obs_data_create();

	obs_data_set_array(settings, "audio_names", audio_names);
	obs_data_set_string(settings, "url", url);
	obs_data_set_string(settings, "format_name", formatName);
	obs_data_set_string(settings, "format_mime_type", mimeType);
	obs_data_set_string(settings, "muxer_settings", muxCustom);
	obs_data_set_int(settings, "gop_size", gopSize);
	obs_data_set_int(settings, "video_bitrate", vBitrate);
	obs_data_set_string(settings, "video_encoder", vEncoder);
	obs_data_set_int(settings, "video_encoder_id", vEncoderId);
	obs_data_set_string(settings, "video_settings", vEncCustom);
	obs_data_set_int(settings, "audio_bitrate", aBitrate);
	obs_data_set_string(settings, "audio_encoder", aEncoder);
	obs_data_set_int(settings, "audio_encoder_id", aEncoderId);
	obs_data_set_string(settings, "audio_settings", aEncCustom);

	if (rescale && rescaleRes && *rescaleRes) {
		int width;
		int height;
		int val = sscanf(rescaleRes, "%dx%d", &width, &height);

		if (val == 2 && width && height) {
			obs_data_set_int(settings, "scale_width", width);
			obs_data_set_int(settings, "scale_height", height);
		}
	}

	obs_output_set_mixers(fileOutput, aMixes);
	obs_output_set_media(fileOutput, obs_get_video(), obs_get_audio());
	obs_output_update(fileOutput, settings);
}

static inline void SetEncoderName(obs_encoder_t *encoder, const char *name, const char *defaultName)
{
	obs_encoder_set_name(encoder, (name && *name) ? name : defaultName);
}

inline void AdvancedOutput::UpdateAudioSettings()
{
	bool applyServiceSettings = config_get_bool(main->Config(), "AdvOut", "ApplyServiceSettings");
	bool enforceBitrate = !config_get_bool(main->Config(), "Stream1", "IgnoreRecommended");
	int streamTrackIndex = config_get_int(main->Config(), "AdvOut", "TrackIndex");
	int vodTrackIndex = config_get_int(main->Config(), "AdvOut", "VodTrackIndex");
	const char *audioEncoder = config_get_string(main->Config(), "AdvOut", "AudioEncoder");
	const char *recAudioEncoder = config_get_string(main->Config(), "AdvOut", "RecAudioEncoder");

	bool is_multitrack_output = allowsMultiTrack();

	OBSDataAutoRelease settings[MAX_AUDIO_MIXES];

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		string cfg_name = "Track";
		cfg_name += to_string((int)i + 1);
		cfg_name += "Name";
		const char *name = config_get_string(main->Config(), "AdvOut", cfg_name.c_str());

		string def_name = "Track";
		def_name += to_string((int)i + 1);
		SetEncoderName(recordTrack[i], name, def_name.c_str());
		SetEncoderName(streamTrack[i], name, def_name.c_str());
	}

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		int track = (int)(i + 1);
		settings[i] = obs_data_create();

		bool vbr_active = AdvTrackVBRActive(main->Config(), i);
		int quality_target = vbr_active ? AdvTrackQualityTarget(main->Config(), i) : 0;

		const char *rec_id = useStreamAudioEncoder ? audioEncoder : recAudioEncoder;
		if (vbr_active && strcmp(rec_id, "CoreAudio_AAC") == 0) {
			obs_data_set_bool(settings[i], "vbr", true);
			obs_data_set_int(settings[i], "quality_target", quality_target);
		} else {
			obs_data_set_int(settings[i], "bitrate", GetAudioBitrate(i, recAudioEncoder));
		}

		obs_encoder_update(recordTrack[i], settings[i]);

		if (vbr_active && strcmp(audioEncoder, "CoreAudio_AAC") == 0) {
			obs_data_set_bool(settings[i], "vbr", true);
			obs_data_set_int(settings[i], "quality_target", quality_target);
		} else {
			obs_data_set_int(settings[i], "bitrate", GetAudioBitrate(i, audioEncoder));
		}

		if (!is_multitrack_output) {
			if (track == streamTrackIndex || track == vodTrackIndex) {
				if (applyServiceSettings) {
					int bitrate = (int)obs_data_get_int(settings[i], "bitrate");
					obs_service_apply_encoder_settings(main->GetService(), nullptr, settings[i]);

					if (!enforceBitrate)
						obs_data_set_int(settings[i], "bitrate", bitrate);
				}
			}

			if (track == streamTrackIndex)
				obs_encoder_update(streamAudioEnc, settings[i]);
			if (track == vodTrackIndex)
				obs_encoder_update(streamArchiveEnc, settings[i]);
		} else {
			obs_encoder_update(streamTrack[i], settings[i]);
		}
	}
}

void AdvancedOutput::SetupOutputs()
{
	obs_encoder_set_video(videoStreaming, obs_get_video());
	if (videoRecording)
		obs_encoder_set_video(videoRecording, obs_get_video());
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_set_audio(streamTrack[i], obs_get_audio());
		obs_encoder_set_audio(recordTrack[i], obs_get_audio());
	}
	obs_encoder_set_audio(streamAudioEnc, obs_get_audio());
	obs_encoder_set_audio(streamArchiveEnc, obs_get_audio());

	SetupStreaming();

	if (ffmpegOutput)
		SetupFFmpeg();
	else
		SetupRecording();
}

int AdvancedOutput::GetAudioBitrate(size_t i, const char *id) const
{
	static const char *names[] = {
		"Track1Bitrate", "Track2Bitrate", "Track3Bitrate", "Track4Bitrate", "Track5Bitrate", "Track6Bitrate",
	};
	int bitrate = (int)config_get_uint(main->Config(), "AdvOut", names[i]);
	return FindClosestAvailableAudioBitrate(id, bitrate);
}

inline std::optional<size_t> AdvancedOutput::VodTrackMixerIdx(obs_service_t *service)
{
	int streamTrackIndex = config_get_int(main->Config(), "AdvOut", "TrackIndex");
	bool vodTrackEnabled = config_get_bool(main->Config(), "AdvOut", "VodTrackEnabled");
	int vodTrackIndex = config_get_int(main->Config(), "AdvOut", "VodTrackIndex");
	bool enableForCustomServer = config_get_bool(App()->GetUserConfig(), "General", "EnableCustomServerVodTrack");

	const char *id = obs_service_get_id(service);
	if (strcmp(id, "rtmp_custom") == 0) {
		vodTrackEnabled = enableForCustomServer ? vodTrackEnabled : false;
	} else {
		OBSDataAutoRelease settings = obs_service_get_settings(service);
		const char *service = obs_data_get_string(settings, "service");
		if (!ServiceSupportsVodTrack(service))
			vodTrackEnabled = false;
	}

	if (vodTrackEnabled && streamTrackIndex != vodTrackIndex)
		return {vodTrackIndex - 1};
	return std::nullopt;
}

inline void AdvancedOutput::SetupVodTrack(obs_service_t *service)
{
	if (VodTrackMixerIdx(service).has_value())
		obs_output_set_audio_encoder(streamOutput, streamArchiveEnc, 1);
	else
		clear_archive_encoder(streamOutput, ADV_ARCHIVE_NAME);
}

std::shared_future<void> AdvancedOutput::SetupStreaming(obs_service_t *service,
							SetupStreamingContinuation_t continuation)
{
	if (!useStreamEncoder || (!ffmpegOutput && !obs_output_active(fileOutput))) {
		UpdateStreamSettings();
	}

	UpdateAudioSettings();

	if (!Active())
		SetupOutputs();

	Auth *auth = main->GetAuth();
	if (auth)
		auth->OnStreamConfig();

	/* --------------------- */

	const char *type = GetStreamOutputType(service);
	if (!type) {
		continuation(false);
		return StartMultitrackVideoStreamingGuard::MakeReadyFuture();
	}

	const char *audio_encoder_id = config_get_string(main->Config(), "AdvOut", "AudioEncoder");
	int streamTrackIndex = config_get_int(main->Config(), "AdvOut", "TrackIndex") - 1;

	auto handle_multitrack_video_result = [=](std::optional<bool> multitrackVideoResult) {
		if (multitrackVideoResult.has_value())
			return multitrackVideoResult.value();

		/* XXX: this is messy and disgusting and should be refactored */
		if (outputType != type) {
			streamDelayStarting.Disconnect();
			streamStopping.Disconnect();
			startStreaming.Disconnect();
			stopStreaming.Disconnect();

			streamOutput = obs_output_create(type, "adv_stream", nullptr, nullptr);
			if (!streamOutput) {
				blog(LOG_WARNING,
				     "Creation of stream output type '%s' "
				     "failed!",
				     type);
				return false;
			}

			streamDelayStarting.Connect(obs_output_get_signal_handler(streamOutput), "starting",
						    OBSStreamStarting, this);
			streamStopping.Connect(obs_output_get_signal_handler(streamOutput), "stopping",
					       OBSStreamStopping, this);

			startStreaming.Connect(obs_output_get_signal_handler(streamOutput), "start", OBSStartStreaming,
					       this);
			stopStreaming.Connect(obs_output_get_signal_handler(streamOutput), "stop", OBSStopStreaming,
					      this);

			outputType = type;
		}

		AttachStreamEncoders(streamOutput);

		return true;
	};

	return SetupMultitrackVideo(service, audio_encoder_id, static_cast<size_t>(streamTrackIndex),
				    VodTrackMixerIdx(service), [=](std::optional<bool> res) {
					    continuation(handle_multitrack_video_result(res));
				    });
}

/* Attach the streaming encoders to one connection's output: the same video encoder and the same audio mix
 * for every connection of the session, a multitrack service getting its per-mix tracks. Both the handler's
 * own output and the destination outputs are wired through here, so the first connection of a session can
 * never send different media than the ones after it. */
void AdvancedOutput::AttachStreamEncoders(obs_output_t *output)
{
	const int multiTrackAudioMixes = config_get_int(main->Config(), "AdvOut", "StreamMultiTrackAudioMixes");

	obs_output_set_video_encoder(output, videoStreaming);

	if (!allowsMultiTrack()) {
		obs_output_set_audio_encoder(output, streamAudioEnc, 0);
		return;
	}

	for (int i = 0, idx = 0; i < MAX_AUDIO_MIXES; i++) {
		if ((multiTrackAudioMixes & (1 << i)) != 0)
			obs_output_set_audio_encoder(output, streamTrack[i], idx++);
	}
}

/* The profile's connection-level settings for one output: the socket options, dynamic bitrate, and the delay
 * state. Q12/Q15: libobs keeps delay_active and delay_restart_refs per output, so each connection counts down
 * on its own and one destination counting down never drags the others; the delay *values* are still the
 * profile's for all of them until per-connection values exist. Reconnect values are deliberately left to the
 * callers: the primary resolves them through the profile plus the destination override (ApplyDestinationReconnect),
 * a destination through StreamingDestinations::reconnectPolicy() - folding those together belongs to the step
 * that makes the primary destination id authoritative. */
void AdvancedOutput::ApplyConnectionSettings(obs_output_t *output, bool dynBitrate)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "bind_ip", config_get_string(main->Config(), "Output", "BindIP"));
	obs_data_set_string(settings, "ip_family", config_get_string(main->Config(), "Output", "IPFamily"));
#ifdef _WIN32
	obs_data_set_bool(settings, "new_socket_loop_enabled",
			  config_get_bool(main->Config(), "Output", "NewSocketLoopEnable"));
	obs_data_set_bool(settings, "low_latency_mode_enabled",
			  config_get_bool(main->Config(), "Output", "LowLatencyEnable"));
#endif
	obs_data_set_bool(settings, "dyn_bitrate", dynBitrate);
	obs_output_update(output, settings);

	const bool useDelay = config_get_bool(main->Config(), "Output", "DelayEnable");
	const int delaySec = static_cast<int>(config_get_int(main->Config(), "Output", "DelaySec"));
	const bool preserveDelay = config_get_bool(main->Config(), "Output", "DelayPreserve");
	obs_output_set_delay(output, useDelay ? delaySec : 0, preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0);
}

bool AdvancedOutput::StartStreaming(obs_service_t *service)
{
	/* The first connection of a session is a destination like the ones after it, so its service is resolved the
	 * same way the fan-out resolves them - through StreamingDestinations::createService(). For a destination
	 * that still inherits the pre-multi-destination layout that reads the very service.json the Stream settings
	 * page writes, so existing profiles keep streaming to the same place; the difference is that the destination
	 * engine is now the one answering, which is what per-destination control (restart one connection, change one
	 * destination's settings while the others run) needs.
	 * The wrapper lives in the handler for the duration of the session because the runtime entry keeps a
	 * borrowed pointer to it. A destination without a readable service file falls back to the profile service,
	 * which is what this function used before. */
	const StreamDestinationRuntime *primary = main->PrimaryRuntime();
	const bool havePrimaryDestination = primary && !primary->id.empty();
	primaryConnectionService = havePrimaryDestination ? main->Destinations().createService(primary->id)
							   : OBSService();

	obs_service_t *connectionService = service;
	if (havePrimaryDestination && !primaryConnectionService)
		blog(LOG_WARNING, "streaming destination '%s': no readable service file, streaming with the profile service",
		     primary->name.c_str());
	else if (havePrimaryDestination)
		connectionService = primaryConnectionService.Get();

	obs_output_set_service(streamOutput, connectionService);

	/* Record the connection on its destination's runtime entry, the way the extras do it in
	 * StartDestinationOutput(), so the per-destination widgets, the stop/restart/kill actions and the stats
	 * rows all look at the same pair of objects for every connection of the session. */
	if (havePrimaryDestination) {
		if (StreamDestinationRuntime *runtime = main->RuntimeForId(primary->id)) {
			runtime->service = connectionService;
			runtime->output = streamOutput;
		}
	}

	bool reconnect = config_get_bool(main->Config(), "Output", "Reconnect");
	int retryDelay = config_get_int(main->Config(), "Output", "RetryDelay");
	int maxRetries = config_get_int(main->Config(), "Output", "MaxRetries");
	/* Q14: the destination being streamed may carry its own reconnect values; a no-op when it does not. */
	main->ApplyDestinationReconnect(reconnect, retryDelay, maxRetries);
	/* Not const: MultitrackVideoOutput::HandleIncompatibleSettings() takes it by reference and may turn
	 * dynamic bitrate off for a service that cannot have it. */
	bool enableDynBitrate = config_get_bool(main->Config(), "Output", "DynamicBitrate");

	if (multitrackVideo && multitrackVideoActive &&
	    !multitrackVideo->HandleIncompatibleSettings(main, main->Config(), connectionService,
							enableDynBitrate)) {
		multitrackVideoActive = false;
		return false;
	}

	bool is_rtmp = false;
	/* The protocol question is about the connection being started, not about the profile service object: for a
	 * destination that inherits the legacy layout they describe the same service, and for a detached one it is
	 * the destination's own service that the ingest will actually see. */
	obs_service_t *service_obj = connectionService;
	const char *protocol = obs_service_get_protocol(service_obj);
	if (protocol) {
		if (astrcmpi_n(protocol, RTMP_PROTOCOL, strlen(RTMP_PROTOCOL)) == 0)
			is_rtmp = true;
	}

	auto streamOutput = StreamingOutput(); // shadowing is sort of bad, but also convenient

	ApplyConnectionSettings(streamOutput, enableDynBitrate);

	if (!reconnect)
		maxRetries = 0;

	obs_output_set_reconnect_settings(streamOutput, maxRetries, retryDelay);
	if (is_rtmp) {
		SetupVodTrack(connectionService);
	}
	if (obs_output_start(streamOutput)) {
		if (multitrackVideo && multitrackVideoActive)
			multitrackVideo->StartedStreaming();
		StartExtraDestinations();
		return true;
	}

	if (multitrackVideo && multitrackVideoActive)
		multitrackVideoActive = false;

	const char *error = obs_output_get_last_error(streamOutput);
	bool hasLastError = error && *error;
	if (hasLastError)
		lastError = error;
	else
		lastError = string();

	const char *type = obs_output_get_id(streamOutput);
	blog(LOG_WARNING, "Stream output type '%s' failed to start!%s%s", type, hasLastError ? "  Last Error: " : "",
	     hasLastError ? error : "");
	return false;
}

/* Signal handlers of the extra destination outputs. They run on the thread of the output that emitted them,
 * while everything they lead into - the runtime entries, the status bar, the tray, dialogs - belongs to the
 * GUI thread, so the report is marshalled exactly like the primary output's handlers do in
 * BasicOutputHandler.cpp. Only values are captured: an entry can be gone before the queued call runs. */
extern volatile bool streaming_active;

static void ExtraStreamStarted(void *data, calldata_t *)
{
	auto *stream = static_cast<AdvancedOutput::ExtraDestinationStream *>(data);
	OBSBasic *owner = stream->owner;
	std::string id = stream->id;

	/* The session is up as soon as any of its connections is, even when the first destination never got
	 * through: the frontend API and the status bar ask for this flag. */
	os_atomic_set_bool(&streaming_active, true);

	QMetaObject::invokeMethod(owner, [owner, id] { owner->NoteDestinationStarted(id); }, Qt::QueuedConnection);
}

static void ExtraStreamStopped(void *data, calldata_t *params)
{
	auto *stream = static_cast<AdvancedOutput::ExtraDestinationStream *>(data);
	long long code = 0;
	calldata_get_int(params, "code", &code);
	stream->stopPending = false;

	/* The reason the ingest or the socket gave, read while the output object still carries it: a destination
	 * whose failure leaves the session running is reported with it, so the user sees what dropped without a
	 * dialog in the middle of the session. */
	const char *last = stream->output ? obs_output_get_last_error(stream->output) : nullptr;
	std::string error = (last && *last) ? last : "";

	OBSBasic *owner = stream->owner;
	std::string id = stream->id;
	QMetaObject::invokeMethod(
		owner, [owner, id, code, error] { owner->NoteDestinationStopped(id, code, QT_UTF8(error.c_str())); },
		Qt::QueuedConnection);
}

/* The reconnect signals of an extra destination's output: same arrangement - the handler runs on the output's
 * thread and only copies what the GUI thread needs, and the connection lives as long as the entry does. */
static void ExtraStreamReconnect(void *data, calldata_t *params)
{
	auto *stream = static_cast<AdvancedOutput::ExtraDestinationStream *>(data);
	long long seconds = 0;
	calldata_get_int(params, "timeout_sec", &seconds);

	OBSBasic *owner = stream->owner;
	std::string id = stream->id;
	QMetaObject::invokeMethod(
		owner, [owner, id, seconds] { owner->NoteDestinationReconnect(id, seconds); }, Qt::QueuedConnection);
}

static void ExtraStreamReconnected(void *data, calldata_t *)
{
	auto *stream = static_cast<AdvancedOutput::ExtraDestinationStream *>(data);

	OBSBasic *owner = stream->owner;
	std::string id = stream->id;
	QMetaObject::invokeMethod(owner, [owner, id] { owner->NoteDestinationReconnected(id); }, Qt::QueuedConnection);
}

/* Slice 2b, the fan-out: start the destinations after the first one.
 *
 * The first enabled destination is served by the existing streamOutput path (Q42), so it is skipped here.
 * Every remaining one gets its own output object on top of the same encoders: the media is encoded once and
 * uploaded once per connection, which is what the feature is for.
 *
 * A destination that cannot be started is logged and left out; it never undoes the session. The session's UI
 * state follows the whole set of connections since Q13 was flipped: OBSBasic asks the output handler and the
 * runtime entries what is still attempting or uploading, so a destination that failed here has to be marked
 * stopped rather than left in "connecting" - otherwise it would keep a session alive that has nothing in it. */
void AdvancedOutput::StartExtraDestinations()
{
	/* Nothing of the previous session may linger: stop and disconnect before the entries are dropped,
	 * because their signal handlers point at the entries themselves. */
	for (auto &previous : extraStreams) {
		if (previous->output && obs_output_active(previous->output))
			obs_output_force_stop(previous->output);
		previous->startSignal.Disconnect();
		previous->stopSignal.Disconnect();
		previous->reconnectSignal.Disconnect();
		previous->reconnectSuccessSignal.Disconnect();
		previous->output = nullptr;
	}
	extraStreams.clear();

	/* A multitrack-video session is not a plain RTMP output: it owns its own output object, its own ingest
	 * and its own key exchange. Attaching a plain RTMP connection beside it would send the same media to a
	 * second place that never agreed to receive it, so such a session stays with its first destination and
	 * says why. Each destination getting its own multitrack-video session is the Go-Live step of slice 2d. */
	if (multitrackVideo && multitrackVideoActive) {
		const size_t skipped = main->EnabledDestinationCount() - 1;
		if (skipped > 0)
			blog(LOG_INFO,
			     "streaming session is a multitrack-video session: %zu other destination(s) not "
			     "started - one multitrack-video session per destination comes with a later step",
			     skipped);
		return;
	}

	const StreamingDestinations &destinations = main->Destinations();
	size_t started = 0;

	/* The connection this handler serves with its own output is skipped by **id**, not by position: the
	 * runtime model already knows which destination that is (PrimaryRuntime() is the same rule that picked it),
	 * and a position rule goes wrong as soon as the first entry of the list is disabled or the user reorders
	 * the destinations - it then skips a connection that was never started by the handler and leaves the real
	 * primary one started twice. The position rule is only kept as the fallback for a profile with no
	 * destination list at all, where the handler's output is by definition the one and only connection. */
	const StreamDestinationRuntime *primary = main->PrimaryRuntime();
	const bool skipById = primary && !primary->id.empty();
	size_t enabledIndex = 0;

	for (const auto &dest : destinations.destinations()) {
		if (!dest.enabled)
			continue;
		const bool served_by_handler = skipById ? (dest.id == primary->id) : (enabledIndex == 0);
		enabledIndex++;
		if (served_by_handler)
			continue;

		if (StartDestinationOutput(dest))
			started++;
	}

	if (started)
		blog(LOG_INFO, "streaming session: %zu additional destination output(s) started", started);
}

/* Everything one destination needs to become a connection of this session: its service object, an output on
 * the shared encoders, the profile's socket settings, its own reconnect values and its signal handlers. Used
 * by the fan-out and by restarting a single destination (the restart action of Q13), because both need the
 * same thing - one connection for one destination. */
bool AdvancedOutput::StartDestinationOutput(const StreamingDestination &dest)
{
	const StreamingDestinations &destinations = main->Destinations();

	/* A destination that never gets an output running must not stay counted as one of the session's
	 * connections: OBSBasic asks how many runtime entries are still running to decide whether the
	 * session goes on, so a failed one has to be put back to "stopped" here. */
	auto markNotStarted = [&dest, this](const char *why) {
		blog(LOG_WARNING, "streaming destination '%s' not started: %s", dest.name.c_str(), why);
		if (StreamDestinationRuntime *runtime = main->RuntimeForId(dest.id)) {
			runtime->state = StreamDestinationRuntime::State::Stopped;
			runtime->output = nullptr;
			runtime->service = nullptr;
			runtime->encoderGroupId.clear();
		}
	};

	OBSService service = destinations.createService(dest.id);
	if (!service) {
		markNotStarted("its service could not be created");
		return false;
	}

	const char *type = GetStreamOutputType(service);
	if (!type) {
		markNotStarted("no streaming output supports this service");
		return false;
	}

	auto stream = std::make_unique<ExtraDestinationStream>();
	stream->id = dest.id;
	stream->name = dest.name;
	stream->owner = main;
	stream->service = service;
	stream->output = obs_output_create(type, dest.id.c_str(), nullptr, nullptr);
	if (!stream->output) {
		markNotStarted("no output object could be created");
		return false;
	}

	/* Same encoder attachment and connection settings as the primary output: both helpers are shared with
	 * it on purpose, so the first connection of a session and the destinations behind it cannot be
	 * configured differently. */
	AttachStreamEncoders(stream->output);

	obs_output_set_service(stream->output, stream->service);

	ApplyConnectionSettings(stream->output, config_get_bool(main->Config(), "Output", "DynamicBitrate"));

	/* Q14: its own reconnect values. The attempt counter, the backoff and the retry thread are per
		 * output inside libobs already, so one destination can never spend another's attempts. */
	const auto policy = destinations.reconnectPolicy(dest.id);
	obs_output_set_reconnect_settings(stream->output, policy.enabled ? static_cast<int>(policy.maxRetries) : 0,
					  static_cast<int>(policy.retryDelaySec));

	stream->startSignal.Connect(obs_output_get_signal_handler(stream->output), "start", ExtraStreamStarted,
				    stream.get());
	stream->stopSignal.Connect(obs_output_get_signal_handler(stream->output), "stop", ExtraStreamStopped,
				   stream.get());
	stream->reconnectSignal.Connect(obs_output_get_signal_handler(stream->output), "reconnect",
					ExtraStreamReconnect, stream.get());
	stream->reconnectSuccessSignal.Connect(obs_output_get_signal_handler(stream->output), "reconnect_success",
					       ExtraStreamReconnected, stream.get());

	if (!obs_output_start(stream->output)) {
		const char *error = obs_output_get_last_error(stream->output);
		const string reason =
			string("the output refused to start") + ((error && *error) ? (": " + string(error)) : string());
		stream->startSignal.Disconnect();
		stream->stopSignal.Disconnect();
		stream->reconnectSignal.Disconnect();
		stream->reconnectSuccessSignal.Disconnect();
		markNotStarted(reason.c_str());
		return false;
	}

	stream->stopPending = true;

	if (StreamDestinationRuntime *runtime = main->RuntimeForId(dest.id)) {
		runtime->output = stream->output;
		runtime->service = stream->service;
		/* The output starts asynchronously, like the primary one: the connection attempt is what
		 * decides between "live" and another "stopped". */
		runtime->state = StreamDestinationRuntime::State::Connecting;
	}

	extraStreams.push_back(std::move(stream));
	return true;
}

/* Restart one destination of a session that is still running (the restart action of Q13). */
bool AdvancedOutput::StartDestination(std::string_view id)
{
	/* The first enabled destination is served by this handler's own output (Q42), so restarting it means
	 * starting that output again: its service, encoders and settings are still what the session was built
	 * with, and the output object survives because the session did not end (ResetStreaming() only runs
	 * when it does). */
	const StreamDestinationRuntime *primary = main->PrimaryRuntime();
	if (primary && primary->id == id) {
		OBSOutputAutoRelease output = StreamingOutput();
		if (!output) {
			blog(LOG_WARNING,
			     "streaming destination '%s' cannot be restarted: its output went away "
			     "with the session",
			     primary->name.c_str());
			return false;
		}
		if (obs_output_active(output)) {
			blog(LOG_INFO, "streaming destination '%s' is already connecting or uploading",
			     primary->name.c_str());
			return true;
		}
		if (StreamDestinationRuntime *runtime = main->RuntimeForId(id))
			runtime->state = StreamDestinationRuntime::State::Connecting;
		return obs_output_start(output);
	}

	for (const auto &stream : extraStreams) {
		if (stream->id == id && stream->stopPending) {
			blog(LOG_INFO, "streaming destination '%s' is already connecting or uploading",
			     stream->name.c_str());
			return true;
		}
	}

	const StreamingDestination *dest = main->Destinations().find(id);
	if (!dest) {
		blog(LOG_WARNING, "cannot restart streaming destination: this profile does not list '%.*s'",
		     (int)id.size(), id.data());
		return false;
	}
	if (!dest->enabled) {
		blog(LOG_INFO, "streaming destination '%s' is not enabled, so it does not get a connection back",
		     dest->name.c_str());
		return false;
	}

	blog(LOG_INFO, "restarting the connection of streaming destination '%s'", dest->name.c_str());
	return StartDestinationOutput(*dest);
}

void AdvancedOutput::StopDestination(std::string_view id, bool force)
{
	/* One connection of the session, leaving the others uploading (Q13). */
	for (auto &stream : extraStreams) {
		if (stream->id != id)
			continue;
		if (!stream->output || !stream->stopPending)
			return;
		if (force)
			obs_output_force_stop(stream->output);
		else
			obs_output_stop(stream->output);
		return;
	}

	const StreamDestinationRuntime *primary = main->PrimaryRuntime();
	if (primary && primary->id == id) {
		OBSOutputAutoRelease output = StreamingOutput();
		if (!output)
			return;
		if (force)
			obs_output_force_stop(output);
		else
			obs_output_stop(output);
		return;
	}

	blog(LOG_WARNING, "streaming destination '%.*s' has no connection of this session to stop", (int)id.size(),
	     id.data());
}

size_t AdvancedOutput::ActiveExtraDestinations() const
{
	size_t count = 0;
	for (const auto &stream : extraStreams) {
		if (stream->stopPending)
			count++;
	}
	return count;
}

void AdvancedOutput::StopExtraDestinations(bool force)
{
	for (auto &stream : extraStreams) {
		if (!stream->output || !stream->stopPending)
			continue;
		if (force)
			obs_output_force_stop(stream->output);
		else
			obs_output_stop(stream->output);
	}
}

bool AdvancedOutput::StartRecording()
{
	const char *path;
	const char *recFormat;
	const char *filenameFormat;
	bool noSpace = false;
	bool overwriteIfExists = false;
	bool splitFile;
	const char *splitFileType;
	int splitFileTime;
	int splitFileSize;

	if (!useStreamEncoder) {
		if (!ffmpegOutput) {
			UpdateRecordingSettings();
		}
	} else if (!obs_output_active(StreamingOutput())) {
		UpdateStreamSettings();
	}

	UpdateAudioSettings();

	if (!Active())
		SetupOutputs();

	if (!ffmpegOutput || ffmpegRecording) {
		path = config_get_string(main->Config(), "AdvOut", ffmpegRecording ? "FFFilePath" : "RecFilePath");
		recFormat = config_get_string(main->Config(), "AdvOut", ffmpegRecording ? "FFExtension" : "RecFormat2");
		filenameFormat = config_get_string(main->Config(), "Output", "FilenameFormatting");
		overwriteIfExists = config_get_bool(main->Config(), "Output", "OverwriteIfExists");
		noSpace = config_get_bool(main->Config(), "AdvOut",
					  ffmpegRecording ? "FFFileNameWithoutSpace" : "RecFileNameWithoutSpace");
		splitFile = config_get_bool(main->Config(), "AdvOut", "RecSplitFile");

		string strPath = GetRecordingFilename(path, recFormat, noSpace, overwriteIfExists, filenameFormat,
						      ffmpegRecording);

		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, ffmpegRecording ? "url" : "path", strPath.c_str());

		if (splitFile) {
			splitFileType = config_get_string(main->Config(), "AdvOut", "RecSplitFileType");
			splitFileTime = (astrcmpi(splitFileType, "Time") == 0)
						? config_get_int(main->Config(), "AdvOut", "RecSplitFileTime")
						: 0;
			splitFileSize = (astrcmpi(splitFileType, "Size") == 0)
						? config_get_int(main->Config(), "AdvOut", "RecSplitFileSize")
						: 0;
			string ext = GetFormatExt(recFormat);
			obs_data_set_string(settings, "directory", path);
			obs_data_set_string(settings, "format", filenameFormat);
			obs_data_set_string(settings, "extension", ext.c_str());
			obs_data_set_bool(settings, "allow_spaces", !noSpace);
			obs_data_set_bool(settings, "allow_overwrite", overwriteIfExists);
			obs_data_set_bool(settings, "split_file", true);
			obs_data_set_int(settings, "max_time_sec", splitFileTime * 60);
			obs_data_set_int(settings, "max_size_mb", splitFileSize);
		}

		obs_output_update(fileOutput, settings);
	}

	if (!obs_output_start(fileOutput)) {
		QString error_reason;
		const char *error = obs_output_get_last_error(fileOutput);
		if (error)
			error_reason = QT_UTF8(error);
		else
			error_reason = QTStr("Output.StartFailedGeneric");
		QMessageBox::critical(main, QTStr("Output.StartRecordingFailed"), error_reason);
		return false;
	}

	return true;
}

bool AdvancedOutput::StartReplayBuffer()
{
	const char *path;
	const char *recFormat;
	const char *filenameFormat;
	bool noSpace = false;
	bool overwriteIfExists = false;
	const char *rbPrefix;
	const char *rbSuffix;
	int rbTime;
	int rbSize;

	if (!useStreamEncoder) {
		if (!ffmpegOutput)
			UpdateRecordingSettings();
	} else if (!obs_output_active(StreamingOutput())) {
		UpdateStreamSettings();
	}

	UpdateAudioSettings();

	if (!Active())
		SetupOutputs();

	if (!ffmpegOutput || ffmpegRecording) {
		path = config_get_string(main->Config(), "AdvOut", ffmpegRecording ? "FFFilePath" : "RecFilePath");
		recFormat = config_get_string(main->Config(), "AdvOut", ffmpegRecording ? "FFExtension" : "RecFormat2");
		filenameFormat = config_get_string(main->Config(), "Output", "FilenameFormatting");
		overwriteIfExists = config_get_bool(main->Config(), "Output", "OverwriteIfExists");
		noSpace = config_get_bool(main->Config(), "AdvOut",
					  ffmpegRecording ? "FFFileNameWithoutSpace" : "RecFileNameWithoutSpace");
		rbPrefix = config_get_string(main->Config(), "AdvOut", "RecRBPrefix");
		rbSuffix = config_get_string(main->Config(), "AdvOut", "RecRBSuffix");
		rbTime = config_get_int(main->Config(), "AdvOut", "RecRBTime");
		rbSize = config_get_int(main->Config(), "AdvOut", "RecRBSize");

		string f = GetFormatString(filenameFormat, rbPrefix, rbSuffix);
		string ext = GetFormatExt(recFormat);

		OBSDataAutoRelease settings = obs_data_create();

		obs_data_set_string(settings, "directory", path);
		obs_data_set_string(settings, "format", f.c_str());
		obs_data_set_string(settings, "extension", ext.c_str());
		obs_data_set_bool(settings, "allow_spaces", !noSpace);
		obs_data_set_int(settings, "max_time_sec", rbTime);
		obs_data_set_int(settings, "max_size_mb", usesBitrate ? 0 : rbSize);

		obs_output_update(replayBuffer, settings);
	}

	if (!obs_output_start(replayBuffer)) {
		QString error_reason;
		const char *error = obs_output_get_last_error(replayBuffer);
		if (error)
			error_reason = QT_UTF8(error);
		else
			error_reason = QTStr("Output.StartFailedGeneric");
		QMessageBox::critical(main, QTStr("Output.StartReplayFailed"), error_reason);
		return false;
	}

	return true;
}

void AdvancedOutput::StopStreaming(bool force)
{
	/* The extras go down with the session: while the session state is still the primary destination's,
	 * a session stop means every connection stops (Q42). */
	StopExtraDestinations(force);

	auto output = StreamingOutput();
	if (force && output)
		obs_output_force_stop(output);
	else if (multitrackVideo && multitrackVideoActive)
		multitrackVideo->StopStreaming();
	else
		obs_output_stop(output);
}

void AdvancedOutput::StopRecording(bool force)
{
	if (force)
		obs_output_force_stop(fileOutput);
	else
		obs_output_stop(fileOutput);
}

void AdvancedOutput::StopReplayBuffer(bool force)
{
	if (force)
		obs_output_force_stop(replayBuffer);
	else
		obs_output_stop(replayBuffer);
}

bool AdvancedOutput::StreamingActive() const
{
	if (obs_output_active(StreamingOutput()))
		return true;

	/* Q13: the session is every connection it owns, not only the first one. stopPending covers the extra
	 * outputs from the moment they are started until their stop signal arrives, which includes the
	 * handshake - an output that is still connecting is not "active" for libobs but is very much part of
	 * the session. */
	for (const auto &stream : extraStreams) {
		if (stream->stopPending)
			return true;
	}
	return false;
}

bool AdvancedOutput::RecordingActive() const
{
	return obs_output_active(fileOutput);
}

bool AdvancedOutput::ReplayBufferActive() const
{
	return obs_output_active(replayBuffer);
}
