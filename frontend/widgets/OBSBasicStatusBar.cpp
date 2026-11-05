#include "OBSBasicStatusBar.hpp"
#include "ui_StatusBarWidget.h"

#include <utility/EncoderStats.hpp>

#include <widgets/OBSBasic.hpp>

#include "moc_OBSBasicStatusBar.cpp"

#include <algorithm>

static constexpr int bitrateUpdateSeconds = 2;
static constexpr int congestionUpdateSeconds = 4;
static constexpr float excellentThreshold = 0.0f;
static constexpr float goodThreshold = 0.3333f;
static constexpr float mediocreThreshold = 0.6667f;
static constexpr float badThreshold = 1.0f;

OBSBasicStatusBar::OBSBasicStatusBar(QWidget *parent)
	: QStatusBar(parent),
	  excellentPixmap(QIcon(":/res/images/network-excellent.svg").pixmap(QSize(16, 16))),
	  goodPixmap(QIcon(":/res/images/network-good.svg").pixmap(QSize(16, 16))),
	  mediocrePixmap(QIcon(":/res/images/network-mediocre.svg").pixmap(QSize(16, 16))),
	  badPixmap(QIcon(":/res/images/network-bad.svg").pixmap(QSize(16, 16))),
	  recordingActivePixmap(QIcon(":/res/images/recording-active.svg").pixmap(QSize(16, 16))),
	  recordingPausePixmap(QIcon(":/res/images/recording-pause.svg").pixmap(QSize(16, 16))),
	  streamingActivePixmap(QIcon(":/res/images/streaming-active.svg").pixmap(QSize(16, 16)))
{
	congestionArray.reserve(congestionUpdateSeconds);

	statusWidget = new StatusBarWidget(this);
	statusWidget->ui->delayInfo->setText("");
	statusWidget->ui->droppedFrames->setText(QTStr("DroppedFrames").arg("0", "0.0"));
	statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
	statusWidget->ui->streamTime->setDisabled(true);
	statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
	statusWidget->ui->recordTime->setDisabled(true);
	statusWidget->ui->delayFrame->hide();
	statusWidget->ui->issuesFrame->hide();
	statusWidget->ui->kbps->hide();

	addPermanentWidget(statusWidget, 1);
	setMinimumHeight(statusWidget->height());

	UpdateIcons();
	connect(App(), &OBSApp::StyleChanged, this, &OBSBasicStatusBar::UpdateIcons);

	messageTimer = new QTimer(this);
	messageTimer->setSingleShot(true);
	connect(messageTimer, &QTimer::timeout, this, &OBSBasicStatusBar::clearMessage);

	clearMessage();
}

/* The base canvas mix has no consumers of its own in this fork (every encoder
 * consumes an encoder-only delivery mix), so the "due to encoding lag" counters
 * have to be summed over the encoders of the running outputs. */
OBS::EncoderFrameCounters OBSBasicStatusBar::activeEncoderFrameCounters()
{
	std::vector<const obs_output_t *> outputs;
	for (const OBSOutput &output : sessionOutputs())
		outputs.push_back(output);

	OBSOutput record = recordOutput ? OBSGetStrongRef(recordOutput) : OBSOutput();
	if (record)
		outputs.push_back(record);

	return OBS::outputFrameCounters(outputs);
}

/* Every connection of the streaming session, in the order the session reports them: the first destination's
 * output (Q42) plus one per extra destination (slice 2b). The encoders behind them are shared, which is why
 * the sums in EncoderStats count each encoder and each delivery mix once. */
std::vector<OBSOutput> OBSBasicStatusBar::sessionOutputs()
{
	std::vector<OBSOutput> outputs;

	for (const auto &entry : connections) {
		if (!entry.second.output)
			continue;
		OBSOutput output = OBSGetStrongRef(entry.second.output);
		if (output)
			outputs.push_back(output);
	}

	/* A session started before any per-destination report arrived is still the single stream the handler
	 * owns, and the legacy field knows it. */
	if (outputs.empty() && streamOutput) {
		OBSOutput output = OBSGetStrongRef(streamOutput);
		if (output)
			outputs.push_back(output);
	}

	return outputs;
}

OBSBasicStatusBar::Connection *OBSBasicStatusBar::findConnection(const std::string &id)
{
	auto it = connections.find(id);
	return it == connections.end() ? nullptr : &it->second;
}

void OBSBasicStatusBar::SetStreamConnections(const std::vector<StreamConnectionInfo> &list)
{
	const std::string primaryId = list.empty() ? std::string() : list.front().id;

	/* Drop what is gone: removing the entry disconnects its signal handlers, because they live in it. An
	 * entry whose destination is still listed but now runs on a different output (a restart) is dropped and
	 * rebuilt, so the handlers watch the connection that exists. */
	for (auto it = connections.begin(); it != connections.end();) {
		const auto match = std::find_if(list.begin(), list.end(),
						[&](const StreamConnectionInfo &info) { return info.id == it->first; });
		OBSOutput existing = OBSGetStrongRef(it->second.output);
		const bool same_output = match != list.end() && existing && existing == match->output;
		if (!same_output)
			it = connections.erase(it);
		else
			++it;
	}

	for (const auto &info : list) {
		/* Built in place: an entry holds the signal connections of its output, and copying one of those
		 * would connect the same handler twice. */
		Connection &entry = connections[info.id];
		if (entry.output)
			continue;

		entry.id = info.id;
		entry.name = info.name;
		entry.output = OBSWeakOutputAutoRelease(info.output ? OBSGetWeakRef(info.output) : nullptr);
		entry.retries = 0;
		entry.reconnectTimeout = 0;

		/* Only the first destination's output is watched from here: the extra destinations are owned by
		 * the output handler, which forwards their reconnect signals through NoteStreamReconnect. That
		 * keeps every handler argument a long-lived object instead of an entry that can be removed.
		 * "First" is the order the session passed in, not the map order. */
		if (info.output && info.id == primaryId) {
			signal_handler_t *handler = obs_output_get_signal_handler(info.output);
			entry.sigs.emplace_back(handler, "reconnect", OBSOutputReconnect, this);
			entry.sigs.emplace_back(handler, "reconnect_success", OBSOutputReconnectSuccess, this);
		}
	}

	/* The legacy single-connection fields follow the first connection of the session, which keeps the icon
	 * and the activate/deactivate behaviour of the single-stream case exactly as it was. */
	if (!list.empty() && list.front().output)
		streamOutput = OBSGetWeakRef(list.front().output);
	else
		streamOutput = nullptr;
	primaryConnectionId = primaryId;

	if (!connections.empty()) {
		lastBytesSent = 0;
		lastBytesSentTime = os_gettime_ns();
		Activate();
	} else {
		clearMessage();
	}
}

void OBSBasicStatusBar::ConnectionMessage()
{
	/* One line for the whole session: how many connections are up, and which ones are waiting for their
	 * next attempt. With a single connection this is the wording the single-stream case always used. */
	if (connections.size() <= 1) {
		for (const auto &entry : connections) {
			const Connection &c = entry.second;
			if (c.reconnectTimeout > 0) {
				showMessage(
					QTStr("Basic.StatusBar.Reconnecting")
						.arg(QString::number(c.retries), QString::number(c.reconnectTimeout)));
			} else if (c.retries > 0) {
				showMessage(
					QTStr("Basic.StatusBar.AttemptingReconnect").arg(QString::number(c.retries)));
			}
		}
		return;
	}

	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	const int enabled = main ? int(main->EnabledDestinationCount()) : int(connections.size());

	/* "up" means uploading, not merely listed: a connection that is between attempts is down even while it
	 * still has an entry, and libobs counts a reconnecting output as active. */
	int up = 0;
	for (const auto &entry : connections) {
		if (!entry.second.output)
			continue;
		OBSOutput output = OBSGetStrongRef(entry.second.output);
		if (output && obs_output_active(output) && !obs_output_reconnecting(output))
			up++;
	}

	const QChar sep = QChar(0x00B7);
	QString text = QTStr("Basic.StatusBar.DestinationsUp").arg(QString::number(up), QString::number(enabled));

	for (const auto &entry : connections) {
		const Connection &c = entry.second;
		const QString name = c.name.empty() ? QTStr("Basic.Stats.Output.Stream") : QT_UTF8(c.name.c_str());
		if (c.reconnectTimeout > 0) {
			text += QStringLiteral(" ") + sep + QStringLiteral(" ") +
				QTStr("Basic.StatusBar.DestinationReconnecting")
					.arg(name, QString::number(c.retries), QString::number(c.reconnectTimeout));
		} else if (c.retries > 0) {
			text += QStringLiteral(" ") + sep + QStringLiteral(" ") +
				QTStr("Basic.StatusBar.DestinationAttempting").arg(name, QString::number(c.retries));
		}
	}

	showMessage(text);
}

void OBSBasicStatusBar::Activate()
{
	if (!active) {
		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &OBSBasicStatusBar::UpdateStatusBar);

		OBS::EncoderFrameCounters counters = activeEncoderFrameCounters();

		totalStreamSeconds = 0;
		totalRecordSeconds = 0;
		lastSkippedFrameCount = 0;
		startSkippedFrameCount = counters.lagSkipped;
		startTotalFrameCount = counters.delivered;

		refreshTimer->start(1000);
		active = true;

		if (streamOutput) {
			statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		}
	}

	if (streamOutput) {
		statusWidget->ui->streamIcon->setPixmap(streamingActivePixmap);
		statusWidget->ui->streamTime->setDisabled(false);
		statusWidget->ui->issuesFrame->show();
		statusWidget->ui->kbps->show();
		firstCongestionUpdate = true;
	}

	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingActivePixmap);
		statusWidget->ui->recordTime->setDisabled(false);
	}
}

void OBSBasicStatusBar::Deactivate()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main)
		return;

	if (!streamOutput) {
		statusWidget->ui->streamTime->setText(QString("00:00:00"));
		statusWidget->ui->streamTime->setDisabled(true);
		statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		statusWidget->ui->delayFrame->hide();
		statusWidget->ui->issuesFrame->hide();
		statusWidget->ui->kbps->hide();
		totalStreamSeconds = 0;
		congestionArray.clear();
		disconnected = false;
		firstCongestionUpdate = false;
	}

	if (!recordOutput) {
		statusWidget->ui->recordTime->setText(QString("00:00:00"));
		statusWidget->ui->recordTime->setDisabled(true);
		statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
		totalRecordSeconds = 0;
	}

	if (main->outputHandler && !main->outputHandler->Active()) {
		delete refreshTimer;

		statusWidget->ui->delayInfo->setText("");
		statusWidget->ui->droppedFrames->setText(QTStr("DroppedFrames").arg("0", "0.0"));
		statusWidget->ui->kbps->setText("0 kbps");

		delaySecTotal = 0;
		delaySecStarting = 0;
		delaySecStopping = 0;
		reconnectTimeout = 0;
		for (auto &entry : connections) {
			entry.second.retries = 0;
			entry.second.reconnectTimeout = 0;
		}
		active = false;
		overloadedNotify = true;

		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	}
}

void OBSBasicStatusBar::UpdateDelayMsg()
{
	QString msg;

	if (delaySecTotal) {
		if (delaySecStarting && !delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStartingIn");
			msg = msg.arg(QString::number(delaySecStarting));

		} else if (!delaySecStarting && delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStoppingIn");
			msg = msg.arg(QString::number(delaySecStopping));

		} else if (delaySecStarting && delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStartingStoppingIn");
			msg = msg.arg(QString::number(delaySecStopping), QString::number(delaySecStarting));
		} else {
			msg = QTStr("Basic.StatusBar.Delay");
			msg = msg.arg(QString::number(delaySecTotal));
		}

		if (!statusWidget->ui->delayFrame->isVisible())
			statusWidget->ui->delayFrame->show();

		statusWidget->ui->delayInfo->setText(msg);
	}
}

void OBSBasicStatusBar::UpdateBandwidth()
{
	if (!streamOutput && connections.empty())
		return;

	if (++seconds < bitrateUpdateSeconds)
		return;

	/* The session uploads one copy per connection, so the number shown is the sum over its outputs - the
	 * same figure the destinations page lists as the combined upload. */
	uint64_t bytesSent = 0;
	for (const OBSOutput &output : sessionOutputs())
		bytesSent += obs_output_get_total_bytes(output);
	uint64_t bytesSentTime = os_gettime_ns();

	if (bytesSent < lastBytesSent)
		bytesSent = 0;
	if (bytesSent == 0)
		lastBytesSent = 0;

	uint64_t bitsBetween = (bytesSent - lastBytesSent) * 8;

	double timePassed = double(bytesSentTime - lastBytesSentTime) / 1000000000.0;

	double kbitsPerSec = double(bitsBetween) / timePassed / 1000.0;

	QString text;
	text += QString::number(kbitsPerSec, 'f', 0) + QString(" kbps");

	statusWidget->ui->kbps->setText(text);
	statusWidget->ui->kbps->setMinimumWidth(statusWidget->ui->kbps->width());

	if (!statusWidget->ui->kbps->isVisible())
		statusWidget->ui->kbps->show();

	lastBytesSent = bytesSent;
	lastBytesSentTime = bytesSentTime;
	seconds = 0;
}

void OBSBasicStatusBar::UpdateCPUUsage()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main)
		return;

	QString text;
	text += QString("CPU: ") + QString::number(main->GetCPUUsage(), 'f', 1) + QString("%");

	statusWidget->ui->cpuUsage->setText(text);
	statusWidget->ui->cpuUsage->setMinimumWidth(statusWidget->ui->cpuUsage->width());

	UpdateCurrentFPS();
}

void OBSBasicStatusBar::UpdateCurrentFPS()
{
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	float targetFPS = (float)ovi.fps_num / (float)ovi.fps_den;

	QString text = QString::asprintf("%.2f / %.2f FPS", obs_get_active_fps(), targetFPS);

	statusWidget->ui->fpsCurrent->setText(text);
	statusWidget->ui->fpsCurrent->setMinimumWidth(statusWidget->ui->fpsCurrent->width());
}

void OBSBasicStatusBar::UpdateStreamTime()
{
	totalStreamSeconds++;

	int seconds = totalStreamSeconds % 60;
	int totalMinutes = totalStreamSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString text = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	statusWidget->ui->streamTime->setText(text);
	if (streamOutput && !statusWidget->ui->streamTime->isEnabled())
		statusWidget->ui->streamTime->setDisabled(false);

	/* Every connection counts down its own wait: the attempt counter and the backoff belong to one output
	 * each inside libobs, so a destination that is between attempts is named as such instead of hiding the
	 * ones that are still uploading. */
	bool anyWaiting = false;
	for (auto &entry : connections) {
		if (entry.second.reconnectTimeout > 0) {
			--entry.second.reconnectTimeout;
			anyWaiting = true;
		}
	}

	if (connections.empty()) {
		/* Single-stream fallback on the legacy fields, unchanged from before. */
		if (reconnectTimeout > 0) {
			showMessage(QTStr("Basic.StatusBar.Reconnecting")
					    .arg(QString::number(retries), QString::number(reconnectTimeout)));
			anyWaiting = true;
			reconnectTimeout--;
		} else if (retries > 0) {
			showMessage(QTStr("Basic.StatusBar.AttemptingReconnect").arg(QString::number(retries)));
		}
	} else {
		ConnectionMessage();
	}

	if (anyWaiting) {
		disconnected = true;
		statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
		congestionArray.clear();
	}

	if (delaySecStopping > 0 || delaySecStarting > 0) {
		if (delaySecStopping > 0)
			--delaySecStopping;
		if (delaySecStarting > 0)
			--delaySecStarting;
		UpdateDelayMsg();
	}
}

extern volatile bool recording_paused;

void OBSBasicStatusBar::UpdateRecordTime()
{
	bool paused = os_atomic_load_bool(&recording_paused);

	if (!paused) {
		totalRecordSeconds++;

		if (recordOutput && !statusWidget->ui->recordTime->isEnabled())
			statusWidget->ui->recordTime->setDisabled(false);
	} else {
		statusWidget->ui->recordIcon->setPixmap(streamPauseIconToggle ? recordingPauseInactivePixmap
									      : recordingPausePixmap);

		streamPauseIconToggle = !streamPauseIconToggle;
	}

	UpdateRecordTimeLabel();
}

void OBSBasicStatusBar::UpdateRecordTimeLabel()
{
	int seconds = totalRecordSeconds % 60;
	int totalMinutes = totalRecordSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString text = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	if (os_atomic_load_bool(&recording_paused)) {
		text += QStringLiteral(" (PAUSED)");
	}

	statusWidget->ui->recordTime->setText(text);
}

void OBSBasicStatusBar::UpdateDroppedFrames()
{
	std::vector<OBSOutput> outputs = sessionOutputs();
	if (outputs.empty())
		return;

	/* Every connection sends the same media, so a frame dropped by one of them is the number that matters:
	 * the worst connection, not the sum (summing would count one encoding problem several times). */
	int totalDropped = 0;
	int totalFrames = 0;
	float congestion = 0.0f;

	for (const OBSOutput &output : outputs) {
		totalDropped = std::max(totalDropped, obs_output_get_frames_dropped(output));
		totalFrames = std::max(totalFrames, obs_output_get_total_frames(output));
		congestion = std::max(congestion, obs_output_get_congestion(output));
	}

	double percent = (double)totalDropped / (double)totalFrames * 100.0;

	if (!totalFrames)
		return;

	QString text = QTStr("DroppedFrames");
	text = text.arg(QString::number(totalDropped), QString::number(percent, 'f', 1));
	statusWidget->ui->droppedFrames->setText(text);

	if (!statusWidget->ui->issuesFrame->isVisible())
		statusWidget->ui->issuesFrame->show();

	/* ----------------------------------- *
	 * calculate congestion color          */

	float avgCongestion = (congestion + lastCongestion) * 0.5f;
	if (avgCongestion < congestion)
		avgCongestion = congestion;
	if (avgCongestion > 1.0f)
		avgCongestion = 1.0f;

	lastCongestion = congestion;

	if (disconnected)
		return;

	bool update = firstCongestionUpdate;
	float congestionOverTime = avgCongestion;

	if (congestionArray.size() >= congestionUpdateSeconds) {
		congestionOverTime = accumulate(congestionArray.begin(), congestionArray.end(), 0.0f) /
				     (float)congestionArray.size();
		congestionArray.clear();
		update = true;
	} else {
		congestionArray.emplace_back(avgCongestion);
	}

	if (update) {
		if (congestionOverTime <= excellentThreshold + EPSILON)
			statusWidget->ui->statusIcon->setPixmap(excellentPixmap);
		else if (congestionOverTime <= goodThreshold)
			statusWidget->ui->statusIcon->setPixmap(goodPixmap);
		else if (congestionOverTime <= mediocreThreshold)
			statusWidget->ui->statusIcon->setPixmap(mediocrePixmap);
		else if (congestionOverTime <= badThreshold)
			statusWidget->ui->statusIcon->setPixmap(badPixmap);

		firstCongestionUpdate = false;
	}
}

void OBSBasicStatusBar::OBSOutputReconnect(void *data, calldata_t *params)
{
	OBSBasicStatusBar *statusBar = static_cast<OBSBasicStatusBar *>(data);
	const int seconds = (int)calldata_int(params, "timeout_sec");

	/* The signal fires on the output's thread; the attempt counters, the labels and the tray live on the GUI
	 * thread. The handler argument is the status bar itself, which outlives every output, so nothing here
	 * points at a per-connection entry that could be removed in between. */
	QMetaObject::invokeMethod(
		statusBar, [statusBar, seconds] { statusBar->ReconnectPrimary(seconds); }, Qt::QueuedConnection);
}

void OBSBasicStatusBar::OBSOutputReconnectSuccess(void *data, calldata_t *)
{
	OBSBasicStatusBar *statusBar = static_cast<OBSBasicStatusBar *>(data);

	QMetaObject::invokeMethod(
		statusBar, [statusBar] { statusBar->ReconnectPrimarySuccess(); }, Qt::QueuedConnection);
}

void OBSBasicStatusBar::ReconnectPrimary(int seconds)
{
	MarkReconnecting(primaryConnectionId, seconds);
}

void OBSBasicStatusBar::ReconnectPrimarySuccess()
{
	MarkReconnected(primaryConnectionId);
}

void OBSBasicStatusBar::NoteStreamReconnect(const std::string &id, int seconds)
{
	MarkReconnecting(id, seconds);
}

void OBSBasicStatusBar::NoteStreamReconnectSuccess(const std::string &id)
{
	MarkReconnected(id);
}

void OBSBasicStatusBar::MarkReconnecting(const std::string &id, int seconds)
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	Connection *c = findConnection(id);
	if (!c && !primaryConnectionId.empty())
		c = findConnection(primaryConnectionId);
	if (!c) {
		/* The single-stream case with no session report yet still uses the old fields. */
		if (!retries)
			if (main)
				main->SysTrayNotify(QTStr("Basic.SystemTray.Message.Reconnecting"),
						    QSystemTrayIcon::Warning);
		reconnectTimeout = seconds;
		retries++;
		disconnected = true;
		statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
		return;
	}

	if (!c->retries)
		if (main)
			main->SysTrayNotify(QTStr("Basic.SystemTray.Message.Reconnecting"), QSystemTrayIcon::Warning);

	c->retries++;
	c->reconnectTimeout = seconds;

	if (OBSOutput output = c->output ? OBSGetStrongRef(c->output) : OBSOutput(); output) {
		delaySecTotal = obs_output_get_active_delay(output);
		UpdateDelayMsg();
	}

	disconnected = true;
	statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
	congestionArray.clear();

	ConnectionMessage();
}

void OBSBasicStatusBar::MarkReconnected(const std::string &id)
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	Connection *c = findConnection(id);
	if (!c && !primaryConnectionId.empty())
		c = findConnection(primaryConnectionId);
	if (!c) {
		ReconnectClear();
		return;
	}

	const QString msg = QTStr("Basic.StatusBar.ReconnectSuccessful");
	showMessage(msg, 4000);
	if (main)
		main->SysTrayNotify(msg, QSystemTrayIcon::Information);

	c->retries = 0;
	c->reconnectTimeout = 0;

	seconds = -1;
	lastBytesSent = 0;
	lastBytesSentTime = os_gettime_ns();

	disconnected = false;
	for (const auto &entry : connections) {
		if (entry.second.reconnectTimeout > 0) {
			disconnected = true;
			break;
		}
	}

	if (!disconnected) {
		firstCongestionUpdate = true;
		if (OBSOutput output = c->output ? OBSGetStrongRef(c->output) : OBSOutput(); output) {
			delaySecTotal = obs_output_get_active_delay(output);
			UpdateDelayMsg();
		}
	}

	ConnectionMessage();
}

void OBSBasicStatusBar::ReconnectClear()
{
	retries = 0;
	reconnectTimeout = 0;
	seconds = -1;
	lastBytesSent = 0;
	lastBytesSentTime = os_gettime_ns();
	delaySecTotal = 0;

	for (auto &entry : connections) {
		entry.second.retries = 0;
		entry.second.reconnectTimeout = 0;
	}

	UpdateDelayMsg();
}

void OBSBasicStatusBar::UpdateStatusBar()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	UpdateBandwidth();

	if (streamOutput)
		UpdateStreamTime();

	if (recordOutput)
		UpdateRecordTime();

	UpdateDroppedFrames();

	OBS::EncoderFrameCounters counters = activeEncoderFrameCounters();
	uint64_t skipped = counters.lagSkipped;
	uint64_t total = counters.delivered;

	/* An encoder re-init or a delivery-mix rebind resets these counters, so a
	 * value below the activation baseline means a new window started. */
	if (skipped < startSkippedFrameCount || total < startTotalFrameCount) {
		startSkippedFrameCount = skipped;
		startTotalFrameCount = total;
		lastSkippedFrameCount = 0;
	}

	skipped -= startSkippedFrameCount;
	total -= startTotalFrameCount;

	if (total > 0) {
		int diff = (int) (skipped - lastSkippedFrameCount);
		double percentage = double(skipped) / double(total) * 100.0;

		if (diff > 10 && percentage >= 0.1f) {
			showMessage(QTStr("HighResourceUsage"), 4000);
			if (!main->isVisible() && overloadedNotify) {
				main->SysTrayNotify(QTStr("HighResourceUsage"), QSystemTrayIcon::Warning);
				overloadedNotify = false;
			}
		}
	}

	lastSkippedFrameCount = skipped;
}

void OBSBasicStatusBar::StreamDelayStarting(int sec)
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main || !main->outputHandler)
		return;

	OBSOutputAutoRelease output = obs_frontend_get_streaming_output();
	streamOutput = OBSGetWeakRef(output);

	delaySecTotal = delaySecStarting = sec;
	UpdateDelayMsg();
	Activate();
}

void OBSBasicStatusBar::StreamDelayStopping(int sec)
{
	delaySecTotal = delaySecStopping = sec;
	UpdateDelayMsg();
}

void OBSBasicStatusBar::StreamStarted(obs_output_t *output)
{
	/* Reconcile with the session's connections: the first destination plus one output per extra destination
	 * (slice 2b). Going through that report is what makes "n of N up" and the per-destination reconnect text
	 * possible, and it is also how a restarted connection gets its signals attached again. */
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (main) {
		main->PushStreamConnections();
		if (!connections.empty()) {
			retries = 0;
			return;
		}
	}

	streamOutput = OBSGetWeakRef(output);

	streamSigs.emplace_back(obs_output_get_signal_handler(output), "reconnect", OBSOutputReconnect, this);
	streamSigs.emplace_back(obs_output_get_signal_handler(output), "reconnect_success", OBSOutputReconnectSuccess,
				this);

	retries = 0;
	lastBytesSent = 0;
	lastBytesSentTime = os_gettime_ns();
	Activate();
}

void OBSBasicStatusBar::StreamStopped()
{
	for (auto &entry : connections)
		entry.second.sigs.clear();
	connections.clear();
	primaryConnectionId.clear();

	if (streamOutput) {
		streamSigs.clear();

		ReconnectClear();
		streamOutput = nullptr;
		clearMessage();
		Deactivate();
	}
}

void OBSBasicStatusBar::RecordingStarted(obs_output_t *output)
{
	recordOutput = OBSGetWeakRef(output);
	Activate();
}

void OBSBasicStatusBar::RecordingStopped()
{
	recordOutput = nullptr;
	Deactivate();
}

void OBSBasicStatusBar::RecordingPaused()
{
	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingPausePixmap);
		streamPauseIconToggle = true;
	}

	UpdateRecordTimeLabel();
}

void OBSBasicStatusBar::RecordingUnpaused()
{
	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingActivePixmap);
	}

	UpdateRecordTimeLabel();
}

static QPixmap GetPixmap(const QString &filename)
{
	QString path = obs_frontend_is_theme_dark() ? "theme:Dark/" : ":/res/images/";
	return QIcon(path + filename).pixmap(QSize(16, 16));
}

void OBSBasicStatusBar::UpdateIcons()
{
	disconnectedPixmap = GetPixmap("network-disconnected.svg");
	inactivePixmap = GetPixmap("network-inactive.svg");

	streamingInactivePixmap = GetPixmap("streaming-inactive.svg");

	recordingInactivePixmap = GetPixmap("recording-inactive.svg");
	recordingPauseInactivePixmap = GetPixmap("recording-pause-inactive.svg");

	bool streaming = obs_frontend_streaming_active();

	if (!streaming) {
		statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	} else {
		if (disconnected)
			statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
	}

	bool recording = obs_frontend_recording_active();

	if (!recording)
		statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
}

void OBSBasicStatusBar::showMessage(const QString &message, int timeout)
{
	messageTimer->stop();

	statusWidget->ui->message->setText(message);

	if (timeout)
		messageTimer->start(timeout);
}

void OBSBasicStatusBar::clearMessage()
{
	statusWidget->ui->message->setText("");
}
