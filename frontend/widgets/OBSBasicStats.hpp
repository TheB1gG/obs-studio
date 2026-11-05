#pragma once

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>

#include <QFrame>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

class QLabel;
class QWidget;
class QGridLayout;
class QToolButton;

#include <string>
#include <vector>

/* EXPERIMENT (option G) - remove together with widgets/OBSStatsRenditionInspector.{hpp,cpp}. */
class OBSStatsRenditionInspector;

class OBSBasicStats : public QFrame {
	Q_OBJECT

	QLabel *fps = nullptr;
	QLabel *cpuUsage = nullptr;
	QLabel *hddSpace = nullptr;
	QLabel *recordTimeLeft = nullptr;
	QLabel *memUsage = nullptr;

	QLabel *renderTime = nullptr;
	QLabel *skippedFrames = nullptr;
	QLabel *missedFrames = nullptr;

	QGridLayout *outputLayout = nullptr;

	os_cpu_usage_info_t *cpu_info = nullptr;

	QTimer timer;
	QTimer recTimeLeft;
	uint64_t num_bytes = 0;
	std::vector<long double> bitrates;

	/* One row per video encoder (rendition) of an output, i.e. one row per track of a
	 * multitrack stream or per video encoder of a recording output.
	 *
	 * The labels are views: they get destroyed and re-created whenever the structure of
	 * the table changes (RebuildOutputGrid()). The counters below are the state - they
	 * survive those rebuilds and are carried over to the new row for the same encoder
	 * (an encoder object survives a re-init or a live rescale; only its delivery mix
	 * may change, which re-arms the baselines).
	 *
	 * Baselines exist because both libobs counter groups restart on delivery changes:
	 * the mix counters when the first consumer attaches (video_output_connect2() ->
	 * reset_frames()) and the per-encoder pair when the encoder re-binds. Never display
	 * an absolute libobs counter - always the delta against the baseline. */
	struct EncoderRow {
		/* Identity of the row. Re-resolved every tick by SyncRenditions(); only used to
		 * match a row up with its baselines, never dereferenced across ticks: an active
		 * output holds strong references to its encoders, so the pointer stays valid
		 * while the row exists. */
		obs_encoder_t *encoder = nullptr;
		/* Delivery mix the row's numbers were last read from. */
		const video_t *mix = nullptr;

		QPointer<QLabel> name;      /* rendition label, e.g. "MT video enc 0" */
		QPointer<QLabel> impl;      /* what encodes it, e.g. "QSV H264"        */
		QPointer<QLabel> delivery;  /* mix size, encoder rate, pixel format    */
		QPointer<QLabel> encoded;
		QPointer<QLabel> lag;       /* mix-level skips because of encoding lag */
		QPointer<QLabel> decimated; /* this encoder's own rate-divisor skips    */
		QPointer<QLabel> bitrate;

		uint64_t baseMixFrames = 0;
		uint64_t baseMixLagSkipped = 0;
		uint64_t baseOffered = 0;
		uint64_t baseDecimated = 0;
		uint64_t baseEncoded = 0;
		uint64_t baseBytes = 0;
		uint64_t baseTime = 0;
		uint64_t lastBytes = 0;
		uint64_t lastBytesTime = 0;

		/* More than one encoder is fed from this row's mix, so the mix-level numbers
		 * (delivery, lag skips) are shared with a sibling row. */
		bool sharedMix = false;

		long double kbps = 0.0l;

		/* Take a fresh set of baselines from the given counters (start of a run, or a
		 * restarted/re-bound delivery path). */
		void Rebase(const struct obs_encoder_frame_counters &counters, const video_t *mix, uint64_t timeNs);
	};

	struct OutputLabels {
		QString title;

		/* A section (its bar and its rendition rows) only exists in the table while the
		 * output is running - an idle Stream/Recording row is noise when renditions can be
		 * several rows deep. `active` mirrors what RebuildOutputGrid() laid out.
		 *
		 * A streaming session can hold several connections (slice 2b), so a destination that is
		 * part of the session but currently not uploading still gets a bar (`present`): that is
		 * where its restart action lives. Its rendition rows stay hidden. */
		bool active = false;
		bool present = false;

		/* The destination this section belongs to; empty for the recording section and for the
		 * single-destination case. Identity only - never dereferenced, the output is handed to
		 * Update()/SyncRenditions() on every tick. */
		std::string id;
		bool recording = false;
		/* The output this section was last built for, so a replaced connection (a restart) can be
		 * noticed without throwing the section's counters away. */
		obs_output_t *watched = nullptr;

		/* One bar spanning the table width:
		 *   "Stream · LIVE · sent 2.1 GiB · 24.8 Mb/s · dropped 0 / 117741 (0.0%)"
		 * The values stay separate labels because the status and the drop count are
		 * coloured through setClasses() and the active theme. */
		QPointer<QWidget> bar;
		QPointer<QLabel> nameLabel;
		QPointer<QLabel> statusLabel;
		QPointer<QLabel> sentLabel;
		QPointer<QLabel> bitrateLabel;
		QPointer<QLabel> droppedLabel;

		/* Per-connection actions (Q13): stop or kill one destination without ending the session, and
		 * bring a dropped one back while the rest keeps uploading. */
		QPointer<QToolButton> stopButton;
		QPointer<QToolButton> killButton;
		QPointer<QToolButton> restartButton;

		uint64_t lastBytesSent = 0;
		uint64_t lastBytesSentTime = 0;

		int first_total = 0;
		int first_dropped = 0;

		QVector<EncoderRow> rows;

		void Update(obs_output_t *output, bool rec);
		void Reset(obs_output_t *output);

		/* Rebuild the row list from the video encoders of the given output; returns
		 * true when the set of rows changed and the table has to be laid out again. */
		bool SyncRenditions(obs_output_t *output);
		/* Refresh the counters and the text of every row. */
		void UpdateRenditions();

		long double kbps = 0.0l;
	};

	QList<OutputLabels> outputLabels;

	/* One connection of the streaming session as the stats window is told about it. The output pointer is
	 * borrowed for the duration of the call; the caller (Update) keeps the references alive while it ticks. */
	struct SectionInfo {
		std::string id;
		QString title;
		obs_output_t *output = nullptr;
		bool present = false;
	};

	void AddOutputLabels(QString name, bool recording = false);
	/* Bring the stream sections in line with the connections of the session: one section per destination,
	 * named by destination, with the recording section kept. Counters of a section that stays are carried
	 * over, so a rebuild does not reset the deltas. Returns true when the set changed and the table has to
	 * be laid out again. */
	bool SyncSections(const std::vector<SectionInfo> &session, obs_output_t *recOutput);
	/* (Re-)create every label of the output table from the current model. */
	void RebuildOutputGrid();
	/* Flag the rows whose delivery mix feeds more than one encoder (mix reuse). */
	void MarkSharedMixes();
	void Update();

	/* EXPERIMENT (option G): the single-rendition drill-down pane below the table. The whole
	 * feature lives in widgets/OBSStatsRenditionInspector.{hpp,cpp} plus the blocks marked
	 * "EXPERIMENT (option G)" here - one revert removes it, and [Stats] RenditionInspector=false
	 * turns it off without a rebuild. */
	QPointer<OBSStatsRenditionInspector> renditionInspector;

	virtual void closeEvent(QCloseEvent *event) override;

	static void OBSFrontendEvent(enum obs_frontend_event event, void *ptr);

public:
	OBSBasicStats(QWidget *parent = nullptr, bool closable = true);
	~OBSBasicStats();

	static void InitializeValues();

	void StartRecTimeLeft();
	void ResetRecTimeLeft();

private:
	QPointer<QObject> shortcutFilter;

private slots:
	void RecordingTimeLeft();

public slots:
	void Reset();

protected:
	virtual void showEvent(QShowEvent *event) override;
	virtual void hideEvent(QHideEvent *event) override;
};
