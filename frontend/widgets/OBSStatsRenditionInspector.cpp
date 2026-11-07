#include "OBSStatsRenditionInspector.hpp"

#include <utility/EncoderStats.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>
#include <util/platform.h>

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cstdint>
#include <cstring>

static QString ColorSpaceName(enum video_colorspace space)
{
	switch (space) {
	case VIDEO_CS_601:
		return QStringLiteral("BT.601");
	case VIDEO_CS_709:
		return QStringLiteral("BT.709");
	case VIDEO_CS_SRGB:
		return QStringLiteral("sRGB");
	case VIDEO_CS_2100_PQ:
		return QStringLiteral("BT.2100 PQ");
	case VIDEO_CS_2100_HLG:
		return QStringLiteral("BT.2100 HLG");
	default:
		return QStringLiteral("default");
	}
}

static QString RangeName(enum video_range_type range)
{
	return range == VIDEO_RANGE_FULL ? QStringLiteral("full range") : QStringLiteral("limited range");
}

OBSStatsRenditionInspector::OBSStatsRenditionInspector(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	QHBoxLayout *head = new QHBoxLayout();
	QLabel *title = new QLabel(QTStr("Basic.Stats.Inspector"), this);
	title->setStyleSheet("font-weight: bold");
	title->setToolTip(QTStr("Basic.Stats.Inspector.Hint"));
	selector = new QComboBox(this);
	selector->setToolTip(QTStr("Basic.Stats.Inspector.Select.ToolTip"));
	head->addWidget(title);
	head->addWidget(selector);
	head->addStretch();
	layout->addLayout(head);

	QFormLayout *form = new QFormLayout();
	form->setContentsMargins(0, 0, 0, 0);

	auto addValue = [&](const char *loc) {
		QLabel *value = new QLabel(this);
		value->setTextInteractionFlags(Qt::TextSelectableByMouse);
		value->setWordWrap(true);
		form->addRow(QTStr(loc), value);
		return value;
	};

	valueObject = addValue("Basic.Stats.Inspector.Object");
	valueFeed = addValue("Basic.Stats.Inspector.Feed");
	valueEncoderInput = addValue("Basic.Stats.Inspector.EncoderInput");
	valueFramePath = addValue("Basic.Stats.Inspector.FramePath");
	valueFrames = addValue("Basic.Stats.Inspector.Frames");
	valueRate = addValue("Basic.Stats.Inspector.Rate");
	valueSkips = addValue("Basic.Stats.Inspector.Skips");
	valueBitrate = addValue("Basic.Stats.Inspector.Bitrate");
	valuePayload = addValue("Basic.Stats.Inspector.Payload");

	layout->addLayout(form);

	connect(selector, &QComboBox::currentIndexChanged, this, [this](int index) {
		shownIndex = index;
		/* The numbers describe the rendition shown from now on: baselines restart, otherwise
		 * this pane would keep showing the previous rendition's counters. */
		shown = nullptr;
	});
}

void OBSStatsRenditionInspector::Rebase(obs_encoder_t *encoder, const video_t *mix,
					const obs_encoder_frame_counters &counters, uint64_t timeNs)
{
	shown = encoder;
	shownMix = mix;
	baseMixFrames = counters.mix_frames;
	baseLagSkipped = counters.mix_lag_skipped;
	baseOffered = counters.frames_offered;
	baseDecimated = counters.frames_decimated;
	baseEncoded = counters.frames_encoded;
	baseBytes = counters.encoded_bytes;
	baseTime = timeNs;
	lastBytes = counters.encoded_bytes;
	lastTime = timeNs;
}

void OBSStatsRenditionInspector::Update(obs_output_t *streamOutput, obs_output_t *recOutput)
{
	const uint64_t curTime = os_gettime_ns();

	/* Entry list: the video encoders of the running outputs, re-derived every tick. */
	QVector<Entry> next;
	const obs_output_t *outputs[] = {streamOutput, recOutput};
	for (size_t i = 0; i < 2; i++) {
		const obs_output_t *output = outputs[i];
		if (!output || !obs_output_active(output))
			continue;

		const QString section = QTStr(i == 0 ? "Basic.Stats.Output.Stream" : "Basic.Stats.Output.Recording");
		for (const OBS::EncoderSlot &slot : OBS::outputVideoEncoders(output))
			next.push_back(Entry{slot.encoder, section,
					     QString::fromStdString(OBS::renditionName(slot.encoder))});
	}

	if (next.isEmpty()) {
		entries.clear();
		shownIndex = -1;
		shown = nullptr;
		selector->clear();
		setVisible(false);
		return;
	}
	setVisible(true);

	bool changed = next.size() != entries.size();
	if (!changed) {
		for (qsizetype j = 0; j < next.size(); j++) {
			if (next[j].encoder == entries[j].encoder)
				continue;
			changed = true;
			break;
		}
	}

	if (changed) {
		obs_encoder_t *keep =
			(shownIndex >= 0 && shownIndex < entries.size()) ? entries[shownIndex].encoder : nullptr;
		entries = next;
		shownIndex = 0;
		if (keep) {
			for (qsizetype j = 0; j < entries.size(); j++) {
				if (entries[j].encoder != keep)
					continue;
				shownIndex = static_cast<int>(j);
				break;
			}
		}

		const QSignalBlocker blocker(selector);
		selector->clear();
		for (const Entry &entry : entries)
			selector->addItem(entry.section + QStringLiteral(" · ") + entry.label);
		selector->setCurrentIndex(shownIndex);
		shown = nullptr; /* new selection: re-baseline */
	}

	const Entry &entry = entries[shownIndex];
	obs_encoder_t *encoder = entry.encoder;
	const video_t *mix = OBS::deliveryMix(encoder);
	const obs_encoder_frame_counters c = OBS::encoderCounters(encoder);

	/* Re-arm on a selection change, a re-bound delivery mix or restarted counters. */
	if (encoder != shown || mix != shownMix || c.mix_frames < baseMixFrames || c.mix_lag_skipped < baseLagSkipped ||
	    c.frames_offered < baseOffered || c.frames_decimated < baseDecimated || c.frames_encoded < baseEncoded ||
	    c.encoded_bytes < baseBytes)
		Rebase(encoder, mix, c, curTime);

	const struct video_output_info *voi = mix ? video_output_get_info(mix) : nullptr;
	const double nominalFps = obs_encoder_get_effective_fps(encoder);

	valueObject->setText(QString("%1  [%2]").arg(obs_encoder_get_name(encoder), obs_encoder_get_id(encoder)));
	valueFeed->setText(
		voi ? QString("%1x%2, %3, %4, %5")
			    .arg(voi->width)
			    .arg(voi->height)
			    .arg(get_video_format_name(voi->format), ColorSpaceName(voi->colorspace), RangeName(voi->range))
		    : QString());
	valueEncoderInput->setText(QString("%1x%2 @ %3 fps, divisor %4")
					   .arg(obs_encoder_get_width(encoder))
					   .arg(obs_encoder_get_height(encoder))
					   .arg(QString::number(nominalFps, 'f', 2))
					   .arg(obs_encoder_get_frame_rate_divisor(encoder)));

	const uint32_t caps = obs_encoder_get_caps(encoder);
	valueFramePath->setText((caps & OBS_ENCODER_CAP_PASS_TEXTURE) ? QTStr("Basic.Stats.Inspector.FramePath.GPU")
								     : QTStr("Basic.Stats.Inspector.FramePath.CPU"));
	valueFramePath->setToolTip(QTStr("Basic.Stats.Inspector.FramePath.ToolTip"));

	const uint64_t mixFrames = c.mix_frames - baseMixFrames;
	const uint64_t lagSkipped = c.mix_lag_skipped - baseLagSkipped;
	const uint64_t decimated = c.frames_decimated - baseDecimated;
	const uint64_t offered = c.frames_offered - baseOffered;
	const uint64_t encoded = c.frames_encoded - baseEncoded;

	valueFrames->setText(QString("mix %1, offered %2, encoded %3")
				     .arg(QString::number(mixFrames), QString::number(offered), QString::number(encoded)));
	valueFrames->setToolTip(QTStr("Basic.Stats.Inspector.Frames.ToolTip"));

	const long double runSeconds = (long double)(curTime - baseTime) / 1000000000.0l;
	const double measuredFps = runSeconds >= 0.01l ? (double) ((long double) offered / runSeconds) : 0.0;
	valueRate->setText(QString("%1 fps delivered / %2 fps nominal")
				   .arg(QString::number(measuredFps, 'f', 2), QString::number(nominalFps, 'f', 2)));

	const long double pct = mixFrames ? (long double)lagSkipped / (long double)mixFrames * 100.0l : 0.0l;
	valueSkips->setText(QString("lag %1 (%2%) · decimated %3")
				    .arg(QString::number(lagSkipped), QString::number(pct, 'f', 1),
					 QString::number(decimated)));
	if (pct > 5.0l)
		setClasses(valueSkips, "text-danger");
	else if (pct > 1.0l)
		setClasses(valueSkips, "text-warning");
	else
		setClasses(valueSkips, "");

	uint64_t bytes = c.encoded_bytes;
	if (bytes < lastBytes)
		bytes = lastBytes;
	const long double seconds = (long double)(curTime - lastTime) / 1000000000.0l;
	long double kbps = seconds >= 0.01l ? (long double)(bytes - lastBytes) * 8.0l / seconds / 1000.0l : 0.0l;
	lastBytes = bytes;
	lastTime = curTime;

	const char *unit = "kb/s";
	if (kbps >= 10'000) {
		kbps /= 1000;
		unit = "Mb/s";
	}
	valueBitrate->setText(QString("%1 %2").arg(kbps, 0, 'f', 0).arg(unit));
	valuePayload->setText(QString("%1 MiB").arg((long double) (bytes - baseBytes) / (1024.0l * 1024.0l), 0, 'f', 1));
}
