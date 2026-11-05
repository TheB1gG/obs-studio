/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

#include "OBSBasic.hpp"
#include "OBSBasicControls.hpp"
#include "OBSBasicStatusBar.hpp"

#include <components/UIValidation.hpp>
#ifdef YOUTUBE_ENABLED
#include <docks/YouTubeAppDock.hpp>
#include <utility/YoutubeApiWrappers.hpp>
#endif

#include <qt-wrappers.hpp>

#include <QDateTime>

#define STREAMING_START "==== Streaming Start ==============================================="
#define STREAMING_STOP "==== Streaming Stop ================================================"

/* Q13 (ii): two or more connections dropping inside this window are reported as one correlated failure - the
 * likely cause is then the uplink rather than the destinations, and one line per destination would hide it. */
static constexpr qint64 failure_collapse_window_ms = 5000;

void OBSBasic::DisplayStreamStartError()
{
	QString message = !outputHandler->lastError.empty() ? QTStr(outputHandler->lastError.c_str())
							    : QTStr("Output.StartFailedGeneric");

	emit StreamingStopped();

	if (sysTrayStream) {
		sysTrayStream->setText(QTStr("Basic.Main.StartStreaming"));
		sysTrayStream->setEnabled(true);
	}

	QMessageBox::critical(this, QTStr("Output.StartStreamFailed"), message);
}

/* Q14: a destination may use its own reconnect values instead of the profile-wide ones. The session runs a
 * single stream today, and that stream belongs to the first enabled destination (Q42), so this is the entry
 * consulted here; the multi-destination loop will call the same resolver once per destination. Without an
 * override the values are left exactly as they were read from [Output], which is what keeps the behaviour of
 * a profile with the flag off identical. */
void OBSBasic::ApplyDestinationReconnect(bool &enabled, int &retryDelaySec, int &maxRetries)
{
	StreamDestinationRuntime *runtime = CurrentRuntime();
	if (!runtime || runtime->id.empty())
		return;

	const StreamingDestinations::ReconnectPolicy policy = streamingDestinations.reconnectPolicy(runtime->id);
	if (!policy.overridden)
		return;

	enabled = policy.enabled;
	retryDelaySec = static_cast<int>(policy.retryDelaySec);
	maxRetries = static_cast<int>(policy.maxRetries);
	blog(LOG_INFO, "streaming destination '%s' overrides reconnect: %s, retry every %d s, %d attempts",
	     runtime->name.c_str(), enabled ? "enabled" : "disabled", retryDelaySec, maxRetries);
}

/* Slice 2b: the outputs of the extra destinations report through these. They keep the runtime model and the
 * log right about how many connections are up; the session-level decision is made by StreamingStop, which the
 * stop report routes into so that the session comes down with its last connection instead of its first (Q13). */
void OBSBasic::NoteDestinationStarted(const std::string &id)
{
	StreamDestinationRuntime *runtime = RuntimeForId(id);
	if (!runtime)
		return;

	runtime->state = StreamDestinationRuntime::State::Live;
	runtime->reconnectAttempt = 0;
	blog(LOG_INFO, "streaming destination '%s' is live (%zu of %zu enabled destinations up)", runtime->name.c_str(),
	     LiveDestinationCount(), EnabledDestinationCount());
	PushStreamConnections();
}

void OBSBasic::NoteDestinationStopped(const std::string &id, long long code, const QString &error)
{
	StreamDestinationRuntime *runtime = RuntimeForId(id);
	if (!runtime)
		return;

	const char *previousState = runtime->stateName();
	/* A connection the user killed through the skull button is not a failure, and its marker belongs to
	 * this destination alone: it must not turn another destination's real drop into a silent stop. */
	const bool killed = runtime->killRequested;
	runtime->killRequested = false;
	runtime->state = StreamDestinationRuntime::State::Stopped;
	runtime->output = nullptr;
	runtime->service = nullptr;
	runtime->encoderGroupId.clear();
	runtime->reconnectAttempt = 0;
	blog(LOG_INFO, "streaming destination '%s' stopped after %s, code %lld%s (%zu of %zu enabled destinations up)",
	     runtime->name.c_str(), previousState, code, killed ? " - killed ungracefully" : "", LiveDestinationCount(),
	     EnabledDestinationCount());
	PushStreamConnections();

	/* Only a stop that leaves nothing of the session up has to bring the session down. While another
	 * destination is still attempting or uploading this was a per-destination failure, and StreamingStop
	 * must not run: it reports the destination of its own signal through CurrentRuntime(), which would
	 * otherwise pick up that other destination's entry. When this is the last connection every entry is
	 * already stopped, which is exactly the case its own report skips. */
	if (outputHandler && (outputHandler->StreamingActive() || RunningDestinationCount() > 0)) {
		blog(LOG_INFO,
		     "streaming session continues after destination '%s' went down: %zu of %zu enabled destinations still attempting or uploading",
		     runtime->name.c_str(), RunningDestinationCount(), EnabledDestinationCount());
		ReportDestinationFailure(*runtime, code, killed, error);
		return;
	}

	StreamingStop(killed ? OBS_OUTPUT_SUCCESS : static_cast<int>(code), QString());
}

/* The status bar shows the session, so every transition pushes the current connection list and the reconnect
 * reports of the extra destinations are routed through here (the handler owns their signals). */
void OBSBasic::PushStreamConnections()
{
	if (!ui || !ui->statusbar)
		return;

	std::vector<OBSBasicStatusBar::StreamConnectionInfo> list;
	for (const auto &entry : StreamSessionSnapshot())
		list.push_back({entry.id, entry.name, entry.output, entry.reconnecting});

	ui->statusbar->SetStreamConnections(list);
}

void OBSBasic::NoteDestinationReconnect(const std::string &id, long long seconds)
{
	if (StreamDestinationRuntime *runtime = RuntimeForId(id)) {
		runtime->state = StreamDestinationRuntime::State::Reconnecting;
		runtime->reconnectAttempt++;
		blog(LOG_INFO, "streaming destination '%s' is reconnecting (attempt %d, next try in %lld s)",
		     runtime->name.c_str(), runtime->reconnectAttempt, seconds);
	}

	if (ui && ui->statusbar)
		ui->statusbar->NoteStreamReconnect(id, static_cast<int>(seconds));
}

void OBSBasic::NoteDestinationReconnected(const std::string &id)
{
	if (StreamDestinationRuntime *runtime = RuntimeForId(id)) {
		if (runtime->state == StreamDestinationRuntime::State::Reconnecting)
			runtime->state = StreamDestinationRuntime::State::Live;
		runtime->reconnectAttempt = 0;
		blog(LOG_INFO, "streaming destination '%s' reconnected", runtime->name.c_str());
	}

	if (ui && ui->statusbar)
		ui->statusbar->NoteStreamReconnectSuccess(id);
}

/* Q13 (ii): a destination that fails while the rest of the session keeps uploading has to be visible without a
 * dialog. A modal in the middle of a running session would block the work the other connections are still doing,
 * and what the user needs here is to see which destination is gone - not to confirm anything. The restart action
 * itself lives in the stats window's section for that destination. */
void OBSBasic::ReportDestinationFailure(const StreamDestinationRuntime &runtime, long long code, bool killed,
					const QString &error, bool shared_encoder_failure)
{
	/* Nothing to report for a connection the user stopped or killed themselves, nor while the session as a
	 * whole is coming down - that cascade ends every connection and is reported by the session's own stop
	 * path, which is the case a dialog is right for. */
	if (killed || streamingStopping || code == OBS_OUTPUT_SUCCESS)
		return;

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (lastDestinationFailureMs != 0 && now - lastDestinationFailureMs <= failure_collapse_window_ms)
		++destinationFailureRun;
	else
		destinationFailureRun = 1;
	lastDestinationFailureMs = now;

	QString text;
	const QString name = QT_UTF8(runtime.name.c_str());

	if (shared_encoder_failure) {
		/* An encoder or HDR problem is not this destination's: the connections of a session share the
		 * encoder group, so saying which destination reported it would blame the wrong one. */
		text = QTStr("Basic.Streaming.EncoderFailureShared");
		if (!error.isEmpty())
			text += QStringLiteral(" (") + error + QStringLiteral(")");
	} else if (destinationFailureRun >= 2) {
		/* Several connections failing within a few seconds have one likely cause between them - the
		 * uplink - and a stack of per-destination lines would hide that. One line, counting them. */
		text = QTStr("Basic.Streaming.DestinationsDropped").arg(QString::number(destinationFailureRun));
	} else if (!error.isEmpty()) {
		text = QTStr("Basic.Streaming.DestinationDroppedWithReason").arg(name, error);
	} else {
		text = QTStr("Basic.Streaming.DestinationDropped").arg(name);
	}

	blog(LOG_WARNING, "streaming session: %s", QT_TO_UTF8(text));

	if (ui && ui->statusbar)
		ui->statusbar->showMessage(text, 8000);

	/* The window may be in the tray while a session runs; the status bar line would go unseen. */
	if (!isVisible())
		SysTrayNotify(text, QSystemTrayIcon::Warning);
}

void OBSBasic::StartStreaming()
{
	if (outputHandler->StreamingActive())
		return;

	/* A new session reports its failures on their own: correlated drops of the previous one must not collapse
	 * into this session's first message. */
	destinationFailureRun = 0;
	lastDestinationFailureMs = 0;
	if (disableOutputsRef)
		return;

	if (auth && auth->broadcastFlow()) {
		if (!broadcastActive && !broadcastReady) {
			QMessageBox no_broadcast(this);
			no_broadcast.setText(QTStr("Output.NoBroadcast.Text"));
			QPushButton *SetupBroadcast =
				no_broadcast.addButton(QTStr("Basic.Main.SetupBroadcast"), QMessageBox::YesRole);
			no_broadcast.setDefaultButton(SetupBroadcast);
			no_broadcast.addButton(QTStr("Close"), QMessageBox::NoRole);
			no_broadcast.setIcon(QMessageBox::Information);
			no_broadcast.setWindowTitle(QTStr("Output.NoBroadcast.Title"));
			no_broadcast.exec();

			if (no_broadcast.clickedButton() == SetupBroadcast)
				QMetaObject::invokeMethod(this, "SetupBroadcast");
			return;
		}
	}

	emit StreamingPreparing();

	if (sysTrayStream) {
		sysTrayStream->setEnabled(false);
		sysTrayStream->setText("Basic.Main.PreparingStream");
	}

	if (StreamDestinationRuntime *runtime = CurrentRuntime()) {
		runtime->state = StreamDestinationRuntime::State::Preparing;
		runtime->reconnectAttempt = 0;
	}

	auto finish_stream_setup = [&](bool setupStreamingResult) {
		if (!setupStreamingResult) {
			DisplayStreamStartError();
			return;
		}

		OnEvent(OBS_FRONTEND_EVENT_STREAMING_STARTING);

		SaveProject();

		emit StreamingStarting(autoStartBroadcast);

		if (sysTrayStream)
			sysTrayStream->setText("Basic.Main.Connecting");

		if (!outputHandler->StartStreaming(service)) {
			DisplayStreamStartError();
			return;
		}

		if (StreamDestinationRuntime *runtime = CurrentRuntime()) {
			runtime->state = StreamDestinationRuntime::State::Connecting;
			/* The first destination is served by the handler's own output (Q42); its runtime entry keeps
			 * a reference to it so that the per-destination widgets - and the per-destination stop,
			 * restart and kill - can reach the connection instead of only the extra ones. */
			runtime->output = outputHandler->StreamingOutput();
			runtime->service = service;
			PushStreamConnections();
		}

		if (autoStartBroadcast) {
			emit BroadcastStreamStarted(autoStopBroadcast);
			broadcastActive = true;
		}

		bool recordWhenStreaming =
			config_get_bool(App()->GetUserConfig(), "BasicWindow", "RecordWhenStreaming");
		if (recordWhenStreaming)
			StartRecording();

		bool replayBufferWhileStreaming =
			config_get_bool(App()->GetUserConfig(), "BasicWindow", "ReplayBufferWhileStreaming");
		if (replayBufferWhileStreaming)
			StartReplayBuffer();

#ifdef YOUTUBE_ENABLED
		if (!autoStartBroadcast)
			OBSBasic::ShowYouTubeAutoStartWarning();
#endif
	};

	/* The first number to look at when a stream is congested. Logged per destination because where the
	 * value comes from depends on the profile mode and on whether the destination has its own video
	 * tracks, and that is not visible from the number alone. */
	for (const auto &destination : streamingDestinations.destinations()) {
		if (!destination.enabled)
			continue;
		const auto bandwidth = streamingDestinations.bitrateFor(destination.id);
		blog(LOG_INFO, "streaming destination '%s' will upload %lld kbps (video %lld, audio %lld)",
		     destination.name.c_str(), bandwidth.videoKbps + bandwidth.audioKbps, bandwidth.videoKbps,
		     bandwidth.audioKbps);
	}

	setupStreamingGuard = outputHandler->SetupStreaming(service, finish_stream_setup);
}

void OBSBasic::StopStreaming()
{
	SaveProject();

	if (outputHandler->StreamingActive())
		outputHandler->StopStreaming(streamingStopping);

	// special case: force reset broadcast state if
	// no autostart and no autostop selected
	if (!autoStartBroadcast && !broadcastActive) {
		broadcastActive = false;
		autoStartBroadcast = true;
		autoStopBroadcast = true;
		broadcastReady = false;
	}

	if (autoStopBroadcast) {
		broadcastActive = false;
		broadcastReady = false;
	}

	emit BroadcastStreamReady(broadcastReady);

	OnDeactivate();

	bool recordWhenStreaming = config_get_bool(App()->GetUserConfig(), "BasicWindow", "RecordWhenStreaming");
	bool keepRecordingWhenStreamStops =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "KeepRecordingWhenStreamStops");
	if (recordWhenStreaming && !keepRecordingWhenStreamStops)
		StopRecording();

	bool replayBufferWhileStreaming =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "ReplayBufferWhileStreaming");
	bool keepReplayBufferStreamStops =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "KeepReplayBufferStreamStops");
	if (replayBufferWhileStreaming && !keepReplayBufferStreamStops)
		StopReplayBuffer();
}

void OBSBasic::ForceStopStreaming()
{
	SaveProject();

	if (outputHandler->StreamingActive())
		outputHandler->StopStreaming(true);

	// special case: force reset broadcast state if
	// no autostart and no autostop selected
	if (!autoStartBroadcast && !broadcastActive) {
		broadcastActive = false;
		autoStartBroadcast = true;
		autoStopBroadcast = true;
		broadcastReady = false;
	}

	if (autoStopBroadcast) {
		broadcastActive = false;
		broadcastReady = false;
	}

	emit BroadcastStreamReady(broadcastReady);

	OnDeactivate();

	bool recordWhenStreaming = config_get_bool(App()->GetUserConfig(), "BasicWindow", "RecordWhenStreaming");
	bool keepRecordingWhenStreamStops =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "KeepRecordingWhenStreamStops");
	if (recordWhenStreaming && !keepRecordingWhenStreamStops)
		StopRecording();

	bool replayBufferWhileStreaming =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "ReplayBufferWhileStreaming");
	bool keepReplayBufferStreamStops =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "KeepReplayBufferStreamStops");
	if (replayBufferWhileStreaming && !keepReplayBufferStreamStops)
		StopReplayBuffer();
}

void OBSBasic::KillDestinationUngracefully(std::string id)
{
	StreamDestinationRuntime *primary = PrimaryRuntime();
	StreamDestinationRuntime *runtime = id.empty() ? primary : RuntimeForId(id);
	if (!runtime) {
		blog(LOG_WARNING, "KillDestinationUngracefully: no runtime entry for destination '%s'", id.c_str());
		return;
	}

	/* An extra destination holds its own output through its runtime entry (slice 2b); the first destination
	 * is served by the output handler's stream output (Q42). A destination that has no connection of its own
	 * has no socket to reset. */
	const bool primaryRuntime = (runtime == primary);
	if (!runtime->output && !primaryRuntime) {
		blog(LOG_INFO, "streaming destination '%s' has no connection of its own to kill",
		     runtime->name.c_str());
		return;
	}

	OBSOutputAutoRelease output = runtime->output ? OBSOutputAutoRelease(obs_output_get_ref(runtime->output))
						      : outputHandler->StreamingOutput();
	if (!output) {
		blog(LOG_WARNING, "KillDestinationUngracefully: nothing to abort for destination '%s'",
		     runtime->name.c_str());
		return;
	}

	runtime->killRequested = true;
	blog(LOG_WARNING, "Killing streaming destination '%s' ungracefully (simulated crash)", runtime->name.c_str());

	// Abruptly kill the RTMP session of this one destination (RST close, no end-of-stream data). Its own
	// "stop" signal settles the destination, and the session goes down with the last of its connections
	// (Q13) - nothing session-level happens here.
	const bool abortable = obs_output_active(output);
	obs_output_abort_stream(output);

	/* obs_output_abort_stream only reaches an output that is already uploading or mid-reconnect (its
	 * active() test covers both). A destination stuck in a handshake - the case a kill is most often asked
	 * for - is neither, so it has to be pulled down the way a stop during connect does it. */
	if (!abortable) {
		blog(LOG_INFO, "streaming destination '%s' was still connecting, force-stopping it instead",
		     runtime->name.c_str());
		obs_output_force_stop(output);
	}
}

/* Per-destination actions for the widgets that show the session (the stats window rows, and later the
 * hotkeys): one connection stopped on its own, one brought back, and the snapshot both of them render. */
void OBSBasic::StopDestination(std::string id, bool force)
{
	if (!outputHandler)
		return;

	StreamDestinationRuntime *runtime = id.empty() ? PrimaryRuntime() : RuntimeForId(id);
	if (!runtime) {
		blog(LOG_WARNING, "StopDestination: no runtime entry for destination '%s'", id.c_str());
		return;
	}

	blog(LOG_INFO, "stopping the connection of streaming destination '%s'%s", runtime->name.c_str(),
	     force ? " (forced)" : "");

	/* Counting as still running until its own stop signal arrives is what keeps the session alive while the
	 * other connections are up (Q13) - the transition is reported by the signal, not here. */
	runtime->state = StreamDestinationRuntime::State::Stopping;
	outputHandler->StopDestination(runtime->id, force);
	PushStreamConnections();
}

bool OBSBasic::RestartDestination(const std::string &id)
{
	StreamDestinationRuntime *runtime = id.empty() ? PrimaryRuntime() : RuntimeForId(id);
	if (!runtime) {
		blog(LOG_WARNING, "RestartDestination: no runtime entry for destination '%s'", id.c_str());
		return false;
	}
	if (!runtime->enabled) {
		blog(LOG_INFO, "streaming destination '%s' is disabled, so it does not get a connection back",
		     runtime->name.c_str());
		return false;
	}
	if (!outputHandler || !outputHandler->StreamingActive()) {
		/* Nothing of the session is left to attach to: starting again means starting a session, which is
		 * the normal StartStreaming path with all of its encoder setup. */
		blog(LOG_INFO, "streaming destination '%s' cannot be restarted on its own: the session has ended",
		     runtime->name.c_str());
		return false;
	}

	const bool started = outputHandler->StartDestination(runtime->id);
	PushStreamConnections();
	return started;
}

std::vector<OBSBasic::StreamSessionEntry> OBSBasic::StreamSessionSnapshot(bool includeStopped) const
{
	std::vector<StreamSessionEntry> entries;
	const bool sessionLive = outputHandler && outputHandler->StreamingActive();

	for (const auto &runtime : streamRuntimes) {
		if (!runtime.enabled)
			continue;

		const bool running = runtime.isRunning();
		/* A destination that is not part of a live session has nothing to show: after the session ended,
		 * the entries stay in the list to remember their configuration, not to be reported as down. */
		if (!running && !(includeStopped && sessionLive))
			continue;

		StreamSessionEntry entry;
		entry.id = runtime.id;
		entry.name = runtime.name;
		entry.output = runtime.output;
		entry.state = runtime.state;
		entry.reconnectAttempt = runtime.reconnectAttempt;
		entry.reconnecting = runtime.output ? obs_output_reconnecting(runtime.output) : false;
		entry.stopped = !running;
		entries.push_back(std::move(entry));
	}

	return entries;
}

void OBSBasic::KillStreamingUngracefully()
{
	if (!outputHandler->StreamingActive())
		return;

	blog(LOG_INFO,
	     "Force-killing the streaming session ungracefully (simulated crash): %zu connection(s) of %zu enabled destinations",
	     RunningDestinationCount(), EnabledDestinationCount());

	// Make sure automatic restart does not bring the stream back up
	if (outputHandler->multitrackVideo)
		outputHandler->multitrackVideo->SetRestartOnError(false);

	/* Killing the session means killing each of its connections separately: every one of them gets its own
	 * abort and its own OBS_OUTPUT_KILLED marker, so a killed connection is never mistaken for a dropped
	 * one. The ids are collected first because aborting an output can report its stop right away, which
	 * moves runtime entries to "stopped" while this loop is still running. */
	std::vector<std::string> ids;
	ids.reserve(streamRuntimes.size());
	for (const auto &runtime : streamRuntimes) {
		if (runtime.isRunning())
			ids.push_back(runtime.id);
	}

	for (const auto &id : ids)
		KillDestinationUngracefully(id);

	/* Outputs that cannot abort themselves (an output type without an abort implementation) still have to
	 * go down with the session; a second stop of an output that is already down does nothing. */
	outputHandler->StopExtraDestinations(true);
}

void OBSBasic::StreamDelayStarting(int sec)
{
	emit StreamingStarted(true);

	if (StreamDestinationRuntime *runtime = CurrentRuntime())
		runtime->state = StreamDestinationRuntime::State::DelayStarting;

	if (sysTrayStream) {
		sysTrayStream->setText(QTStr("Basic.Main.StopStreaming"));
		sysTrayStream->setEnabled(true);
	}

	ui->statusbar->StreamDelayStarting(sec);

	OnActivate();
}

void OBSBasic::StreamDelayStopping(int sec)
{
	emit StreamingStopped(true);

	if (StreamDestinationRuntime *runtime = CurrentRuntime())
		runtime->state = StreamDestinationRuntime::State::DelayStopping;

	if (sysTrayStream) {
		sysTrayStream->setText(QTStr("Basic.Main.StartStreaming"));
		sysTrayStream->setEnabled(true);
	}

	ui->statusbar->StreamDelayStopping(sec);

	OnEvent(OBS_FRONTEND_EVENT_STREAMING_STOPPING);
}

void OBSBasic::StreamingStart()
{
	emit StreamingStarted();

	if (StreamDestinationRuntime *runtime = CurrentRuntime()) {
		runtime->state = StreamDestinationRuntime::State::Live;
		runtime->output = outputHandler->StreamingOutput();
		blog(LOG_INFO, "Streaming destination '%s' is live (%zu of %zu enabled destinations up)",
		     runtime->name.c_str(), LiveDestinationCount(), EnabledDestinationCount());
	}
	OBSOutputAutoRelease output = obs_frontend_get_streaming_output();
	ui->statusbar->StreamStarted(output);

	if (sysTrayStream) {
		sysTrayStream->setText(QTStr("Basic.Main.StopStreaming"));
		sysTrayStream->setEnabled(true);
	}

#ifdef YOUTUBE_ENABLED
	if (!autoStartBroadcast) {
		// get a current stream key
		obs_service_t *service_obj = GetService();
		OBSDataAutoRelease settings = obs_service_get_settings(service_obj);
		std::string key = obs_data_get_string(settings, "stream_id");
		if (!key.empty() && !youtubeStreamCheckThread) {
			youtubeStreamCheckThread = CreateQThread([this, key] { YoutubeStreamCheck(key); });
			youtubeStreamCheckThread->setObjectName("YouTubeStreamCheckThread");
			youtubeStreamCheckThread->start();
		}
	}
#endif

	OnEvent(OBS_FRONTEND_EVENT_STREAMING_STARTED);

	OnActivate();

#ifdef YOUTUBE_ENABLED
	if (YouTubeAppDock::IsYTServiceSelected())
		youtubeAppDock->IngestionStarted();
#endif

	blog(LOG_INFO, STREAMING_START);
}

void OBSBasic::StreamStopping()
{
	emit StreamingStopping();

	if (StreamDestinationRuntime *runtime = CurrentRuntime())
		runtime->state = StreamDestinationRuntime::State::Stopping;

	if (sysTrayStream)
		sysTrayStream->setText(QTStr("Basic.Main.StoppingStreaming"));

	streamingStopping = true;
	OnEvent(OBS_FRONTEND_EVENT_STREAMING_STOPPING);
}

void OBSBasic::StreamingStop(int code, QString last_error)
{
	/* Q13: the session is the set of connections of the session, not its first one. A connection going
	 * down while another is still up is a per-destination failure and must not touch anything that
	 * describes the session; the session comes down with the last connection, which is decided below by
	 * asking the output handler what it still has running. Remembering whether the user asked for the stop
	 * keeps a stop of the whole session from producing failure dialogs for the connections that were
	 * already dropping when it was pressed. */
	const bool userRequestedStop = streamingStopping;

	const char *errorDescription = "";
	DStr errorMessage;
	bool use_last_error = false;
	bool encode_error = false;
	bool should_reconnect = false;

	// An ungraceful kill requested via the skull button must behave exactly like clicking "Stop Streaming":
	// no error dialog, no reconnect handling -- just a clean stop.
	// The marker string is written by plugins/obs-outputs' abort path (see rtmp_stream_abort), so a stop
	// event carrying it is one of ours even if this destination's request flag was consumed elsewhere. The
	// flag is per destination: a killed connection never silences the failure report of another one.
	StreamDestinationRuntime *killedRuntime = CurrentRuntime();
	const bool ungracefulKill = last_error.contains(QStringLiteral("Stream killed ungracefully")) ||
				    (killedRuntime && killedRuntime->killRequested);
	if (ungracefulKill) {
		if (killedRuntime)
			killedRuntime->killRequested = false;
		code = OBS_OUTPUT_SUCCESS;
	}

	/* Ignore stream key error for multitrack output if its internal reconnect handling is active. */
	if (code == OBS_OUTPUT_INVALID_STREAM && outputHandler->multitrackVideo &&
	    outputHandler->multitrackVideo->RestartOnError()) {
		code = OBS_OUTPUT_SUCCESS;
		should_reconnect = true;
	}

	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		errorDescription = Str("Output.ConnectFail.BadPath");
		break;

	case OBS_OUTPUT_CONNECT_FAILED:
		use_last_error = true;
		errorDescription = Str("Output.ConnectFail.ConnectFailed");
		break;

	case OBS_OUTPUT_INVALID_STREAM:
		errorDescription = Str("Output.ConnectFail.InvalidStream");
		break;

	case OBS_OUTPUT_ENCODE_ERROR:
		encode_error = true;
		break;

	case OBS_OUTPUT_HDR_DISABLED:
		errorDescription = Str("Output.ConnectFail.HdrDisabled");
		break;

	default:
	case OBS_OUTPUT_ERROR:
		use_last_error = true;
		errorDescription = Str("Output.ConnectFail.Error");
		break;

	case OBS_OUTPUT_DISCONNECTED:
		/* doesn't happen if output is set to reconnect.  note that
		 * reconnects are handled in the output, not in the UI */
		use_last_error = true;
		errorDescription = Str("Output.ConnectFail.Disconnected");
	}

	if (use_last_error && !last_error.isEmpty())
		dstr_printf(errorMessage, "%s\n\n%s", errorDescription, QT_TO_UTF8(last_error));
	else
		dstr_copy(errorMessage, errorDescription);

	/* The destination this signal belongs to. An entry that is already stopped is skipped, which is what
	 * keeps the last extra destination - routed through this same function - from re-logging the first one.
	 * Declared outside the if, because the failure report further down has to know which connection this
	 * particular signal was about. */
	StreamDestinationRuntime *runtime = CurrentRuntime();
	const bool stopped_here = runtime && runtime->state != StreamDestinationRuntime::State::Stopped;
	if (stopped_here) {
		const char *previousState = runtime->stateName();
		runtime->state = StreamDestinationRuntime::State::Stopped;
		runtime->output = nullptr;
		runtime->service = nullptr;
		runtime->encoderGroupId.clear();
		if (should_reconnect)
			runtime->reconnectAttempt++;
		else
			runtime->reconnectAttempt = 0;
		blog(LOG_INFO, "Streaming destination '%s' stopped after %s (%zu of %zu enabled destinations up)",
		     runtime->name.c_str(), previousState, LiveDestinationCount(), EnabledDestinationCount());
	}

	/* The bar shows the session, so it has to learn about this connection going down even when the session
	 * itself continues. */
	PushStreamConnections();

	/* Q13 (b): one connection going down does not end the session while other destinations of the session
	 * are still up. Nothing that describes the session as a whole may change here - not the status bar,
	 * not the tray entry, not the frontend/websocket event, and not the recording or replay-buffer
	 * coupling. They all follow the session, which ends with the last connection. A destination that is
	 * still working on its handshake counts as a connection of the session even though libobs does not
	 * call its output active yet, which is what the runtime count is for. */
	const size_t stillRunning = RunningDestinationCount();
	if (outputHandler->StreamingActive() || stillRunning > 0) {
		blog(LOG_INFO,
		     "streaming session continues after a destination went down: %zu of %zu enabled destinations still attempting or uploading",
		     stillRunning, EnabledDestinationCount());
		/* Q13 (ii): the session goes on, so this is reported as a message rather than a dialog - and a
		 * drop that leaves the session running has to be reported, or the user only notices it when a
		 * connection they expected to be live never shows up. */
		if (stopped_here)
			ReportDestinationFailure(*runtime, code, ungracefulKill, QT_UTF8(errorMessage),
						 encode_error || code == OBS_OUTPUT_HDR_DISABLED);
		return;
	}

	ui->statusbar->StreamStopped();

	emit StreamingStopped();

	if (sysTrayStream) {
		sysTrayStream->setText(QTStr("Basic.Main.StartStreaming"));
		sysTrayStream->setEnabled(true);
	}

	/* An output that never got as far as starting emits no stop signal, so its runtime entry would be
	 * left saying "connecting" after the session is over. Everything that is not live and not already
	 * going down is simply stopped with the session. */
	for (auto &runtime : streamRuntimes) {
		if (runtime.state == StreamDestinationRuntime::State::Preparing ||
		    runtime.state == StreamDestinationRuntime::State::Connecting)
			runtime.state = StreamDestinationRuntime::State::Stopped;
	}

	streamingStopping = false;
	OnEvent(OBS_FRONTEND_EVENT_STREAMING_STOPPED);

	OnDeactivate();

#ifdef YOUTUBE_ENABLED
	if (YouTubeAppDock::IsYTServiceSelected())
		youtubeAppDock->IngestionStopped();
#endif

	blog(LOG_INFO, STREAMING_STOP);

	if (encode_error) {
		QString msg = last_error.isEmpty() ? QTStr("Output.StreamEncodeError.Msg")
						   : QTStr("Output.StreamEncodeError.Msg.LastError").arg(last_error);
		OBSMessageBox::information(this, QTStr("Output.StreamEncodeError.Title"), msg);

	} else if (code != OBS_OUTPUT_SUCCESS && !userRequestedStop && isVisible()) {
		/* Q13 (ii): only a stop that nobody asked for is worth a modal. When the user stopped the
		 * session themselves, connections that were already dropping on the way down must not pop an
		 * error window on top of a stream they just ended. */
		OBSMessageBox::information(this, QTStr("Output.ConnectFail.Title"), QT_UTF8(errorMessage));

	} else if (code != OBS_OUTPUT_SUCCESS && !userRequestedStop && !isVisible()) {
		SysTrayNotify(QT_UTF8(errorDescription), QSystemTrayIcon::Warning);
	}

	// Reset broadcast button state/text
	if (!broadcastActive)
		SetBroadcastFlowEnabled(auth && auth->broadcastFlow());
	if (should_reconnect)
		QMetaObject::invokeMethod(this, "StartStreaming", Qt::QueuedConnection);
}

void OBSBasic::StreamActionTriggered()
{
	if (outputHandler->StreamingActive()) {
		bool confirm = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingStream");

#ifdef YOUTUBE_ENABLED
		if (isVisible() && auth && IsYouTubeService(auth->service()) && autoStopBroadcast) {
			QMessageBox::StandardButton button = OBSMessageBox::question(
				this, QTStr("ConfirmStop.Title"), QTStr("YouTube.Actions.AutoStopStreamingWarning"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

			if (button == QMessageBox::No)
				return;

			confirm = false;
		}
#endif
		if (confirm && isVisible()) {
			QMessageBox::StandardButton button =
				OBSMessageBox::question(this, QTStr("ConfirmStop.Title"), QTStr("ConfirmStop.Text"),
							QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

			if (button == QMessageBox::No)
				return;
		}

		StopStreaming();
	} else {
		if (!UIValidation::NoSourcesConfirmation(this))
			return;

		Auth *auth = GetAuth();

		auto action = (auth && auth->external()) ? StreamSettingsAction::ContinueStream
							 : UIValidation::StreamSettingsConfirmation(this, service);
		switch (action) {
		case StreamSettingsAction::ContinueStream:
			break;
		case StreamSettingsAction::OpenSettings:
			on_action_Settings_triggered();
			return;
		case StreamSettingsAction::Cancel:
			return;
		}

		bool confirm = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStartingStream");

		bool bwtest = false;

		if (this->auth) {
			OBSDataAutoRelease settings = obs_service_get_settings(service);
			bwtest = obs_data_get_bool(settings, "bwtest");
			// Disable confirmation if this is going to open broadcast setup
			if (auth && auth->broadcastFlow() && !broadcastReady && !broadcastActive)
				confirm = false;
		}

		if (bwtest && isVisible()) {
			QMessageBox::StandardButton button = OBSMessageBox::question(this, QTStr("ConfirmBWTest.Title"),
										     QTStr("ConfirmBWTest.Text"));

			if (button == QMessageBox::No)
				return;
		} else if (confirm && isVisible()) {
			QMessageBox::StandardButton button =
				OBSMessageBox::question(this, QTStr("ConfirmStart.Title"), QTStr("ConfirmStart.Text"),
							QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

			if (button == QMessageBox::No)
				return;
		}

		StartStreaming();
	}
}

bool OBSBasic::StreamingActive()
{
	if (!outputHandler)
		return false;
	return outputHandler->StreamingActive();
}

bool OBSBasic::ApplyMultitrackConfigOverride(const std::string &json, std::string *failure_reason)
{
	if (!outputHandler || !outputHandler->multitrackVideo || !outputHandler->multitrackVideoActive) {
		if (failure_reason)
			*failure_reason = "no multitrack video stream is active";
		return false;
	}

	return outputHandler->multitrackVideo->ApplyConfigOverride(json, failure_reason);
}

void OBSBasic::SetMultitrackDebugOverlay(bool enabled, int corner, int size)
{
	if (outputHandler && outputHandler->multitrackVideo)
		outputHandler->multitrackVideo->SetDebugOverlay(enabled, corner, size);
}

extern std::string DeserializeConfigText(const char *value);

void OBSBasic::ApplyMultitrackConfigOverridePreset(int index)
{
	if (index < 0 || index > 2)
		return;

	config_t *config = Config();
	if (!config)
		return;

	const bool stream_active =
		outputHandler && outputHandler->multitrackVideo && outputHandler->multitrackVideoActive;

	auto set_active = [&](int active_index) {
		config_set_int(config, "Stream1", "MultitrackVideoConfigOverrideActiveSource", active_index);
		if (controlsDock) {
			if (auto *controls = controlsDock->findChild<OBSBasicControls *>())
				controls->SetConfigOverridePresetActive(active_index);
		}
	};

	// "None": fall back to the default behavior (go-live API, or the config override box when enabled). The
	// enable checkbox and the JSON box are left untouched.
	if (index == 1) {
		set_active(1);

		if (!stream_active)
			return;

		// Mirror the stream-start resolution for "None": the config override box when enabled, otherwise go-live.
		std::string json;
		if (config_get_bool(config, "Stream1", "MultitrackVideoConfigOverrideEnabled")) {
			const char *raw = config_get_string(config, "Stream1", "MultitrackVideoConfigOverride");
			json = raw ? DeserializeConfigText(raw) : std::string();
		}
		if (json.empty())
			json = outputHandler->multitrackVideo->GetGoLiveConfigJson();

		if (json.empty()) {
			QMessageBox::information(this, QTStr("Basic.Settings.Stream.MultitrackVideoLabel"),
				QString("No config override is available to apply live; the default settings apply on restart."));
			return;
		}

		std::string reason;
		if (!ApplyMultitrackConfigOverride(json, &reason)) {
			QString msg = QString("The config override could not be applied to the running stream: %1")
					.arg(QString::fromStdString(reason));
			QMessageBox::warning(this, QTStr("Basic.Settings.Stream.MultitrackVideoLabel"), msg);
		}
		return;
	}

	// Preset 1 (index 0) / Preset 2 (index 2): use that preset's JSON, ignoring the enable checkbox and box.
	const int preset_number = (index == 0) ? 1 : 2;
	const std::string preset_key = "MultitrackVideoConfigOverridePreset" + std::to_string(preset_number);

	std::string json;
	if (config_has_user_value(config, "Stream1", preset_key.c_str())) {
		const char *raw = config_get_string(config, "Stream1", preset_key.c_str());
		json = raw ? DeserializeConfigText(raw) : std::string();
	}

	if (json.empty()) {
		QMessageBox::warning(this, QTStr("Basic.Settings.Stream.MultitrackVideoLabel"),
		                     QString("Config override preset %1 has not been set.\nSet it in Settings -> Stream.")
			                     .arg(preset_number));
		return;
	}

	set_active(index);

	// Apply live only when a multitrack video stream is actually running; otherwise it is saved for the next start.
	if (!stream_active)
		return;

	std::string reason;
	if (!ApplyMultitrackConfigOverride(json, &reason))
		QMessageBox::warning(this, QTStr("Basic.Settings.Stream.MultitrackVideoLabel"),
		                      QString("The config override preset could not be applied to the running stream: %1")
			                      .arg(QString::fromStdString(reason)));
}

void OBSBasic::UpdateConfigOverridePresetDock()
{
	if (!controlsDock)
		return;

	auto *controls = controlsDock->findChild<OBSBasicControls *>();
	if (!controls)
		return;

	config_t *config = Config();
	if (!config)
		return;

	// Only show the preset toggles when enhanced broadcasting (multitrack video) is enabled.
	const bool multitrack_enabled = config_get_bool(config, "Stream1", "EnableMultitrackVideo");
	controls->SetConfigOverridePresetVisible(multitrack_enabled);
	if (!multitrack_enabled)
		return;

	// Restore the highlighted toggle (the persisted active source, default None).
	int active = 1; // None
	if (config_has_user_value(config, "Stream1", "MultitrackVideoConfigOverrideActiveSource"))
		active = config_get_int(config, "Stream1", "MultitrackVideoConfigOverrideActiveSource");
	if (active < 0 || active > 2)
		active = 1;
	controls->SetConfigOverridePresetActive(active);
}

