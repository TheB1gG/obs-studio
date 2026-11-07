#include "OBSBasicStats.hpp"

#include "OBSStatsRenditionInspector.hpp" /* EXPERIMENT (option G) - remove with that file */

#include <widgets/OBSBasic.hpp>
#include <utility/EncoderStats.hpp>

#include <qt-wrappers.hpp>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
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

/* Theme-aware icon, same arrangement as the status bar's icons: the dark theme ships its own copy of the
 * icons the main window uses (data/themes/Dark/...), the light build falls back to the compiled-in ones. */
static QIcon ThemedIcon(const QString &name)
{
	const QString prefix = obs_frontend_is_theme_dark() ? QStringLiteral("theme:Dark/")
							    : QStringLiteral(":/res/images/");
	return QIcon(prefix + name);
}

/* EXPERIMENT (option G): on while the layout is being evaluated. Turn it off without a
 * rebuild by writing "RenditionInspector=false" into the [Stats] section of the
 * configuration; removing the feature entirely is one revert (see OBSBasicStats.hpp). */
static bool RenditionInspectorEnabled()
{
	const char *value = config_get_string(OBSBasic::Get()->Config(), "Stats", "RenditionInspector");
	return !value || !*value || strcmp(value, "false") != 0;
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
	AddOutputLabels(QTStr("Basic.Stats.Output.Recording"), true);
	RebuildOutputGrid();

	/* --------------------------------------------- */

	QVBoxLayout *outputContainerLayout = new QVBoxLayout();
	outputContainerLayout->addLayout(outputLayout);

	/* EXPERIMENT (option G) begin */
	if (RenditionInspectorEnabled()) {
		renditionInspector = new OBSStatsRenditionInspector(this);
		outputContainerLayout->addWidget(renditionInspector.data());
	}
	/* EXPERIMENT (option G) end */

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

/* Rendition table columns. A section bar spans all of them, so the columns carry rendition
 * values only and one header row at the top names them for the whole table. */
enum StatsColumn {
	StatsColRendition, /* which rendition this row is                     */
	StatsColImpl,      /* what encodes it: implementation + codec         */
	StatsColDelivery,  /* size, rate and pixel format of the feed         */
	StatsColEncoded,   /* frames it produced                             */
	StatsColLag,       /* mix-level skips, encoding lag                   */
	StatsColDecimated, /* its own deliberate frame-rate-divisor skips      */
	StatsColBitrate,   /* its own bitstream bitrate                       */
	StatsColCount,
};

void OBSBasicStats::AddOutputLabels(QString name, bool recording)
{
	OutputLabels ol;
	ol.title = name;
	ol.recording = recording;
	outputLabels.push_back(ol);
}

/* One section per connection of the streaming session, plus the recording section. The section of a
 * destination keeps its counters when only its title or order changes; when its output object was replaced
 * (a restart), its rendition rows are dropped because the delivery-path baselines cannot be carried over to a
 * new connection. */
bool OBSBasicStats::SyncSections(const std::vector<SectionInfo> &session, obs_output_t *recOutput)
{
	bool changed = outputLabels.size() != static_cast<qsizetype>(session.size()) + 1;

	QList<OutputLabels> next;
	next.reserve(int(session.size()) + 1);

	for (const SectionInfo &info : session) {
		OutputLabels section;
		for (const OutputLabels &old : outputLabels) {
			if (!old.recording && old.id == info.id) {
				section = old;
				break;
			}
		}

		if (section.watched != info.output) {
			section.rows.clear();
			section.watched = info.output;
			changed = true;
		}

		section.id = info.id;
		section.title = info.title;
		section.recording = false;
		section.present = info.present;
		next.push_back(section);
	}

	for (const OutputLabels &old : outputLabels) {
		if (!old.recording)
			continue;
		OutputLabels section = old;
		section.present = recOutput != nullptr;
		if (section.watched != recOutput) {
			section.rows.clear();
			section.watched = recOutput;
			changed = true;
		}
		next.push_back(section);
		break;
	}

	outputLabels = next;
	return changed;
}

/* Create the whole table from the model: one header row naming the rendition columns, then
 * one section per running output - a bar spanning the table with that output's status and
 * totals, followed by one row per video encoder (rendition) of that output. Called only when
 * the structure changes (an output starts or stops, a track is added, ...); Update() writes
 * the text. All counters live in EncoderRow, not in the labels, so rebuilding the table
 * never resets a number. */
void OBSBasicStats::RebuildOutputGrid()
{
	while (QLayoutItem *item = outputLayout->takeAt(0)) {
		if (QWidget *widget = item->widget())
			delete widget;
		delete item;
	}

	int row = 0;

	struct ColHeader {
		const char *loc;
		const char *toolTip;
	};
	static constexpr ColHeader headers[] = {
		{"Basic.Stats.Rendition.Title", "Basic.Stats.Rendition.Title.ToolTip"},
		{"Basic.Stats.Rendition.Encoder", "Basic.Stats.Rendition.Encoder.ToolTip"},
		{"Basic.Stats.Rendition.Delivery", "Basic.Stats.Rendition.Delivery.ToolTip"},
		{"Basic.Stats.Rendition.Encoded", "Basic.Stats.Rendition.Encoded.ToolTip"},
		{"Basic.Stats.Rendition.Lag", "Basic.Stats.Rendition.Lag.ToolTip"},
		{"Basic.Stats.Rendition.Decimated", "Basic.Stats.Rendition.Decimated.ToolTip"},
		{"Basic.Stats.Rendition.Bitrate", "Basic.Stats.Rendition.Bitrate.ToolTip"},
	};
	for (int col = 0; col < StatsColCount; col++) {
		QLabel *label = new QLabel(QTStr(headers[col].loc), this);
		label->setStyleSheet("font-weight: bold");
		label->setToolTip(QTStr(headers[col].toolTip));
		bool right = col >= StatsColEncoded && col <= StatsColBitrate;
		label->setAlignment(right ? (Qt::AlignRight | Qt::AlignVCenter) : (Qt::AlignLeft | Qt::AlignVCenter));
		outputLayout->addWidget(label, row, col);
	}
	row++;

	for (OutputLabels &section : outputLabels) {
		/* A destination that is part of the session but not uploading gets a bar too: that is where its
		 * restart action lives. Its rendition rows stay hidden. */
		if (!section.active && !section.present)
			continue;

		/* The output itself: one bar spanning the table, with the status and the
		 * output-wide totals. Values stay separate labels so the status and the drop count
		 * can be coloured through setClasses() and the active theme. */
		QWidget *bar = new QWidget(this);
		QHBoxLayout *barLayout = new QHBoxLayout(bar);
		barLayout->setContentsMargins(0, 0, 0, 0);

		section.bar = bar;
		section.nameLabel = new QLabel(section.title, bar);
		section.nameLabel->setStyleSheet("font-weight: bold");
		section.statusLabel = new QLabel(bar);
		section.sentLabel = new QLabel(bar);
		section.sentLabel->setToolTip(QTStr("Basic.Stats.MegabytesSent"));
		section.bitrateLabel = new QLabel(bar);
		section.bitrateLabel->setToolTip(QTStr("Basic.Stats.Bitrate.Output.ToolTip"));
		section.droppedLabel = new QLabel(bar);
		section.droppedLabel->setToolTip(QTStr("Basic.Stats.DroppedFrames"));

		barLayout->addWidget(section.nameLabel.data());
		barLayout->addWidget(section.statusLabel.data());
		barLayout->addWidget(new QLabel(QStringLiteral("·"), bar));
		barLayout->addWidget(section.sentLabel.data());
		barLayout->addWidget(new QLabel(QStringLiteral("·"), bar));
		barLayout->addWidget(section.bitrateLabel.data());
		barLayout->addWidget(new QLabel(QStringLiteral("·"), bar));
		barLayout->addWidget(section.droppedLabel.data());
		barLayout->addStretch();

		/* The actions of one connection (Q13): stop or kill this destination while the others keep
		 * uploading, and bring back one that dropped. Only for the destinations of a session with more than
		 * the plain single stream - with one unnamed connection the main window's own Start/Stop and skull
		 * buttons are exactly these actions, so showing them twice would only invite confusion. */
		if (!section.recording && !section.id.empty()) {
			OBSBasic *main = OBSBasic::Get();
			const std::string id = section.id;

			auto addButton = [&](QPointer<QToolButton> &slot, const QString &icon, const char *textLoc,
					     const char *tipLoc, void (OBSBasic::*action)(std::string, bool)) {
				QToolButton *button = new QToolButton(bar);
				button->setIcon(ThemedIcon(icon));
				button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
				button->setText(QTStr(textLoc));
				button->setToolTip(QTStr(tipLoc));
				button->setAutoRaise(true);
				button->setStyleSheet("font-size: 9pt;");
				QObject::connect(button, &QToolButton::clicked, this,
						 [main, id, action] { (main->*action)(id, false); });
				barLayout->addWidget(button);
				slot = button;
			};

			addButton(section.stopButton, "media/media_stop.svg", "Basic.Stats.Bar.Stop.Text",
				  "Basic.Stats.Bar.Stop.ToolTip", &OBSBasic::StopDestination);

			/* The kill takes the force flag the stop does not: the same action, ending the connection
			 * without any end-of-stream data (Q41). */
			QToolButton *kill = new QToolButton(bar);
			kill->setIcon(ThemedIcon("settings/skull.svg"));
			kill->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
			kill->setText(QTStr("Basic.Stats.Bar.Kill.Text"));
			kill->setToolTip(QTStr("Basic.Stats.Bar.Kill.ToolTip"));
			kill->setAutoRaise(true);
			kill->setStyleSheet("font-size: 9pt;");
			QObject::connect(kill, &QToolButton::clicked, this,
					 [main, id] { main->KillDestinationUngracefully(id); });
			barLayout->addWidget(kill);
			section.killButton = kill;

			QToolButton *restart = new QToolButton(bar);
			restart->setIcon(ThemedIcon("media/media_restart.svg"));
			restart->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
			restart->setText(QTStr("Basic.Stats.Bar.Restart.Text"));
			restart->setToolTip(QTStr("Basic.Stats.Bar.Restart.ToolTip"));
			restart->setAutoRaise(true);
			restart->setStyleSheet("font-size: 9pt;");
			QObject::connect(restart, &QToolButton::clicked, this,
					 [main, id] { main->RestartDestination(id); });
			barLayout->addWidget(restart);
			section.restartButton = restart;
		}

		outputLayout->addWidget(bar, row++, 0, 1, StatsColCount);

		/* ... and one row per video encoder of that output. */
		for (EncoderRow &entry : section.rows) {
			entry.name = new QLabel(this);
			entry.impl = new QLabel(this);
			entry.delivery = new QLabel(this);
			entry.encoded = new QLabel(this);
			entry.lag = new QLabel(this);
			entry.decimated = new QLabel(this);
			entry.bitrate = new QLabel(this);

			for (QLabel *label : {entry.encoded.data(), entry.lag.data(), entry.decimated.data(), entry.bitrate.data()})
				label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

			outputLayout->addWidget(entry.name.data(), row, StatsColRendition);
			outputLayout->addWidget(entry.impl.data(), row, StatsColImpl);
			outputLayout->addWidget(entry.delivery.data(), row, StatsColDelivery);
			outputLayout->addWidget(entry.encoded.data(), row, StatsColEncoded);
			outputLayout->addWidget(entry.lag.data(), row, StatsColLag);
			outputLayout->addWidget(entry.decimated.data(), row, StatsColDecimated);
			outputLayout->addWidget(entry.bitrate.data(), row, StatsColBitrate);
			row++;
		}
	}

	/* Nothing running: say so instead of leaving an empty table (idle Stream/Recording
	 * rows would otherwise be the only content). */
	if (row == 1)
		outputLayout->addWidget(new QLabel(QTStr("Basic.Stats.Output.None"), this), row, 0, 1, StatsColCount);

	/* Hand the slack to the text columns so the numbers never get squeezed out of a narrow
	 * dock (the dock opens at 700 px by default, OBSBasic.cpp:308). */
	outputLayout->setColumnStretch(StatsColRendition, 3);
	outputLayout->setColumnStretch(StatsColImpl, 2);
	outputLayout->setColumnStretch(StatsColDelivery, 3);
	outputLayout->setColumnStretch(StatsColEncoded, 1);
	outputLayout->setColumnStretch(StatsColLag, 2);
	outputLayout->setColumnStretch(StatsColDecimated, 1);
	outputLayout->setColumnStretch(StatsColBitrate, 2);
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

	/* The session's connections: since slice 2b every destination of a session has its own output, and the
	 * legacy getter only reports the first one - a session whose first destination dropped but whose others
	 * still upload has nothing to do with "no stream output", so the tick is gated on the snapshot instead.
	 * includeStopped keeps the destinations that are part of the session but currently not uploading: that
	 * is where their restart action lives. */
	std::vector<OBSBasic::StreamSessionEntry> session = OBSBasic::Get()->StreamSessionSnapshot(true);
	obs_output_t *rec = recOutput;

	auto outputFor = [&](const OutputLabels &section) -> obs_output_t * {
		if (section.recording)
			return rec;
		for (const auto &entry : session) {
			if (entry.id == section.id)
				return entry.output;
		}
		return nullptr;
	};

	if (session.empty() && !recOutput)
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
	 * counters of every encoder feeding the active outputs instead - for a session
	 * with several destinations that is every connection, since they all attach to
	 * the same encoders and the sum counts each encoder and mix once. */
	std::vector<const obs_output_t *> counted;
	counted.reserve(session.size() + 1);
	for (const auto &entry : session)
		counted.push_back(entry.output);
	counted.push_back(recOutput);

	OBS::EncoderFrameCounters counters = OBS::outputFrameCounters(counted);
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
	std::vector<SectionInfo> sections;
	sections.reserve(session.size());
	for (const auto &entry : session) {
		SectionInfo info;
		info.id = entry.id;
		/* Named by the destination so its rows can be told apart; a single unnamed destination keeps the
		 * plain "Stream" title, which leaves the single-stream look unchanged. */
		info.title = entry.name.empty() ? QTStr("Basic.Stats.Output.Stream") : QT_UTF8(entry.name.c_str());
		info.output = entry.output;
		info.present = true;
		sections.push_back(info);
	}

	bool structureChanged = SyncSections(sections, rec);

	bool rowsChanged = false;
	for (OutputLabels &section : outputLabels)
		rowsChanged |= section.SyncRenditions(outputFor(section));

	if (structureChanged || rowsChanged)
		RebuildOutputGrid();

	for (OutputLabels &section : outputLabels)
		section.Update(outputFor(section), section.recording);

	MarkSharedMixes();
	for (OutputLabels &section : outputLabels)
		section.UpdateRenditions();

	/* EXPERIMENT (option G) begin */
	if (renditionInspector)
		renditionInspector->Update(strOutput, recOutput);
	/* EXPERIMENT (option G) end */

	if (obs_output_active(rec)) {
		for (const OutputLabels &section : outputLabels) {
			if (section.recording) {
				bitrates.push_back(section.kbps);
				break;
			}
		}
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

	OBSOutputAutoRelease recOutput = obs_frontend_get_recording_output();

	/* Re-baseline every section against the connection it is showing right now, the same way Update()
	 * resolves them: the first destination's output is not the whole session any more. */
	std::vector<OBSBasic::StreamSessionEntry> session = OBSBasic::Get()->StreamSessionSnapshot(true);
	obs_output_t *rec = recOutput;

	/* Drop the rendition rows as well: Update() re-creates them with baselines taken at
	 * the current counter values, which is what "reset" means for a delta-based display. */
	for (OutputLabels &section : outputLabels) {
		section.rows.clear();

		obs_output_t *out = nullptr;
		if (section.recording) {
			out = rec;
		} else {
			for (const auto &entry : session) {
				if (entry.id == section.id) {
					out = entry.output;
					break;
				}
			}
		}

		section.Reset(out);
	}

	Update();
}

void OBSBasicStats::OutputLabels::Update(obs_output_t *output, bool rec)
{
	/* Section not in the table (output idle) - nothing to write. */
	if (!statusLabel)
		return;

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
		} else if (present && !id.empty()) {
			/* Part of a session that is still running, but this connection is not uploading: either it
			 * never started or its attempts are over. The restart action below is what Q13 promises for
			 * this case. */
			str = QTStr("Basic.Stats.Status.NotUploading");
			styling = "text-warning";
		}
	}

	statusLabel->setText(str);
	setClasses(statusLabel, styling);

	/* The actions follow the state: while the connection is up it can be stopped or killed, once it is down
	 * only restarting it makes sense. */
	if (stopButton)
		stopButton->setVisible(active);
	if (killButton)
		killButton->setVisible(active);
	if (restartButton)
		restartButton->setVisible(!active && present);

	long double num = (long double)totalBytes / (1024.0l * 1024.0l);
	const char *unit = "MiB";
	if (num > 1024) {
		num /= 1024;
		unit = "GiB";
	}
	sentLabel->setText(QTStr("Basic.Stats.Bar.Sent.Text").arg(QString("%1 %2").arg(num, 0, 'f', 1).arg(unit)));

	num = kbps;
	unit = "kb/s";
	if (num >= 10'000) {
		num /= 1000;
		unit = "Mb/s";
	}
	bitrateLabel->setText(QString("%1 %2").arg(num, 0, 'f', 0).arg(unit));

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
		droppedLabel->setText(QTStr("Basic.Stats.Bar.Dropped.Text").arg(str));

		if (num > 5.0l)
			setClasses(droppedLabel, "text-danger");
		else if (num > 1.0l)
			setClasses(droppedLabel, "text-warning");
		else
			setClasses(droppedLabel, "");
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
	baseTime = timeNs;
	lastBytes = counters.encoded_bytes;
	lastBytesTime = timeNs;
}

bool OBSBasicStats::OutputLabels::SyncRenditions(obs_output_t *output)
{
	QVector<EncoderRow> next;

	/* The whole section (bar + rows) exists only while the output runs: the delivery-path
	 * counters restart with the first consumer on their mix, so numbers gathered from a
	 * stopped output cannot be related to whatever run follows - and an idle section is
	 * clutter when a single output can be several rows deep. */
	const bool running = output && obs_output_active(output);
	if (running) {
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

	bool changed = (running != active) || (next.size() != rows.size());
	active = running;

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
		const uint64_t offered = counters.frames_offered - entry.baseOffered;

		/* Frames per second this encoder is really getting, over the current run (re-armed
		 * on every re-bind). The delivery column carries the nominal rate; the gap between
		 * the two is what a stalling feed looks like. */
		const long double runSeconds = (long double)(curTime - entry.baseTime) / 1000000000.0l;
		const double measuredFps = runSeconds >= 0.01l ? (double) ((long double) offered / runSeconds) : 0.0;

		/* "multitrack video video encoder 0" -> "MT video enc 0". */
		entry.name->setText(QString::fromStdString(OBS::renditionName(entry.encoder)));
		/* What really encodes this rendition: implementation + codec ("QSV H264", "x264").
		 * The encoder type id and the object name stay in the tooltip. */
		entry.impl->setText(QString::fromStdString(OBS::encoderLabel(entry.encoder)));
		entry.impl->setToolTip(QString("%1\n%2").arg(obs_encoder_get_id(entry.encoder),
							     obs_encoder_get_name(entry.encoder)));

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

		/* Lag is a property of the delivery mix, shown as a share of the frames that mix
		 * pushed - the same basis as libobs' own log line "skipped frames due to encoding
		 * lag: X/Y (Z%)". Decimation is this encoder's own deliberate rate reduction and
		 * gets its own column, so neither can be mistaken for the other (bug 1). */
		long double pct = mixFrames ? (long double)lagSkipped / (long double)mixFrames * 100.0l : 0.0l;
		entry.lag->setText(QString("%1 (%2%)").arg(QString::number(lagSkipped), QString::number(pct, 'f', 1)));
		if (pct > 5.0l)
			setClasses(entry.lag, "text-danger");
		else if (pct > 1.0l)
			setClasses(entry.lag, "text-warning");
		else
			setClasses(entry.lag, "");

		entry.decimated->setText(QString::number(decimated));

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
			QString("offered %1 (%2 fps), encoded %3, decimated %4, lag-skipped %5, mix frames %6")
				.arg(QString::number(offered), QString::number(measuredFps, 'f', 1), QString::number(encoded),
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
