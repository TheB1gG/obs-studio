#pragma once

#include "BasicOutputHandler.hpp"

#include <memory>
#include <string>
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
	 * Sharing an encoder costs upload, not encoding, so this is about the connection, not the media. */
	struct ExtraDestinationStream {
		std::string id;
		std::string name;
		OBSBasic *owner = nullptr;
		OBSService service;
		OBSOutput output; /* released when the entry goes away */
		OBSSignal stopSignal;
		OBSSignal startSignal;
		/* Set once the output has been started and cleared by its stop signal: an output that is still
		 * connecting has to be stopped just as much as one that is already uploading. Written by the
		 * output's own thread, read by the GUI thread - same arrangement as the handler flags in
		 * BasicOutputHandler. */
		bool stopPending = false;
	};

	std::vector<std::unique_ptr<ExtraDestinationStream>> extraStreams;

	void StartExtraDestinations();
	void StopExtraDestinations(bool force) override;
	size_t ActiveExtraDestinations() const override;
};
