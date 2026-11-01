#include "OBSBasicStats.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/EncoderStats.hpp>

#include <qt-wrappers.hpp>

#include <cstdint>
#include <utility>
#include <vector>

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "moc_OBSBasicStats.cpp"

#define TIMER_INTERVAL 2000
#define REC_TIME_LEFT_INTERVAL 30000

void OBSBasicStats::OBSFrontendEvent(enum obs_frontend_event event, void *ptr)
{
	OBSBasicStats *stats = static_cast<OBSBasicStats *>(ptr);

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		stats->StartRecTimeLeft();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		stats->ResetRecTimeLeft();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// This is only reached when the non-closable (dock) stats
		// window is being cleaned up. The closable stats window is
		// already gone by this point as it's deleted on close.
		obs_frontend_remove_event_callback(OBSFrontendEvent, stats);
		break;
	default:
		break;
	}
}

static QString MakeTimeLeftText(int hours, int minutes)
{
	return QTStr("Basic.Stats.DiskFullIn.Text").arg(QString::number(hours), QString::number(minutes));
}

static QString MakeMissedFramesText(uint32_t total_lagged, uint32_t total_rendered, long double num)
{
	return QString("%1 / %2 (%3%)")
		.arg(QString::number(total_lagged), QString::number(total_rendered), QString::number(num, 'f', 1));
}

OBSBasicStats::OBSBasicStats(QWidget *parent, bool closable)
	: QFrame(parent),
	  cpu_info(os_cpu_usage_info_start()),
	  timer(this),
	  recTimeLeft(this)
{
	QVBoxLayout *mainLayout = new QVBoxLayout();
	QGridLayout *topLayout = new QGridLayout();
	outputLayout = new QGridLayout();

	bitrates.reserve(REC_TIME_LEFT_INTERVAL / TIMER_INTERVAL);

	int row = 0;

	auto newStatBare = [&](QString name, QWidget *label, int col) {
		QLabel *typeLabel = new QLabel(name, this);
		topLayout->addWidget(typeLabel, row, col);
		topLayout->addWidget(label, row++, col + 1);
	};

	auto newStat = [&](const char *strLoc, QWidget *label, int col) {
		std::string str = "Basic.Stats.";
		str += strLoc;
		newStatBare(QTStr(str.c_str()), label, col);
	};

	/* --------------------------------------------- */

	cpuUsage = new QLabel(this);
	hddSpace = new QLabel(this);
	recordTimeLeft = new QLabel(this);
	memUsage = new QLabel(this);

	QString str = MakeTimeLeftText(99999, 59);
	int textWidth = recordTimeLeft->fontMetrics().boundingRect(str).width();
	recordTimeLeft->setMinimumWidth(textWidth);

	newStat("CPUUsage", cpuUsage, 0);
	newStat("HDDSpaceAvailable", hddSpace, 0);
	newStat("DiskFullIn", recordTimeLeft, 0);
	newStat("MemoryUsage", memUsage, 0);

	fps = new QLabel(this);
	renderTime = new QLabel(this);
	skippedFrames = new QLabel(this);
	missedFrames = new QLabel(this);

	str = MakeMissedFramesText(999999, 999999, 99.99);
	textWidth = missedFrames->fontMetrics().boundingRect(str).width();
	missedFrames->setMinimumWidth(textWidth);

	row = 0;

	newStatBare("FPS", fps, 2);
	newStat("AverageTimeToRender", renderTime, 2);
	newStat("MissedFrames", missedFrames, 2);
	newStat("SkippedFrames", skippedFrames, 2);

	/* --------------------------------------------- */
	QPushButton *closeButton = nullptr;
	if (closable)
		closeButton = new QPushButton(QTStr("Close"));
	QPushButton *resetButton = new QPushButton(QTStr("Reset"));
	QHBoxLayout *buttonLayout = new QHBoxLayout;
	buttonLayout->addStretch();
	buttonLayout->addWidget(resetButton);
	if (closable)
		buttonLayout->addWidget(closeButton);

	/* --------------------------------------------- */

	AddOutputLabels(QTStr("Basic.Stats.Output.Stream"));
	AddOutputLabels(QTStr("Basic.Stats.Output.Recording"));
	RebuildOutputGrid();

	/* --------------------------------------------- */

	QVBoxLayout *outputContainerLayout = new QVBoxLayout();
	outputContainerLayout->addLayout(outputLayout);
	outputContainerLayout->addStretch();

	QWidget *widget = new QWidget(this);
	widget->setLayout(outputContainerLayout);

	QScrollArea *scrollArea = new QScrollArea(this);
	scrollArea->setWidget(widget);
	scrollArea->setWidgetResizable(true);

	/* --------------------------------------------- */

	mainLayout->addLayout(topLayout);
	mainLayout->addWidget(scrollArea);
	mainLayout->addLayout(buttonLayout);
	setLayout(mainLayout);

	/* --------------------------------------------- */
	if (closable)
		connect(closeButton, &QPushButton::clicked, [this]() { close(); });
	connect(resetButton, &QPushButton::clicked, [this]() { Reset(); });

	delete shortcutFilter;
	shortcutFilter = CreateShortcutFilter();
	installEventFilter(shortcutFilter);

	resize(800, 280);

	setWindowTitle(QTStr("Basic.Stats"));
#ifdef __APPLE__
	setWindowIcon(QIcon::fromTheme("obs", QIcon(":/res/images/obs_256x256.png")));
#else
	setWindowIcon(QIcon::fromTheme("obs", QIcon(":/res/images/obs.png")));
#endif

	setWindowModality(Qt::NonModal);
	setAttribute(Qt::WA_DeleteOnClose, true);

	QObject::connect(&timer, &QTimer::timeout, this, &OBSBasicStats::Update);
	timer.setInterval(TIMER_INTERVAL);

	if (isVisible())
		timer.start();

	Update();

	QObject::connect(&recTimeLeft, &QTimer::timeout, this, &OBSBasicStats::RecordingTimeLeft);
	recTimeLeft.setInterval(REC_TIME_LEFT_INTERVAL);

	OBSBasic *main = OBSBasic::Get();

	const char *geometry = config_get_string(main->Config(), "Stats", "geometry");
	if (geometry != NULL) {
		QByteArray byteArray = QByteArray::fromBase64(QByteArray(geometry));
		restoreGeometry(byteArray);

		QRect windowGeometry = normalGeometry();
		if (!WindowPositionValid(windowGeometry)) {
			QRect rect = QGuiApplication::primaryScreen()->geometry();
			setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size(), rect));
		}
	}

	obs_frontend_add_event_callback(OBSFrontendEvent, this);

	if (obs_frontend_recording_active())
		StartRecTimeLeft();
}

void OBSBasicStats::closeEvent(QCloseEvent *event)
{
	OBSBasic *main = OBSBasic::Get();
	if (isVisible()) {
		config_set_string(main->Config(), "Stats", "geometry", saveGeometry().toBase64().constData());
		config_save_safe(main->Config(), "tmp", nullptr);
	}

	// This code is only reached when the non-dockable stats window is
	// manually closed or OBS is exiting.
	obs_frontend_remove_event_callback(OBSFrontendEvent, this);

	QWidget::closeEvent(event);
}

OBSBasicStats::~OBSBasicStats()
{
	delete shortcutFilter;
	os_cpu_usage_info_destroy(cpu_info);
}

/* Columns of the output table. An output row and the rendition rows below it share the
 * columns but put different things in them (see RebuildOutputGrid()), which is why every
 * rendition block carries its own (italic) header row. */
enum StatsColumn {
	StatsColName,      /* output name                / encoder name              */
	StatsColStatus,    /* output status              / codec                     */
	StatsColDropped,   /* frames dropped by network  / delivery size, rate, format */
	StatsColSent,      /* bytes written to the output / frames encoded            */
	StatsColSkipped,   /* (unused)                    / lag skips and decimation  */
	StatsColBitrate,   /* output bitrate              / encoder bitstream bitrate */
	StatsColCount,
};

void OBSBasicStats::AddOutputLabels(QString name)
{
	OutputLabels ol;
	ol.title = name;
	outputLabels.push_back(ol);
}

/* Create every label of the output table from the current model: the header row, one row
 * per output, and below each output one header + one row per video encoder (rendition) of
 * that output. Called only when the structure changes (an output starts or stops, a track
 * is added, ...); Update() writes the text. All counters live in EncoderRow, not in the
 * labels, so rebuilding the table never resets a number. */
void OBSBasicStats::RebuildOutputGrid()
{
	while (QLayoutItem *item = outputLayout->takeAt(0)) {
		if (QWidget *widget = item->widget())
			delete widget;
		delete item;
	}

	int row = 0;

	const char *outputHeaders[] = {
		"Basic.Settings.Output",
		"Basic.Stats.Status",
		"Basic.Stats.DroppedFrames",
		"Basic.Stats.MegabytesSent",
		nullptr,
		"Basic.Stats.Bitrate",
	};
	for (int col = 0; col < StatsColCount; col++) {
		if (!outputHeaders[col])
			continue;
		QLabel *label = new QLabel(QTStr(outputHeaders[col]), this);
		label->setStyleSheet("font-weight: bold");
		outputLayout->addWidget(label, row, col);
	}
	row++;

	for (OutputLabels &section : outputLabels) {
		section.name = new QLabel(section.title, this);
		section.status = new QLabel(this);
		section.droppedFrames = new QLabel(this);
		section.megabytesSent = new QLabel(this);
		section.bitrate = new QLabel(this);

		outputLayout->addWidget(section.name.data(), row, StatsColName);
		outputLayout->addWidget(section.status.data(), row, StatsColStatus);
		outputLayout->addWidget(section.droppedFrames.data(), row, StatsColDropped);
		outputLayout->addWidget(section.megabytesSent.data(), row, StatsColSent);
		outputLayout->addWidget(section.bitrate.data(), row, StatsColBitrate);
		row++;

		if (section.rows.isEmpty())
			continue;

		struct SubHeader {
			const char *loc;
			const char *toolTip;
		};
		const SubHeader subHeaders[] = {
			{"Basic.Stats.Rendition.Title", nullptr},
			{"Basic.Stats.Rendition.Encoder", nullptr},
			{"Basic.Stats.Rendition.Delivery", "Basic.Stats.Rendition.Delivery.ToolTip"},
			{"Basic.Stats.Rendition.Encoded", "Basic.Stats.Rendition.Encoded.ToolTip"},
			{"Basic.Stats.Rendition.Skipped", "Basic.Stats.Rendition.Skipped.ToolTip"},
			{"Basic.Stats.Bitrate", "Basic.Stats.Rendition.Bitrate.ToolTip"},
		};
		for (int col = 0; col < StatsColCount; col++) {
			QLabel *label = new QLabel(QTStr(subHeaders[col].loc), this);
			label->setStyleSheet("font-style: italic");
			if (subHeaders[col].toolTip)
				label->setToolTip(QTStr(subHeaders[col].toolTip));
			outputLayout->addWidget(label, row, col);
		}
		row++;

		for (EncoderRow &entry : section.rows) {
			entry.name = new QLabel(this);
			entry.codec = new QLabel(this);
			entry.delivery = new QLabel(this);
			entry.encoded = new QLabel(this);
			entry.skipped = new QLabel(this);
			entry.bitrate = new QLabel(this);

			outputLayout->addWidget(entry.name.data(), row, StatsColName);
			outputLayout->addWidget(entry.codec.data(), row, StatsColStatus);
			outputLayout->addWidget(entry.delivery.data(), row, StatsColDropped);
			outputLayout->addWidget(entry.encoded.data(), row, StatsColSent);
			outputLayout->addWidget(entry.skipped.data(), row, StatsColSkipped);
			outputLayout->addWidget(entry.bitrate.data(), row, StatsColBitrate);
			row++;
		}
	}
}

/* Encoder-mix reuse (acquire_encoder_only_mix() in libobs/obs-encoder.c) can feed several
 * renditions from one delivery mix, and the mix-level counters (frames delivered to the mix,
 * lag skips) then belong to all of them at once. Flag those rows so the row can say so
 * instead of implying the number is exclusive to it. */
void OBSBasicStats::MarkSharedMixes()
{
	std::vector<std::pair<const video_t *, EncoderRow *>> refs;

	for (OutputLabels &section : outputLabels) {
		for (EncoderRow &entry : section.rows) {
			const video_t *mix = OBS::deliveryMix(entry.encoder);
			entry.sharedMix = false;
			if (mix)
				refs.emplace_back(mix, &entry);
		}
	}

	for (auto &ref : refs) {
		size_t users = 0;
		for (const auto &other : refs) {
			if (other.first == ref.first)
				users++;
		}
		ref.second->sharedMix = users > 1;
	}
}

/* Delivery-counter baselines of the encoders feeding the active outputs
 * (see <utility/EncoderStats.hpp>). They are re-armed whenever a value
 * decreases, which is how an encoder re-init or a delivery-mix rebind
 * (both reset the counters) is detected. */
static uint64_t first_delivered = UINT64_MAX;
static uint64_t first_lag_skipped = UINT64_MAX;
static uint32_t first_rendered = 0xFFFFFFFF;
static uint32_t first_lagged = 0xFFFFFFFF;

void OBSBasicStats::InitializeValues()
{
	/* No absolute baseline here: the encoder-delivery counters restart when an encoder
	 * attaches to its delivery mix, so any value read before the outputs are up would be
	 * meaningless. The first Update() tick re-arms the baselines instead. (Frontend
	 * output getters must not be used here: ResetVideo() runs before the output handler
	 * exists, and obs_frontend_get_streaming_output() dereferences it.) */
	first_delivered = UINT64_MAX;
	first_lag_skipped = UINT64_MAX;
	first_rendered = obs_get_total_frames();
	first_lagged = obs_get_lagged_frames();
}

void OBSBasicStats::Update()
{
	OBSBasic *main = OBSBasic::Get();

	/* TODO: Un-hardcode */

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);

	OBSOutputAutoRelease strOutput = obs_frontend_get_streaming_output();
	OBSOutputAutoRelease recOutput = obs_frontend_get_recording_output();

	if (!strOutput && !recOutput)
		return;

	/* ------------------------------------------- */
	/* general usage                               */

	double curFPS = obs_get_active_fps();
	double obsFPS = (double)ovi.fps_num / (double)ovi.fps_den;

	QString str = QString::number(curFPS, 'f', 2);
	fps->setText(str);

	if (curFPS < (obsFPS * 0.8))
		setClasses(fps, "text-danger");
	else if (curFPS < (obsFPS * 0.95))
		setClasses(fps, "text-warning");
	else
		setClasses(fps, "");

	/* ------------------ */

	double usage = os_cpu_usage_info_query(cpu_info);
	str = QString::number(usage, 'g', 2) + QStringLiteral("%");
	cpuUsage->setText(str);

	/* ------------------ */

	const char *path = main->GetCurrentOutputPath();

#define MBYTE (1024ULL * 1024ULL)
#define GBYTE (1024ULL * 1024ULL * 1024ULL)
#define TBYTE (1024ULL * 1024ULL * 1024ULL * 1024ULL)
	num_bytes = os_get_free_disk_space(path);
	QString abrv = QStringLiteral(" MB");
	long double num;

	num = (long double)num_bytes / (1024.0l * 1024.0l);
	if (num_bytes > TBYTE) {
		num /= 1024.0l * 1024.0l;
		abrv = QStringLiteral(" TB");
	} else if (num_bytes > GBYTE) {
		num /= 1024.0l;
		abrv = QStringLiteral(" GB");
	}

	str = QString::number(num, 'f', 1) + abrv;
	hddSpace->setText(str);

	if (num_bytes < GBYTE)
		setClasses(hddSpace, "text-danger");
	else if (num_bytes < (5 * GBYTE))
		setClasses(hddSpace, "text-warning");
	else
		setClasses(hddSpace, "");

	/* ------------------ */

	num = (long double)os_get_proc_resident_size() / (1024.0l * 1024.0l);

	str = QString::number(num, 'f', 1) + QStringLiteral(" MB");
	memUsage->setText(str);

	/* ------------------ */

	num = (long double)obs_get_average_frame_time_ns() / 1000000.0l;

	str = QString::number(num, 'f', 1) + QStringLiteral(" ms");
	renderTime->setText(str);

	long double fpsFrameTime = (long double)ovi.fps_den * 1000.0l / (long double)ovi.fps_num;

	if (num > fpsFrameTime)
		setClasses(renderTime, "text-danger");
	else if (num > fpsFrameTime * 0.75l)
		setClasses(renderTime, "text-warning");
	else
		setClasses(renderTime, "");

	/* ------------------ */

	/* Encoding lag belongs to the delivery mix of each encoder, not to the base
	 * canvas: with the high-fidelity master canvas the base canvas mix has no
	 * consumers of its own, so obs_get_video() would report 0/0 forever. Sum the
	 * counters of every encoder feeding the active outputs instead. */
	OBS::EncoderFrameCounters counters = OBS::outputFrameCounters({strOutput, recOutput});
	uint64_t total_encoded = counters.delivered;
	uint64_t total_skipped = counters.lagSkipped;

	if (total_encoded < first_delivered || total_skipped < first_lag_skipped) {
		first_delivered = total_encoded;
		first_lag_skipped = total_skipped;
	}
	total_encoded -= first_delivered;
	total_skipped -= first_lag_skipped;

	num = total_encoded ? (long double)total_skipped / (long double)total_encoded : 0.0l;
	num *= 100.0l;

	str = QString("%1 / %2 (%3%)")
		      .arg(QString::number(total_skipped), QString::number(total_encoded),
			   QString::number(num, 'f', 1));
	skippedFrames->setText(str);

	if (num > 5.0l)
		setClasses(skippedFrames, "text-danger");
	else if (num > 1.0l)
		setClasses(skippedFrames, "text-warning");
	else
		setClasses(skippedFrames, "");

	/* ------------------ */

	uint32_t total_rendered = obs_get_total_frames();
	uint32_t total_lagged = obs_get_lagged_frames();

	if (total_rendered < first_rendered || total_lagged < first_lagged) {
		first_rendered = total_rendered;
		first_lagged = total_lagged;
	}
	total_rendered -= first_rendered;
	total_lagged -= first_lagged;

	num = total_rendered ? (long double)total_lagged / (long double)total_rendered : 0.0l;
	num *= 100.0l;

	str = MakeMissedFramesText(total_lagged, total_rendered, num);
	missedFrames->setText(str);

	if (num > 5.0l)
		setClasses(missedFrames, "text-danger");
	else if (num > 1.0l)
		setClasses(missedFrames, "text-warning");
	else
		setClasses(missedFrames, "");

	/* ------------------------------------------- */
	/* recording/streaming stats                   */

	/* Per-rendition rows first: one row per video encoder of each active output (a
	 * multitrack stream therefore gets one row per track). The row list is re-derived from
	 * the outputs on every tick - an active output holds strong references to its
	 * encoders, so those pointers stay valid while the output runs - and the table is laid
	 * out again only when the set of rows really changed. Laying out re-creates the section
	 * labels as well, which is why the output row text is written after it. */
	bool rowsChanged = outputLabels[0].SyncRenditions(strOutput);
	rowsChanged |= outputLabels[1].SyncRenditions(recOutput);
	if (rowsChanged)
		RebuildOutputGrid();

	outputLabels[0].Update(strOutput, false);
	outputLabels[1].Update(recOutput, true);

	MarkSharedMixes();
	outputLabels[0].UpdateRenditions();
	outputLabels[1].UpdateRenditions();

	if (obs_output_active(recOutput)) {
		long double kbps = outputLabels[1].kbps;
		bitrates.push_back(kbps);
	}
}

void OBSBasicStats::StartRecTimeLeft()
{
	if (recTimeLeft.isActive())
		ResetRecTimeLeft();

	recordTimeLeft->setText(QTStr("Calculating"));
	recTimeLeft.start();
}

void OBSBasicStats::ResetRecTimeLeft()
{
	if (recTimeLeft.isActive()) {
		bitrates.clear();
		recTimeLeft.stop();
		recordTimeLeft->setText(QTStr(""));
	}
}

void OBSBasicStats::RecordingTimeLeft()
{
	if (bitrates.empty())
		return;

	long double averageBitrate = accumulate(bitrates.begin(), bitrates.end(), 0.0) / (long double)bitrates.size();
	if (averageBitrate == 0)
		return;

	long double bytesPerSec = (averageBitrate / 8.0l) * 1000.0l;
	long double secondsUntilFull = (long double)num_bytes / bytesPerSec;

	bitrates.clear();

	int totalMinutes = (int)secondsUntilFull / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString text = MakeTimeLeftText(hours, minutes);
	recordTimeLeft->setText(text);
	recordTimeLeft->setMinimumWidth(recordTimeLeft->width());
}

void OBSBasicStats::Reset()
{
	timer.start();

	first_delivered = UINT64_MAX;
	first_lag_skipped = UINT64_MAX;
	first_rendered = 0xFFFFFFFF;
	first_lagged = 0xFFFFFFFF;

	OBSOutputAutoRelease strOutput = obs_frontend_get_streaming_output();
	OBSOutputAutoRelease recOutput = obs_frontend_get_recording_output();

	/* Drop the rendition rows as well: Update() re-creates them with baselines taken at
	 * the current counter values, which is what "reset" means for a delta-based display. */
	for (OutputLabels &section : outputLabels)
		section.rows.clear();

	outputLabels[0].Reset(strOutput);
	outputLabels[1].Reset(recOutput);
	Update();
}

void OBSBasicStats::OutputLabels::Update(obs_output_t *output, bool rec)
{
	uint64_t totalBytes = output ? obs_output_get_total_bytes(output) : 0;
	uint64_t curTime = os_gettime_ns();
	uint64_t bytesSent = totalBytes;

	if (bytesSent < lastBytesSent)
		bytesSent = 0;
	if (bytesSent == 0)
		lastBytesSent = 0;

	uint64_t bitsBetween = (bytesSent - lastBytesSent) * 8;
	long double timePassed = (long double)(curTime - lastBytesSentTime) / 1000000000.0l;
	kbps = (long double)bitsBetween / timePassed / 1000.0l;

	if (timePassed < 0.01l)
		kbps = 0.0l;

	QString str = QTStr("Basic.Stats.Status.Inactive");
	QString styling;
	bool active = output ? obs_output_active(output) : false;
	if (rec) {
		if (active)
			str = QTStr("Basic.Stats.Status.Recording");
	} else {
		if (active) {
			bool reconnecting = output ? obs_output_reconnecting(output) : false;

			if (reconnecting) {
				str = QTStr("Basic.Stats.Status.Reconnecting");
				styling = "text-danger";
			} else {
				str = QTStr("Basic.Stats.Status.Live");
				styling = "text-success";
			}
		}
	}

	status->setText(str);
	setClasses(status, styling);

	long double num = (long double)totalBytes / (1024.0l * 1024.0l);
	const char *unit = "MiB";
	if (num > 1024) {
		num /= 1024;
		unit = "GiB";
	}
	megabytesSent->setText(QString("%1 %2").arg(num, 0, 'f', 1).arg(unit));

	num = kbps;
	unit = "kb/s";
	if (num >= 10'000) {
		num /= 1000;
		unit = "Mb/s";
	}
	bitrate->setText(QString("%1 %2").arg(num, 0, 'f', 0).arg(unit));

	if (!rec) {
		int total = output ? obs_output_get_total_frames(output) : 0;
		int dropped = output ? obs_output_get_frames_dropped(output) : 0;

		if (total < first_total || dropped < first_dropped) {
			first_total = 0;
			first_dropped = 0;
		}

		total -= first_total;
		dropped -= first_dropped;

		num = total ? (long double)dropped / (long double)total * 100.0l : 0.0l;

		str = QString("%1 / %2 (%3%)")
			      .arg(QString::number(dropped), QString::number(total), QString::number(num, 'f', 1));
		droppedFrames->setText(str);

		if (num > 5.0l)
			setClasses(droppedFrames, "text-danger");
		else if (num > 1.0l)
			setClasses(droppedFrames, "text-warning");
		else
			setClasses(droppedFrames, "");
	}

	lastBytesSent = bytesSent;
	lastBytesSentTime = curTime;
}

void OBSBasicStats::OutputLabels::Reset(obs_output_t *output)
{
	if (!output)
		return;

	first_total = obs_output_get_total_frames(output);
	first_dropped = obs_output_get_frames_dropped(output);
}

void OBSBasicStats::EncoderRow::Rebase(const obs_encoder_frame_counters &counters, const video_t *mixPtr,
				       uint64_t timeNs)
{
	mix = mixPtr;
	baseMixFrames = counters.mix_frames;
	baseMixLagSkipped = counters.mix_lag_skipped;
	baseOffered = counters.frames_offered;
	baseDecimated = counters.frames_decimated;
	baseEncoded = counters.frames_encoded;
	baseBytes = counters.encoded_bytes;
	lastBytes = counters.encoded_bytes;
	lastBytesTime = timeNs;
}

bool OBSBasicStats::OutputLabels::SyncRenditions(obs_output_t *output)
{
	QVector<EncoderRow> next;

	/* Rows exist only while the output is active. The delivery-path counters restart with
	 * the first consumer on their mix, so numbers gathered from a stopped output cannot be
	 * related to whatever run follows. */
	if (output && obs_output_active(output)) {
		for (const OBS::EncoderSlot &slot : OBS::outputVideoEncoders(output)) {
			EncoderRow entry;

			for (const EncoderRow &existing : rows) {
				if (existing.encoder != slot.encoder)
					continue;
				/* Same encoder object - it survives an encoder re-init and a live
				 * rescale - so carry the baselines over and keep counting up. */
				entry = existing;
				break;
			}

			if (entry.encoder != slot.encoder) {
				/* First sighting of this encoder: baseline at the current values
				 * rather than at zero. libobs does not reset the per-encoder frame
				 * and byte counters when an encoder restarts, so an absolute read
				 * would otherwise show everything this encoder ever produced. */
				entry.encoder = slot.encoder;
				entry.Rebase(OBS::encoderCounters(slot.encoder), OBS::deliveryMix(slot.encoder),
					     os_gettime_ns());
			}

			next.push_back(entry);
		}
	}

	bool changed = next.size() != rows.size();
	if (!changed) {
		for (qsizetype i = 0; i < next.size(); i++) {
			if (next[i].encoder == rows[i].encoder)
				continue;
			changed = true;
			break;
		}
	}

	rows = std::move(next);
	return changed;
}

void OBSBasicStats::OutputLabels::UpdateRenditions()
{
	const uint64_t curTime = os_gettime_ns();

	for (EncoderRow &entry : rows) {
		if (!entry.encoder || !entry.encoded)
			continue;

		const obs_encoder_frame_counters counters = OBS::encoderCounters(entry.encoder);
		const video_t *mix = OBS::deliveryMix(entry.encoder);

		/* A live rescale or an encoder re-init swaps the delivery mix and restarts the
		 * counters (video_output_connect2() -> reset_frames()). Re-arm the row then,
		 * otherwise the unsigned deltas wrap around and the row goes mad. */
		if (mix != entry.mix || counters.mix_frames < entry.baseMixFrames ||
		    counters.mix_lag_skipped < entry.baseMixLagSkipped || counters.frames_offered < entry.baseOffered ||
		    counters.frames_decimated < entry.baseDecimated || counters.frames_encoded < entry.baseEncoded ||
		    counters.encoded_bytes < entry.baseBytes)
			entry.Rebase(counters, mix, curTime);

		const uint64_t mixFrames = counters.mix_frames - entry.baseMixFrames;
		const uint64_t lagSkipped = counters.mix_lag_skipped - entry.baseMixLagSkipped;
		const uint64_t decimated = counters.frames_decimated - entry.baseDecimated;
		const uint64_t encoded = counters.frames_encoded - entry.baseEncoded;

		entry.name->setText(obs_encoder_get_name(entry.encoder));
		entry.codec->setText(obs_encoder_get_codec(entry.encoder));

		/* What this encoder is actually fed: the mix's size and pixel format plus the
		 * encoder's own output rate (already divided by its frame-rate divisor). */
		QString delivery;
		if (mix) {
			delivery = QString("%1x%2 %3 %4")
					   .arg(video_output_get_width(mix))
					   .arg(video_output_get_height(mix))
					   .arg(QString::number(obs_encoder_get_effective_fps(entry.encoder), 'f', 2))
					   .arg(get_video_format_name(video_output_get_format(mix)));
			if (entry.sharedMix)
				delivery += QStringLiteral(" ") + QTStr("Basic.Stats.Rendition.SharedMix");
		}
		entry.delivery->setText(delivery);

		entry.encoded->setText(QString::number(encoded));

		/* Lag belongs to the mix (shown as a share of the frames that mix pushed),
		 * decimation to this encoder alone: keep them in separate halves of the cell so an
		 * intentional rate reduction is never read as a performance problem, or the other
		 * way round (bug 1 in the review report). */
		long double pct = mixFrames ? (long double)lagSkipped / (long double)mixFrames * 100.0l : 0.0l;
		entry.skipped->setText(QString("%1 (%2%) / %3")
					       .arg(QString::number(lagSkipped), QString::number(pct, 'f', 1),
						    QString::number(decimated)));
		if (pct > 5.0l)
			setClasses(entry.skipped, "text-danger");
		else if (pct > 1.0l)
			setClasses(entry.skipped, "text-warning");
		else
			setClasses(entry.skipped, "");

		/* This encoder's own bitstream bitrate. The output-level Bitrate column is derived
		 * from obs_output_get_total_bytes(), which covers the whole output (every
		 * rendition, the audio, the container overhead) - meaningless per rendition. */
		uint64_t bytes = counters.encoded_bytes;
		if (bytes < entry.lastBytes)
			bytes = entry.lastBytes;
		long double seconds = (long double)(curTime - entry.lastBytesTime) / 1000000000.0l;
		entry.kbps = seconds >= 0.01l ? (long double)(bytes - entry.lastBytes) * 8.0l / seconds / 1000.0l
					      : 0.0l;
		entry.lastBytes = bytes;
		entry.lastBytesTime = curTime;

		long double num = entry.kbps;
		const char *unit = "kb/s";
		if (num >= 10'000) {
			num /= 1000;
			unit = "Mb/s";
		}
		entry.bitrate->setText(QString("%1 %2").arg(num, 0, 'f', 0).arg(unit));

		/* Everything the columns split up, in one place, for the hover. */
		entry.name->setToolTip(
			QString("offered %1, encoded %2, decimated %3, lag-skipped %4, mix frames %5")
				.arg(QString::number(counters.frames_offered - entry.baseOffered), QString::number(encoded),
				     QString::number(decimated), QString::number(lagSkipped), QString::number(mixFrames)));
	}
}

void OBSBasicStats::showEvent(QShowEvent *)
{
	timer.start(TIMER_INTERVAL);
}

void OBSBasicStats::hideEvent(QHideEvent *)
{
	timer.stop();
}
