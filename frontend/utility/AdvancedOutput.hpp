#pragma once

#include "BasicOutputHandler.hpp"

#include <utility/StreamingDestinations.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct AdvancedOutput : BasicOutputHandler {
	OBSEncoder streamAudioEnc;
	OBSEncoder streamArchiveEnc;
	OBSEncoder streamTrack[MAX_AUDIO_MIXES];
	OBSEncoder recordTrack[MAX_AUDIO_MIXES];
	OBSEncoder videoStreaming;
	OBSEncoder videoRecording;

	bool ffmpegOutput;
	bool ffmpegRecording;
	bool useStreamEncoder;
	bool useStreamAudioEncoder;
	bool usesBitrate = false;

	AdvancedOutput(OBSBasic *main_);

	inline void UpdateStreamSettings();
	inline void UpdateRecordingSettings();
	inline void UpdateAudioSettings();
	virtual void Update() override;

	inline std::optional<size_t> VodTrackMixerIdx(obs_service_t *service);
	inline void SetupVodTrack(obs_service_t *service);

	inline void SetupStreaming();
	inline void SetupRecording();
	inline void SetupFFmpeg();
	void SetupOutputs() override;
	int GetAudioBitrate(size_t i, const char *id) const;

	virtual std::shared_future<void> SetupStreaming(obs_service_t *service,
							SetupStreamingContinuation_t continuation) override;
	virtual bool StartStreaming(obs_service_t *service) override;
	virtual bool StartRecording() override;
	virtual bool StartReplayBuffer() override;
	virtual void StopStreaming(bool force) override;
	virtual void StopRecording(bool force) override;
	virtual void StopReplayBuffer(bool force) override;
	virtual bool StreamingActive() const override;
	virtual bool RecordingActive() const override;
	virtual bool ReplayBufferActive() const override;
	bool allowsMultiTrack();

	/* Slice 2b: the destinations after the first one. The first enabled destination keeps using the
	 * existing streamOutput, which leaves the recording, delay and multitrack-video code untouched while
	 * the loop is built; every other destination gets its own output object on top of the same encoders.
	 * Sharing an encoder costs upload, not encoding, so this is about the connection, not the media -
	 * unless a destination asked for a picture rate of its own, in which case it gets an encoder of its
	 * own too and the destinations without an override keep sharing the profile's one. */
	struct ExtraDestinationStream {
		std::string id;
		std::string name;
		OBSBasic *owner = nullptr;
		OBSService service;
		OBSOutput output; /* released when the entry goes away */
		/* The video encoder this connection uploads, when the destination asked for a picture rate of its
		 * own (Q25's other half: one encoder per configuration, not one per connection). Empty while the
		 * destination is fed by the profile's shared stream encoder, which is the default. It lives as
		 * long as the entry: releasing it is what stops the encoder being fed frames. */
		OBSEncoder videoEncoder;
		OBSSignal stopSignal;
		OBSSignal startSignal;
		OBSSignal reconnectSignal;
		OBSSignal reconnectSuccessSignal;
		/* Set once the output has been started and cleared by its stop signal: an output that is still
		 * connecting has to be stopped just as much as one that is already uploading. Written by the
		 * output's own thread, read by the GUI thread - same arrangement as the handler flags in
		 * BasicOutputHandler. */
		bool stopPending = false;
	};

	std::vector<std::unique_ptr<ExtraDestinationStream>> extraStreams;
	/* The service of the first connection of the session, resolved through the destination engine and kept
	 * here while the session runs: the runtime entry of that destination holds a borrowed pointer to it, just
	 * like the extra destinations' own wrappers do. */
	OBSService primaryConnectionService;
	/* The video encoder of that first connection when its destination overrides the profile's bitrate. The
	 * shared encoder is deliberately left at the profile's number, because every destination without an
	 * override is fed by it; so the primary connection needs an encoder of its own instead of a retuned
	 * shared one. Recreated with the session, like the output it is attached to. */
	OBSEncoder primaryVideoEncoder;

	void StartExtraDestinations();
	void StopExtraDestinations(bool force) override;
	size_t ActiveExtraDestinations() const override;
	/* One connection of the session by destination id: stop it alone, or bring it back while the rest of
	 * the session keeps running. */
	void StopDestination(std::string_view id, bool force) override;
	bool StartDestination(std::string_view id) override;

private:
	/* Build one destination's output and start it: the fan-out and the restart of a single destination need
	 * exactly the same thing. */
	bool StartDestinationOutput(const StreamingDestination &dest);
	/* The two halves of "what every connection of the session gets", shared with the handler's own primary
	 * output so the first connection cannot be configured differently from the ones behind it. The video
	 * encoder is the one this connection uploads: the shared stream encoder when the caller passes none. */
	void AttachStreamEncoders(obs_output_t *output, obs_encoder_t *videoEncoder = nullptr);
	void ApplyConnectionSettings(obs_output_t *output, bool dynBitrate);
	/* A destination that asked for a picture rate of its own pays for an encoder of its own: the profile's
	 * stream-encoder settings (same codec, defaults, scaling and frame-rate divisor), the destination's
	 * bitrate, and the limits of the service *that destination* uploads to. Returns empty when the encoder
	 * could not be built, after saying why. */
	OBSEncoder CreateDestinationVideoEncoder(const std::string &id, const std::string &display_name, long long kbps,
						 obs_service_t *service);
};
