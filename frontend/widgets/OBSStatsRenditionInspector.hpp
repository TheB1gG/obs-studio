#pragma once

#include <obs.h>

#include <QString>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;

/* EXPERIMENT (option G of the stats-panel layout review) - single-rendition drill-down.
 *
 * The Stats table deliberately keeps one compact row per rendition. This pane shows
 * everything libobs knows about the rendition selected in its combo box, i.e. the numbers
 * that do not fit into a column: measured delivered frame rate next to the nominal one, the
 * pixel format/colorspace/range of the feed, the frame-rate divisor, payload bytes, and
 * whether the feed mix is shared with another rendition.
 *
 * It is intentionally self-contained: it only reads through the OBS:: helpers in
 * <utility/EncoderStats.hpp>, keeps its own counter baselines and touches no state of
 * OBSBasicStats. Dropping the experiment again is one revert - delete these two files plus
 * the blocks marked "EXPERIMENT (option G)" in OBSBasicStats.{hpp,cpp} and the
 * Basic.Stats.Inspector.* strings. No Q_OBJECT: it needs no signals/slots of its own, so it
 * does not depend on moc at all.
 */
class OBSStatsRenditionInspector : public QWidget {
	struct Entry {
		obs_encoder_t *encoder = nullptr;
		QString section; /* "Stream" / "Recording" */
		QString label;   /* rendition label, already abbreviated */
	};

	QComboBox *selector = nullptr;
	QVector<Entry> entries;
	int shownIndex = -1;

	QLabel *valueObject = nullptr;
	QLabel *valueFeed = nullptr;
	QLabel *valueEncoderInput = nullptr;
	QLabel *valueFramePath = nullptr;
	QLabel *valueFrames = nullptr;
	QLabel *valueRate = nullptr;
	QLabel *valueSkips = nullptr;
	QLabel *valueBitrate = nullptr;
	QLabel *valuePayload = nullptr;

	/* Baselines of the shown encoder - re-armed whenever the encoder or its delivery mix
	 * changes, or any counter went down (same rules as the table rows). */
	obs_encoder_t *shown = nullptr;
	const video_t *shownMix = nullptr;
	uint64_t baseMixFrames = 0;
	uint64_t baseLagSkipped = 0;
	uint64_t baseOffered = 0;
	uint64_t baseDecimated = 0;
	uint64_t baseEncoded = 0;
	uint64_t baseBytes = 0;
	uint64_t baseTime = 0;
	uint64_t lastBytes = 0;
	uint64_t lastTime = 0;

	void Rebase(obs_encoder_t *encoder, const video_t *mix, const obs_encoder_frame_counters &counters,
		    uint64_t timeNs);

public:
	explicit OBSStatsRenditionInspector(QWidget *parent = nullptr);

	/* Refresh the entry list and the shown rendition. Called on the stats tick. */
	void Update(obs_output_t *streamOutput, obs_output_t *recOutput);
};
