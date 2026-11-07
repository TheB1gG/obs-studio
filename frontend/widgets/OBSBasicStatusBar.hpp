#pragma once

#include "StatusBarWidget.hpp"

#include <utility/EncoderStats.hpp>

#include <obs.hpp>

#include <QPointer>
#include <QStatusBar>

#include <map>
#include <memory>
#include <string>
#include <vector>

class QTimer;

class OBSBasicStatusBar : public QStatusBar {
	Q_OBJECT

private:
	StatusBarWidget *statusWidget = nullptr;

	OBSWeakOutputAutoRelease streamOutput;
	std::vector<OBSSignal> streamSigs;
	OBSWeakOutputAutoRelease recordOutput;
	bool active = false;
	bool overloadedNotify = true;
	bool streamPauseIconToggle = false;
	bool disconnected = false;
	bool firstCongestionUpdate = false;

	/* One entry per connection of the streaming session. Since Q13 a session is the set of its
	 * destinations, so the status bar has to say how many of them are up and which one is reconnecting -
	 * the numbers, the kbps and the congestion icon all belong to a connection, not to "the stream". */
	struct Connection {
		std::string id;
		std::string name;
		OBSWeakOutputAutoRelease output;
		/* The first destination's output is watched here; the extra destinations report through the
		 * output handler, which owns their connections and disconnects them before an entry dies. */
		std::vector<OBSSignal> sigs;
		int retries = 0;
		int reconnectTimeout = 0;
	};

	/* Map, not vector: the entries hold the signal connections of their output, and a map leaves a node (and
	 * the address of its handler argument) alone while other connections come and go. The list the session
	 * passes in carries the order; the map keys it by destination id. */
	std::map<std::string, Connection> connections;
	/* The id the session passed first: the destination served by the output handler (Q42), which is the one
	 * this widget watches signals for directly. */
	std::string primaryConnectionId;

	Connection *findConnection(const std::string &id);
	void ConnectionMessage();
	/* Strong references to every connection of the streaming session, the first one being the output the
	 * handler serves for the first destination (Q42). The aggregate numbers - bandwidth, dropped frames,
	 * congestion, encoder counters - are computed over this set, because the session is what is uploading. */
	std::vector<OBSOutput> sessionOutputs();

	std::vector<float> congestionArray;

	int retries = 0;
	int totalStreamSeconds = 0;
	int totalRecordSeconds = 0;

	int reconnectTimeout = 0;

	int delaySecTotal = 0;
	int delaySecStarting = 0;
	int delaySecStopping = 0;

	uint64_t startSkippedFrameCount = 0;
	uint64_t startTotalFrameCount = 0;
	uint64_t lastSkippedFrameCount = 0;

	int seconds = 0;
	uint64_t lastBytesSent = 0;
	uint64_t lastBytesSentTime = 0;

	QPixmap excellentPixmap;
	QPixmap goodPixmap;
	QPixmap mediocrePixmap;
	QPixmap badPixmap;
	QPixmap disconnectedPixmap;
	QPixmap inactivePixmap;

	QPixmap recordingActivePixmap;
	QPixmap recordingPausePixmap;
	QPixmap recordingPauseInactivePixmap;
	QPixmap recordingInactivePixmap;
	QPixmap streamingActivePixmap;
	QPixmap streamingInactivePixmap;

	float lastCongestion = 0.0f;

	QPointer<QTimer> refreshTimer;
	QPointer<QTimer> messageTimer;

	obs_output_t *GetOutput();

	OBS::EncoderFrameCounters activeEncoderFrameCounters();

	void Activate();
	void Deactivate();

	void UpdateDelayMsg();
	void UpdateBandwidth();
	void UpdateStreamTime();
	void UpdateRecordTime();
	void UpdateRecordTimeLabel();
	void UpdateDroppedFrames();

	static void OBSOutputReconnect(void *data, calldata_t *params);
	static void OBSOutputReconnectSuccess(void *data, calldata_t *params);

	void ReconnectPrimary(int seconds);
	void ReconnectPrimarySuccess();
	/* The per-connection state of the reconnect: an attempt counter and a countdown for each output, because
	 * libobs keeps them per output. An id that the bar has not been told about falls back to the first
	 * connection, which is the one it watches by itself. */
	void MarkReconnecting(const std::string &id, int seconds);
	void MarkReconnected(const std::string &id);

public slots:
	void UpdateCPUUsage();

	void clearMessage();
	void showMessage(const QString &message, int timeout = 0);

private slots:
	void UpdateStatusBar();
	void UpdateCurrentFPS();
	void UpdateIcons();

public:
	OBSBasicStatusBar(QWidget *parent);

	/* A connection of the streaming session, as the main window reports it: the output pointer is only
	 * borrowed for the duration of the call, the rest is copied. */
	struct StreamConnectionInfo {
		std::string id;
		std::string name;
		obs_output_t *output = nullptr;
		bool reconnecting = false;
	};
	/* Reconcile the status bar with the connections of the session: entries that appear get their reconnect
	 * signals watched, entries that disappear stop being reported. Called on every session transition, so it
	 * has to be idempotent - it never resets a countdown that is already running. */
	void SetStreamConnections(const std::vector<StreamConnectionInfo> &list);
	/* A destination that is not the first one reports its reconnect through the output handler, because the
	 * handler owns that connection; these calls arrive on the GUI thread. */
	void NoteStreamReconnect(const std::string &id, int seconds);
	void NoteStreamReconnectSuccess(const std::string &id);

	void StreamDelayStarting(int sec);
	void StreamDelayStopping(int sec);
	void StreamStarted(obs_output_t *output);
	void StreamStopped();
	void RecordingStarted(obs_output_t *output);
	void RecordingStopped();
	void RecordingPaused();
	void RecordingUnpaused();

	void ReconnectClear();
};
