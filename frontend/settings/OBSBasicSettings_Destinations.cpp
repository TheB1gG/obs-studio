#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSharedPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

namespace {

/* A one-line label that survives a narrow dialog. The summary lines carry everything a closed block knows,
 * and they are longer than the settings dialog can be made: left alone, the label's size hint becomes the
 * minimum width of the row, which pushes the whole Output tab past its own viewport - the tab grows a
 * horizontal scrollbar and the text is cut mid-glyph anyway. This one takes the width it is handed and
 * elides the tail, and keeps the whole line in its tooltip so nothing is lost.
 *
 * The size policy is the part that is easy to get wrong: a QLabel in a box layout keeps its size hint as a
 * minimum unless the policy tells the layout otherwise, and setTextInteractionFlags() replaces the policy -
 * so both are set here, and callers that set interaction flags afterwards must set the policy again. */
class ElidedLabel : public QLabel {
public:
	explicit ElidedLabel(const QString &text = QString(), QWidget *parent = nullptr) : QLabel(parent)
	{
		/* Ignored: the layout decides the width, the label's own hint does not get a vote. Paired with a
		 * stretch factor at the call site, that is the "take what is left" line the mock draws. */
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		setMinimumWidth(0);
		setFullText(text);
	}

	void setFullText(const QString &text)
	{
		full_ = text;
		/* The tooltip is the un-elided line by the definition of this class, so it has to follow the text
		 * every time: a section header line is re-stated whenever the user changes what it describes
		 * (toggling the reconnect override does exactly that), and a tooltip that was only set once would
		 * then go on describing the old value. */
		setToolTip(full_);
		/* Start from the whole line; resizeEvent elides it once the layout has handed over a width. */
		QLabel::setText(full_);
	}

	/* Where the dots go when the line does not fit. The destination line ends with the bitrate, which is the
	 * one number worth comparing between destinations, so that line elides in the middle instead. */
	void setElideMode(Qt::TextElideMode mode)
	{
		elide_ = mode;
		relabel();
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QLabel::resizeEvent(event);
		relabel();
	}

private:
	void relabel()
	{
		/* setText() invalidates the layout, which can come straight back around to resizeEvent. */
		if (relabelling_)
			return;
		relabelling_ = true;
		const int available = qMax(0, width());
		const QString shown = fontMetrics().horizontalAdvance(full_) <= available
					      ? full_
					      : fontMetrics().elidedText(full_, elide_, available);
		if (shown != QLabel::text())
			QLabel::setText(shown);
		relabelling_ = false;
	}

	QString full_;
	Qt::TextElideMode elide_ = Qt::ElideRight;
	bool relabelling_ = false;
};

/* A titled, collapsible part of a destination block - the shape the mock uses for its sections. The header line
 * carries a one-liner summarising what is inside, so a collapsed block still answers the question the section
 * is about, which is what makes the whole list scannable. */
class DestinationSection : public QWidget {
public:
	explicit DestinationSection(const QString &title, QWidget *parent = nullptr) : QWidget(parent)
	{
		auto layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(3);

		auto header = new QHBoxLayout;
		header->setContentsMargins(0, 0, 0, 0);

		toggle_ = new QToolButton(this);
		toggle_->setObjectName("destinationSectionToggle");
		toggle_->setCheckable(true);
		toggle_->setChecked(true);
		toggle_->setAutoRaise(true);
		toggle_->setArrowType(Qt::DownArrow);

		auto label = new QLabel(title, this);
		QFont bold = label->font();
		bold.setBold(true);
		label->setFont(bold);

		summary_ = new ElidedLabel(QString(), this);
		summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

		header->addWidget(toggle_);
		header->addWidget(label);
		header->addWidget(summary_, 1);
		layout->addLayout(header);

		body_ = new QWidget(this);
		bodyLayout_ = new QVBoxLayout(body_);
		bodyLayout_->setContentsMargins(SECTION_INDENT, 0, 0, 0);
		bodyLayout_->setSpacing(4);
		layout->addWidget(body_);

		connect(toggle_, &QToolButton::toggled, body_, &QWidget::setVisible);
		connect(toggle_, &QToolButton::toggled, this,
			[this](bool open) { toggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow); });
	}

	QVBoxLayout *bodyLayout() const { return bodyLayout_; }
	void setSummary(const QString &text) { summary_->setFullText(text); }

private:
	/* Lines the body up under the section title instead of under the chevron. */
	static constexpr int SECTION_INDENT = 24;

	QToolButton *toggle_ = nullptr;
	ElidedLabel *summary_ = nullptr;
	QWidget *body_ = nullptr;
	QVBoxLayout *bodyLayout_ = nullptr;
};

/* A label that reads as secondary text: smaller rather than coloured, because the themes own the palette and a
 * hard-coded grey looks wrong in the light ones. Word wrap so a long sentence (the bandwidth breakdown, the
 * reconnect values) wraps inside the block instead of asking the whole tab for more width than it has. */
QLabel *SecondaryLabel(const QString &text, QWidget *parent = nullptr)
{
	auto label = new QLabel(text, parent);
	QFont font = label->font();
	font.setPointSizeF(font.pointSizeF() * 0.9);
	label->setFont(font);
	label->setWordWrap(true);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	return label;
}

/* A disabled destination stays fully interactive (its buttons must still work), so it is dimmed with an
 * opacity effect instead of setEnabled(false). */
void DimBlockIfNeeded(QWidget *block, bool dim)
{
	if (!dim && !block->graphicsEffect())
		return;

	auto effect = new QGraphicsOpacityEffect(block);
	effect->setOpacity(dim ? 0.55 : 1.0);
	block->setGraphicsEffect(effect);
}

/* The providers libobs ships, complete and sorted. The Stream settings page offers a curated short list with
 * a "show all" entry, but a second destination is by nature an endpoint that list was not picked for, so the
 * card goes straight to everything the rtmp-services plugin knows about. */
QStringList CommonServiceProviders()
{
	QStringList names;

	obs_properties_t *props = obs_get_service_properties("rtmp_common");
	if (!props)
		return names;

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "show_all", true);
	if (obs_property_t *show_all = obs_properties_get(props, "show_all"))
		obs_property_modified(show_all, settings);

	if (obs_property_t *services = obs_properties_get(props, "service")) {
		const size_t count = obs_property_list_item_count(services);
		for (size_t i = 0; i < count; i++) {
			const char *name = obs_property_list_item_string(services, i);
			if (name && *name)
				names << QString::fromUtf8(name);
		}
	}

	obs_properties_destroy(props);
	names.sort(Qt::CaseInsensitive);
	return names;
}

/* The ingest address and protocol a provider uploads to. libobs answers in two places: the settings object
 * gets the protocol, and the provider's list of ingests is a *list property* whose first entry is the default
 * - which is what the Stream settings page preselects in its server combo (UpdateServerList). A provider with
 * no fixed ingest comes back empty, and then the key alone says where the stream goes. */
struct ProviderIngest {
	QString server;
	QString protocol;
};

ProviderIngest ResolveProviderIngest(const QString &provider)
{
	ProviderIngest out;
	if (provider.isEmpty())
		return out;

	obs_properties_t *props = obs_get_service_properties("rtmp_common");
	if (!props)
		return out;

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "service", QT_TO_UTF8(provider));
	if (obs_property_t *services = obs_properties_get(props, "service"))
		obs_property_modified(services, settings);

	if (const char *protocol = obs_data_get_string(settings, "protocol"))
		out.protocol = QString::fromUtf8(protocol);
	if (const char *server = obs_data_get_string(settings, "server"))
		out.server = QString::fromUtf8(server);

	if (out.server.isEmpty()) {
		if (obs_property_t *servers = obs_properties_get(props, "server")) {
			const char *first = obs_property_list_item_string(servers, 0);
			if (first && *first)
				out.server = QString::fromUtf8(first);
		}
	}

	obs_properties_destroy(props);
	return out;
}

/* The four ways a card says where its stream goes. The first two are the states a destination is in without
 * typing anything; the other two write a service_<id>.json of its own. */
enum class EndpointChoice : int {
	NotConfigured = 0,
	ProfileDefault,
	CustomServer,
	KnownProvider,
};

} // namespace

void OBSBasicSettings::SetupDestinationsPage()
{
	destinationsPageLayout = ui->destinationsPageLayout;
	destinationsPageLayout->setContentsMargins(9, 2, 9, 9);
	destinationsPageLayout->setSpacing(6);

	/* The group box title already names this surface, so the paragraph that used to sit under it becomes the
	 * group's tooltip: the explanation is still there, it just stops eating two lines of the Output tab. */
	ui->advOutDestinationsGroup->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Desc"));

	/* One bar at the top rather than a button at the bottom of a list: what you reach for first is the thing
	 * that has to be easiest to find, and the running total belongs next to it, not under a scroll. */
	auto bar = new QHBoxLayout;
	addDestinationButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Add"));
	connect(addDestinationButton, &QPushButton::clicked, this, &OBSBasicSettings::AddDestination);
	bar->addWidget(addDestinationButton);
	bar->addStretch();
	destinationsTotalBitrateLabel = new QLabel;
	destinationsTotalBitrateLabel->setToolTip(QTStr("Basic.Settings.Stream.Destinations.TotalUpload.ToolTip"));
	bar->addWidget(destinationsTotalBitrateLabel);
	destinationsPageLayout->addLayout(bar);

	destinationsEmptyLabel = new QLabel(QTStr("Basic.Settings.Stream.Destinations.Empty"));
	destinationsEmptyLabel->setWordWrap(true);
	destinationsEmptyLabel->setAlignment(Qt::AlignCenter);
	destinationsPageLayout->addWidget(destinationsEmptyLabel);

	/* One block per destination, in a container of their own so a rebuild can drop every block without touching
	 * the bar or the empty label. Deliberately not a QListWidget: the item view brings a second scroll area
	 * inside the tab's own one, needs a size hint per item to avoid clipping its own widgets, and shows a
	 * selection highlight on a row that has nothing to select. */
	destinationsBlocks = new QWidget;
	destinationsBlocksLayout = new QVBoxLayout(destinationsBlocks);
	destinationsBlocksLayout->setContentsMargins(0, 0, 0, 0);
	destinationsBlocksLayout->setSpacing(6);
	destinationsPageLayout->addWidget(destinationsBlocks);

	/* While a session is running the blocks also say what each destination is doing: the state lives in
	 * OBSBasic's runtime entries and this page only borrows it, so nothing here can change a session. When no
	 * session is running the state labels stay empty and the page looks exactly like it always did. */
	destinationsStateTimer = new QTimer(this);
	connect(destinationsStateTimer, &QTimer::timeout, this, &OBSBasicSettings::RefreshDestinationStates);
	destinationsStateTimer->start(1000);

	LoadDestinationsSettings();
	RebuildDestinationBlocks();
	RefreshDestinationStates();
}
void OBSBasicSettings::RebuildDestinationBlocks()
{
	if (!destinationsBlocksLayout)
		return;

	/* Drop the blocks of the previous run - the bar and the empty label sit in the layout above this one, so
	 * they survive. Their state labels go with them and are remembered again below. */
	while (QLayoutItem *previous = destinationsBlocksLayout->takeAt(0)) {
		if (QWidget *widget = previous->widget())
			widget->deleteLater();
		delete previous;
	}
	destinationStateLabels.clear();
	/* The endpoint checks of the cards that are going away, dropped before the widgets they point at:
	 * a stale closure would stage an edit for a card that no longer exists. */
	destinationServiceChecks.clear();

	StreamingDestinations &destinations = destinationEdits;
	const auto list = destinations.destinations();
	const int count = static_cast<int>(list.size());

	for (int index = 0; index < count; index++) {
		const StreamingDestination &dest = list[index];
		const std::string id = dest.id;

		/* A page of four fully opened blocks is what made this unreadable, so a block starts open only if
		 * the user left it open: everything the header line answers (where it goes, what it costs, whether
		 * it is up) is readable shut. The choice is remembered by id, because any structural change rebuilds
		 * the blocks and a rebuild must not reopen what somebody just closed. */
		const auto remembered = destinationBlockOpen.find(id);
		const bool open = remembered != destinationBlockOpen.end() ? remembered->second : (index == 0);
		destinationBlockOpen[id] = open;

		auto block = new QFrame;
		block->setObjectName("destinationBlock");
		/* A thin frame around each destination: without one, the header line of a block and the section header
		 * of the block above it sit at the same weight and the list reads as one long form. */
		block->setFrameShape(QFrame::StyledPanel);
		block->setFrameShadow(QFrame::Plain);
		auto blockLayout = new QVBoxLayout(block);
		blockLayout->setContentsMargins(8, 6, 8, 6);
		blockLayout->setSpacing(4);

		/* The header line is the point of the whole block: with everything closed it has to answer which
		 * destination this is, where it goes, what it costs and whether it is up - that is what makes a list
		 * of three or four of them readable at a glance instead of a wall of fields. */
		auto header = new QHBoxLayout;
		header->setContentsMargins(0, 0, 0, 0);
		header->setSpacing(6);

		auto expander = new QToolButton;
		expander->setObjectName("destinationExpander");
		expander->setCheckable(true);
		expander->setChecked(open);
		expander->setAutoRaise(true);
		expander->setArrowType(Qt::DownArrow);
		expander->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Toggle"));

		/* No text on the box: it sits at the head of the line it belongs to, the block dims when it is off and
		 * the state label says the rest. The words stay in the tooltip and the accessible name. */
		auto enable = new QCheckBox;
		enable->setChecked(dest.enabled);
		enable->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Enable"));
		enable->setAccessibleName(QTStr("Basic.Settings.Stream.Destinations.Enable"));
		connect(enable, &QCheckBox::toggled, this, [this, id](bool on) {
			destinationEdits.setEnabled(id, on);
			MarkDestinationsChanged();
		});

		/* Numbered like the mock numbers them: the position in the list is not cosmetic (the first enabled
		 * destination is what the legacy streaming API reports), so the number belongs where the user can see
		 * it while reordering. */
		const QString titleText =
			QString("%1 \xE2\x80\xA2 %2").arg(index + 1).arg(QString::fromStdString(dest.name));
		auto title = new ElidedLabel;
		/* Maximum with a hard cap: a long destination name must not push the rest of the header line out of
		 * the dialog, so it gets this much at most and the label elides what does not fit. */
		title->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
		title->setMaximumWidth(320);
		QFont titleFont = title->font();
		titleFont.setBold(true);
		title->setFont(titleFont);
		title->setFullText(titleText);

		auto summary = new ElidedLabel;
		/* setTextInteractionFlags() replaces the size policy, so the policy has to come after it - otherwise
		 * the label keeps its size hint as a minimum and the tab gets a horizontal scrollbar. */
		summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
		summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		/* The line is "id • where it goes • what it costs"; the cost is the part worth comparing, so the
		 * dots go in the middle of the address rather than over the number. */
		summary->setElideMode(Qt::ElideMiddle);

		/* Filled in by RefreshDestinationStates() while a session runs; stays blank otherwise. */
		auto state = new QLabel;
		state->setWordWrap(false);
		destinationStateLabels[id] = state;

		auto renameButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Rename"));
		connect(renameButton, &QPushButton::clicked, this, [this, id]() { RenameDestination(id); });

		auto removeButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Remove"));
		connect(removeButton, &QPushButton::clicked, this, [this, id]() { RemoveDestination(id); });

		/* The house icon classes rather than drawn glyphs: the theme already ships a matching up/down pair,
		 * so these buttons look like every other pair of them in OBS and follow the theme the user chose. */
		auto moveUp = new QToolButton;
		moveUp->setObjectName("destinationMoveUp");
		moveUp->setAutoRaise(true);
		setClasses(moveUp, "icon-up");
		moveUp->setToolTip(QTStr("Basic.Settings.Stream.Destinations.MoveUp"));
		moveUp->setEnabled(index > 0);
		connect(moveUp, &QToolButton::clicked, this, [this, id]() { MoveDestination(id, -1); });

		auto moveDown = new QToolButton;
		moveDown->setAutoRaise(true);
		setClasses(moveDown, "icon-down");
		moveDown->setToolTip(QTStr("Basic.Settings.Stream.Destinations.MoveDown"));
		moveDown->setEnabled(index + 1 < count);
		connect(moveDown, &QToolButton::clicked, this, [this, id]() { MoveDestination(id, 1); });

		header->addWidget(expander);
		header->addWidget(enable);
		header->addWidget(title);
		header->addWidget(summary, 1);
		header->addWidget(state);
		header->addWidget(renameButton);
		header->addWidget(removeButton);
		header->addWidget(moveUp);
		header->addWidget(moveDown);
		blockLayout->addLayout(header);

		/* Everything under the header in one widget, so the expander has a single thing to hide. */
		auto details = new QWidget;
		auto detailsLayout = new QVBoxLayout(details);
		detailsLayout->setContentsMargins(0, 0, 0, 0);
		detailsLayout->setSpacing(6);
		blockLayout->addWidget(details);
		details->setVisible(open);
		expander->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);

		connect(expander, &QToolButton::toggled, details, &QWidget::setVisible);
		connect(expander, &QToolButton::toggled, this, [this, id](bool on) { destinationBlockOpen[id] = on; });
		connect(expander, &QToolButton::toggled, block,
			[expander](bool open) { expander->setArrowType(open ? Qt::DownArrow : Qt::RightArrow); });

		/* Where the stream goes, what it costs, and what happens when the link drops: three sub-sections
		 * with a header line of their own, so a destination that needs nothing is read without opening
		 * anything, and the one that needs attention opens only the part that matters. The Service section
		 * is built at the end of the block and inserted at the top: its endpoint editor holds on to this
		 * block's header line and has to re-state it as the fields change, and that line only exists once
		 * the destination's cost is known. */
		const size_t tracks = destinations.trackCount(dest.id);
		const QString tracksText =
			tracks == 0 ? QTStr("Basic.Settings.Stream.Destinations.Tracks.None")
				    : QTStr("Basic.Settings.Stream.Destinations.Tracks.Count").arg(int(tracks));

		/* What the card uploads is computed once, by the cost closure below: the row, the section header
		 * line and the block line all come from that one place. */

		/* What it costs, in a section of its own: that number is the one worth comparing between
		 * destinations, so it also sits in the section header line and stays visible when closed. */
		auto uploadSection = new DestinationSection(QTStr("Basic.Settings.Stream.Destinations.Bitrate"));
		detailsLayout->addWidget(uploadSection);
		auto uploadForm = new QFormLayout;
		uploadForm->setContentsMargins(0, 0, 0, 0);
		uploadForm->setHorizontalSpacing(10);
		uploadForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
		uploadForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		uploadSection->bodyLayout()->addLayout(uploadForm);
		uploadForm->addRow(QTStr("Basic.Settings.Stream.Destinations.Tracks"), SecondaryLabel(tracksText));
		auto bandwidthRow = SecondaryLabel(QString());
		uploadForm->addRow(bandwidthRow);
		uploadForm->addRow(QTStr("Basic.Settings.Stream.Destinations.Recording"),
				   SecondaryLabel(QTStr("Basic.Settings.Stream.Destinations.Recording.Session")));

		/* One closure answers "what does this card say it costs", reading the model rather than the
		 * widgets: the row inside the section, the section's header line and the block's own line are all
		 * written from the same numbers, so they cannot disagree. The block's line also carries where the
		 * stream goes, which the endpoint editor restates as its fields change - and neither of the two
		 * rebuilds the card, because a rebuild would throw away a half-typed stream key. */
		auto restate_card = [this, id = dest.id, summary, uploadSection, bandwidthRow]() {
			const auto bandwidth = destinationEdits.bitrateFor(id);
			QString bandwidthText;
			QString totalText;

			if (bandwidth.videoKbps == 0 && bandwidth.audioKbps == 0) {
				bandwidthText = QTStr("Basic.Settings.Stream.Destinations.Bitrate.None");
			} else {
				bandwidthText = QTStr("Basic.Settings.Stream.Destinations.Bitrate.Value")
							.arg(bandwidth.videoKbps)
							.arg(bandwidth.audioKbps)
							.arg(bandwidth.videoKbps + bandwidth.audioKbps);
				if (bandwidth.own_encoder)
					bandwidthText += QTStr("Basic.Settings.Stream.Destinations.Bitrate.OwnVideo");
				else if (bandwidth.shared_video)
					bandwidthText +=
						QTStr("Basic.Settings.Stream.Destinations.Bitrate.SharedVideo");

				totalText = QString("%1 kbps").arg(bandwidth.videoKbps + bandwidth.audioKbps);
			}

			bandwidthRow->setText(bandwidthText);
			uploadSection->setSummary(totalText);

			QString headerText =
				QString::fromStdString(id) + " \xE2\x80\xA2 " + DestinationEndpointSummary(id);
			if (!totalText.isEmpty())
				headerText += " \xE2\x80\xA2 " + totalText;
			summary->setFullText(headerText);

			UpdateDestinationsTotal();
		};
		restate_card();

		/* Q25's other half, on the card: the picture rate this destination uploads. Left alone, the
		 * destination is fed by the profile's single stream encoder - one encode serving every connection,
		 * which costs upload bandwidth but no encoding time. Checking the box pays for an encoder of this
		 * connection alone, and the engine caps that number by the service *this* destination uploads to
		 * rather than by the profile's. */
		const auto encoder_policy = destinations.videoBitratePolicy(dest.id);
		const long long profile_kbps = destinations.profileVideoBitrateKbps();

		auto ownEncoder = new QCheckBox(QTStr("Basic.Settings.Stream.Destinations.Encoder.Own"));
		ownEncoder->setChecked(encoder_policy.overridden);
		ownEncoder->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Encoder.ToolTip"));
		uploadSection->bodyLayout()->addWidget(ownEncoder);

		auto encoderDetail = new QWidget;
		auto encoderForm = new QFormLayout;
		encoderForm->setContentsMargins(0, 0, 0, 0);
		encoderForm->setHorizontalSpacing(10);
		encoderForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
		encoderForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		encoderDetail->setLayout(encoderForm);

		auto encoderBitrate = new QSpinBox;
		encoderBitrate->setRange(100, 50000);
		encoderBitrate->setSuffix(QTStr("Basic.Settings.Stream.Destinations.Encoder.Suffix"));
		const long long initial_kbps = encoder_policy.kbps > 0 ? encoder_policy.kbps : profile_kbps;
		encoderBitrate->setValue(static_cast<int>(std::clamp<long long>(initial_kbps, 100, 50000)));
		encoderForm->addRow(QTStr("Basic.Settings.Stream.Destinations.Encoder.Bitrate"), encoderBitrate);
		uploadSection->bodyLayout()->addWidget(encoderDetail);

		/* Said out loud, because it is the fact the checkbox changes: the number in this note is what the
		 * other connections of the session upload, and it does not move. */
		const QString shared_note =
			profile_kbps > 0 ? QTStr("Basic.Settings.Stream.Destinations.Encoder.Shared").arg(profile_kbps)
					 : QTStr("Basic.Settings.Stream.Destinations.Encoder.SharedUnknown");
		auto encoderSharedNote = SecondaryLabel(shared_note);
		uploadSection->bodyLayout()->addWidget(encoderSharedNote);

		auto pushEncoder = [this, id = dest.id, ownEncoder, encoderBitrate, restate_card]() {
			StreamingDestinations::VideoBitratePolicy policy;
			policy.overridden = ownEncoder->isChecked();
			if (policy.overridden)
				policy.kbps = encoderBitrate->value();
			destinationEdits.setVideoBitratePolicy(id, policy);
			restate_card();
			/* No rebuild: the number lives in the labels the closure restates, and a rebuild would
			 * destroy the endpoint fields of this very card mid-edit. */
			TouchDestinations();
		};

		connect(ownEncoder, &QCheckBox::toggled, encoderDetail, &QWidget::setVisible);
		connect(ownEncoder, &QCheckBox::toggled, encoderSharedNote,
			[encoderSharedNote](bool overridden) { encoderSharedNote->setVisible(!overridden); });
		connect(ownEncoder, &QCheckBox::toggled, this, pushEncoder);
		connect(encoderBitrate, &QSpinBox::valueChanged, this, pushEncoder);

		encoderDetail->setVisible(encoder_policy.overridden);
		encoderSharedNote->setVisible(!encoder_policy.overridden);

		/* What happens when the link drops. */
		auto connectionSection =
			new DestinationSection(QTStr("Basic.Settings.Stream.Destinations.Section.Connection"));
		detailsLayout->addWidget(connectionSection);

		/* Q14: reconnect values per destination. Only the values can differ - the attempt counter, the
		 * backoff timer and the thread waiting on it already belong to one output each in libobs, so an
		 * override here can never spend another destination's retries. With the override off the
		 * destination follows the profile-wide values from the hidden Advanced settings, which is what
		 * every destination does today. */
		const auto profile_policy = destinations.profileReconnectPolicy();
		const auto policy = destinations.reconnectPolicy(dest.id);

		/* One sentence for the section header, built from whichever policy is actually in force: the point of
		 * a header line is that the answer is there without opening the section. */
		auto policyText = [](const StreamingDestinations::ReconnectPolicy &p) {
			if (!p.enabled)
				return p.overridden ? QTStr("Basic.Settings.Stream.Destinations.Reconnect.ValuesOff")
						    : QTStr("Basic.Settings.Stream.Destinations.Reconnect.DefaultOff");
			if (p.overridden)
				return QTStr("Basic.Settings.Stream.Destinations.Reconnect.Values")
					.arg(p.retryDelaySec)
					.arg(p.maxRetries);
			return QTStr("Basic.Settings.Stream.Destinations.Reconnect.Default")
				.arg(p.retryDelaySec)
				.arg(p.maxRetries);
		};

		auto reconnectOverride = new QCheckBox(QTStr("Basic.Settings.Stream.Destinations.Reconnect.Override"));
		reconnectOverride->setChecked(policy.overridden);
		reconnectOverride->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Reconnect.ToolTip"));
		connectionSection->bodyLayout()->addWidget(reconnectOverride);

		auto reconnectDetail = new QWidget;
		auto detailLayout = new QHBoxLayout(reconnectDetail);
		detailLayout->setContentsMargins(0, 0, 0, 0);

		auto reconnectEnable = new QCheckBox(QTStr("Basic.Settings.Stream.Destinations.Reconnect.Enable"));
		reconnectEnable->setChecked(policy.enabled);

		auto retryDelayLabel = new QLabel(QTStr("Basic.Settings.Output.RetryDelay"));
		auto retryDelay = new QSpinBox;
		retryDelay->setRange(1, 30);
		retryDelay->setSuffix(" s");
		retryDelay->setValue(static_cast<int>(policy.retryDelaySec));

		auto maxRetriesLabel = new QLabel(QTStr("Basic.Settings.Output.MaxRetries"));
		auto maxRetries = new QSpinBox;
		maxRetries->setRange(1, 10000);
		maxRetries->setValue(static_cast<int>(policy.maxRetries));

		detailLayout->addWidget(reconnectEnable);
		detailLayout->addWidget(retryDelayLabel);
		detailLayout->addWidget(retryDelay);
		detailLayout->addWidget(maxRetriesLabel);
		detailLayout->addWidget(maxRetries);
		detailLayout->addStretch();
		connectionSection->bodyLayout()->addWidget(reconnectDetail);

		/* What the destination uses when it is not overriding anything; hidden the moment the values
		 * stop being the profile's. */
		auto reconnectSummary = SecondaryLabel(
			profile_policy.enabled ? QTStr("Basic.Settings.Stream.Destinations.Reconnect.Default")
							 .arg(profile_policy.retryDelaySec)
							 .arg(profile_policy.maxRetries)
					       : QTStr("Basic.Settings.Stream.Destinations.Reconnect.DefaultOff"));
		connectionSection->bodyLayout()->addWidget(reconnectSummary);
		connectionSection->setSummary(policyText(policy));

		auto pushReconnect = [this, id, reconnectOverride, reconnectEnable, retryDelay, maxRetries]() {
			StreamingDestinations::ReconnectPolicy updated;
			updated.overridden = reconnectOverride->isChecked();
			updated.enabled = reconnectEnable->isChecked();
			updated.retryDelaySec = retryDelay->value();
			updated.maxRetries = maxRetries->value();
			destinationEdits.setReconnectPolicy(id, updated);
			MarkDestinationsChanged();
		};

		connect(reconnectOverride, &QCheckBox::toggled, reconnectDetail, &QWidget::setVisible);
		connect(reconnectOverride, &QCheckBox::toggled, reconnectSummary,
			[reconnectSummary](bool overridden) { reconnectSummary->setVisible(!overridden); });
		connect(reconnectOverride, &QCheckBox::toggled, this, pushReconnect);
		/* Say in the section header what is in force. The blocks sit in a normal layout now, so showing or
		 * hiding a sub-widget resizes the block by itself - nothing has to restate a size hint. */
		connect(reconnectOverride, &QCheckBox::toggled, connectionSection,
			[connectionSection, policyText, reconnectEnable, retryDelay, maxRetries](bool overridden) {
				StreamingDestinations::ReconnectPolicy shown;
				shown.overridden = overridden;
				shown.enabled = reconnectEnable->isChecked();
				shown.retryDelaySec = retryDelay->value();
				shown.maxRetries = maxRetries->value();
				connectionSection->setSummary(policyText(shown));
			});
		connect(reconnectEnable, &QCheckBox::toggled, this, pushReconnect);
		connect(retryDelay, &QSpinBox::valueChanged, this, pushReconnect);
		connect(maxRetries, &QSpinBox::valueChanged, this, pushReconnect);

		reconnectDetail->setVisible(policy.overridden);
		reconnectSummary->setVisible(!policy.overridden);

		/* The endpoint editor goes at the top of the details, above the cost and the connection: it is
		 * built last because it needs the header-line closure just above, and it re-states that line each
		 * time one of its fields changes. */
		detailsLayout->insertWidget(0, BuildDestinationServiceSection(id, restate_card));

		DimBlockIfNeeded(block, !dest.enabled);
		destinationsBlocksLayout->addWidget(block);
	}

	if (destinationsEmptyLabel)
		destinationsEmptyLabel->setVisible(list.empty());

	UpdateDestinationsTotal();
}

/* What the profile would upload altogether: the sum over the enabled cards, recomputed by every card that
 * restates its own cost - one checkbox on one card moves the number the whole page reports. */
void OBSBasicSettings::UpdateDestinationsTotal()
{
	if (!destinationsTotalBitrateLabel)
		return;

	const long long total = destinationEdits.totalUploadKbps();
	destinationsTotalBitrateLabel->setText(
		total > 0 ? QTStr("Basic.Settings.Stream.Destinations.TotalUpload").arg(total)
			  : QTStr("Basic.Settings.Stream.Destinations.TotalUpload.None"));
	destinationsTotalBitrateLabel->setToolTip(QTStr("Basic.Settings.Stream.Destinations.TotalUpload.ToolTip"));
}

/* The "where does this stream go" part of a destination card. Every card needs an answer, because each
 * destination is a connection of its own: two destinations pointed at one stream key do not stream twice,
 * they get refused by the ingest. So the section offers the four states the model can hold, and writes a
 * service_<id>.json whenever the user picks a server or a provider of their own - which is what detaches a
 * destination from the profile's default service.
 *
 * The fields never rebuild the card, because a rebuild would destroy the line edit halfway through a typed
 * stream key: a choice change shows and hides whole groups instead. */
QWidget *OBSBasicSettings::BuildDestinationServiceSection(const std::string &id,
							  const std::function<void()> &restate_header)
{
	StreamingDestinations &destinations = destinationEdits;

	auto section = new DestinationSection(QTStr("Basic.Settings.Stream.Destinations.Section.Service"));

	auto form = new QFormLayout;
	form->setContentsMargins(0, 0, 0, 0);
	form->setHorizontalSpacing(10);
	form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	section->bodyLayout()->addLayout(form);

	const auto endpoint = destinations.serviceEndpoint(id);
	const bool owns = destinations.ownsService(id);
	/* The two service kinds this page can write. A destination whose file says something else (a WHIP
	 * endpoint, or one typed into the profile by hand) is described rather than edited: offering fields
	 * for it would rewrite the file into something the user did not type. */
	const bool writable = endpoint.type.empty() || endpoint.type == "rtmp_custom" || endpoint.type == "rtmp_common";

	auto choice = new QComboBox;
	/* Object names on the endpoint fields, as on the block's other controls: a stylesheet, or anything
	 * outside the page that has to address one card's fields, can then find them by name rather than by
	 * position in the layout. */
	choice->setObjectName("destinationEndpointChoice");
	choice->addItem(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.None"),
			(int)EndpointChoice::NotConfigured);
	/* Nothing to follow when the profile has no default service of its own, so there is no entry for it -
	 * and the note below says why the destination has to have an endpoint here. */
	const bool profile_available = destinations.profileServiceAvailable();
	if (profile_available)
		choice->addItem(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.Profile"),
				(int)EndpointChoice::ProfileDefault);
	choice->addItem(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.Custom"),
			(int)EndpointChoice::CustomServer);
	choice->addItem(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.Provider"),
			(int)EndpointChoice::KnownProvider);
	choice->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.ToolTip"));

	EndpointChoice current = EndpointChoice::NotConfigured;
	if (owns && writable)
		current = (endpoint.type == "rtmp_common") ? EndpointChoice::KnownProvider
							   : EndpointChoice::CustomServer;
	else if (!owns && destinations.followsProfileService(id))
		current = EndpointChoice::ProfileDefault;

	/* A destination marked as following a default service that is not there any more lands on "no
	 * endpoint", which is what it really is. */
	int choice_index = choice->findData((int)current);
	if (choice_index == -1)
		choice_index = choice->findData((int)EndpointChoice::NotConfigured);
	choice->setCurrentIndex(choice_index);
	form->addRow(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint"), choice);

	/* One container per kind, so switching the choice hides and shows a group instead of tearing down the
	 * widgets of the one that was in use. */
	auto customBody = new QWidget;
	auto customForm = new QFormLayout(customBody);
	customForm->setContentsMargins(0, 0, 0, 0);
	customForm->setHorizontalSpacing(10);
	customForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
	customForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	auto serverEdit = new QLineEdit;
	serverEdit->setObjectName("destinationEndpointServer");
	serverEdit->setPlaceholderText("rtmp://host/app   srt://host:port   rists://host:port/streamid=...");
	serverEdit->setClearButtonEnabled(true);
	if (owns && endpoint.type == "rtmp_custom")
		serverEdit->setText(QString::fromStdString(endpoint.server));
	customForm->addRow(QTStr("Basic.Settings.Stream.Destinations.Service.Server"), serverEdit);

	auto providerBody = new QWidget;
	auto providerForm = new QFormLayout(providerBody);
	providerForm->setContentsMargins(0, 0, 0, 0);
	providerForm->setHorizontalSpacing(10);
	providerForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
	providerForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	auto providerList = new QComboBox;
	providerList->setObjectName("destinationEndpointProvider");
	providerList->addItems(CommonServiceProviders());
	if (owns && endpoint.type == "rtmp_common") {
		const int idx = providerList->findText(QString::fromStdString(endpoint.provider), Qt::MatchFixedString);
		if (idx != -1)
			providerList->setCurrentIndex(idx);
	}
	providerForm->addRow(QTStr("Basic.Settings.Stream.Destinations.Service.Provider"), providerList);
	auto ingestNote = SecondaryLabel(QString());
	providerForm->addRow(ingestNote);

	/* The key belongs to both kinds - it is the same field of the same settings object - so it sits once,
	 * below whichever group is showing. */
	auto keyBody = new QWidget;
	auto keyForm = new QFormLayout(keyBody);
	keyForm->setContentsMargins(0, 0, 0, 0);
	keyForm->setHorizontalSpacing(10);
	keyForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
	keyForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	auto keyEdit = new QLineEdit;
	keyEdit->setObjectName("destinationEndpointKey");
	keyEdit->setClearButtonEnabled(true);
	if (!endpoint.key.empty())
		keyEdit->setText(QString::fromStdString(endpoint.key));
	keyForm->addRow(QTStr("Basic.Settings.Stream.Destinations.Service.Key"), keyEdit);

	form->addRow(customBody);
	form->addRow(providerBody);
	form->addRow(keyBody);

	auto note = SecondaryLabel(QString());
	section->bodyLayout()->addWidget(note);
	auto problem = SecondaryLabel(QString());
	section->bodyLayout()->addWidget(problem);

	/* Offered, not wired yet: fetching the service's own recommendation needs the provider's config
	 * download, which is the step after this one. The button keeps its own width: stretched across the
	 * whole page it would read like a disabled text field, which is what it is. */
	auto fetchButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Fetch"));
	fetchButton->setEnabled(false);
	fetchButton->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Fetch.Pending"));
	auto fetchRow = new QHBoxLayout;
	fetchRow->setContentsMargins(0, 0, 0, 0);
	fetchRow->addWidget(fetchButton);
	fetchRow->addStretch();
	section->bodyLayout()->addLayout(fetchRow);

	/* Which group of fields the choice shows. */
	auto showFor = [choice, customBody, providerBody, keyBody]() {
		const int which = choice->currentData().toInt();
		const bool custom = (which == (int)EndpointChoice::CustomServer);
		const bool provider = (which == (int)EndpointChoice::KnownProvider);
		customBody->setVisible(custom);
		providerBody->setVisible(provider);
		keyBody->setVisible(custom || provider);
	};

	/* What the choice means, in words that can be read before Apply: the profile's service for one kind,
	 * the ingest the chosen provider uploads to for the other, and the plain truth for a destination that
	 * has nowhere to send anything. Read-only - nothing here touches the model. */
	auto describe = [this, choice, providerList, ingestNote, note]() {
		const int which = choice->currentData().toInt();

		switch (which) {
		case (int)EndpointChoice::CustomServer:
			note->setText(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.Custom.Note"));
			break;
		case (int)EndpointChoice::KnownProvider: {
			const ProviderIngest ingest = ResolveProviderIngest(providerList->currentText());
			ingestNote->setText(
				ingest.server.isEmpty()
					? QTStr("Basic.Settings.Stream.Destinations.Service.Ingest.Unknown")
					: QTStr("Basic.Settings.Stream.Destinations.Service.Ingest").arg(ingest.server));
			note->setText(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.Provider.Note"));
			break;
		}
		case (int)EndpointChoice::ProfileDefault: {
			const auto profile = destinationEdits.profileServiceEndpoint();
			QString where;
			if (!profile.provider.empty())
				where = QString::fromStdString(profile.provider);
			else if (!profile.server.empty())
				where = QString::fromStdString(profile.server);
			else
				where = QTStr("Basic.Settings.Stream.Destinations.Service.NotConfigured");
			note->setText(
				QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.Profile.Note").arg(where));
			break;
		}
		default:
			note->setText(
				destinationEdits.profileServiceAvailable()
					? QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.None.Note")
					: QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.None.NoProfile"));
			break;
		}
	};

	showFor();
	describe();
	if (!writable) {
		note->setText(QTStr("Basic.Settings.Stream.Destinations.Service.Endpoint.Foreign")
				      .arg(QString::fromStdString(destinationEdits.serviceDescription(id))));
	}

	/* A refused edit leaves the model holding the last endpoint that was usable, and the choice has to say
	 * that: a card left showing "the profile's default service" while the destination really owns an
	 * endpoint would describe a connection that is not going to happen. Signals are blocked while the combo
	 * is put back, so this does not re-enter applyFields. */
	auto restate_choice = [this, id, choice]() {
		const auto ep = destinationEdits.serviceEndpoint(id);
		EndpointChoice back = EndpointChoice::NotConfigured;
		if (destinationEdits.ownsService(id))
			back = (ep.type == "rtmp_common") ? EndpointChoice::KnownProvider
							  : EndpointChoice::CustomServer;
		else if (destinationEdits.followsProfileService(id))
			back = EndpointChoice::ProfileDefault;

		const int index = choice->findData((int)back);
		if (index != -1 && index != choice->currentIndex()) {
			const QSignalBlocker block(choice);
			choice->setCurrentIndex(index);
		}
	};

	/* One closure writes the endpoint from the fields as they stand, and nothing else stages the model. The
	 * block's header line, this section's summary, the inline message and the Apply gate all come out of the
	 * same call, so they cannot end up describing different states. `report` is non-null when Apply/OK
	 * asked: the same text then also stops the save. */
	auto applyFields = [this, id, choice, serverEdit, providerList, keyEdit, note, problem, section, restate_header,
			    restate_choice, showFor, describe](QString *report) -> bool {
		showFor();
		describe();

		const int which = choice->currentData().toInt();
		QString error;
		std::string conflicting;
		bool ok = true;

		if (which == (int)EndpointChoice::CustomServer) {
			const QString server = serverEdit->text().trimmed();
			if (server.isEmpty()) {
				/* Half-typed. The destination goes to "no endpoint" rather than keeping a
				 * staged value the fields no longer show, and Apply says so below. */
				destinationEdits.clearService(id, false, nullptr, &error);
				ok = false;
				if (report)
					*report = QTStr("Basic.Settings.Stream.Destinations.Service.Error.NeedServer");
			} else {
				OBSDataAutoRelease settings = obs_data_create();
				obs_data_set_string(settings, "server", QT_TO_UTF8(server));
				obs_data_set_string(settings, "key", QT_TO_UTF8(keyEdit->text().trimmed()));
				const char *json = obs_data_get_json(settings);
				ok = destinationEdits.setService(id, "rtmp_custom", json ? json : "", &conflicting,
								 &error);
			}
		} else if (which == (int)EndpointChoice::KnownProvider) {
			const QString provider = providerList->currentText();
			const ProviderIngest ingest = ResolveProviderIngest(provider);

			/* Written the way the Stream settings page writes its own service: the provider, its
			 * protocol and the ingest it names, so the file says where the stream goes on its own. */
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_string(settings, "service", QT_TO_UTF8(provider));
			if (!ingest.protocol.isEmpty())
				obs_data_set_string(settings, "protocol", QT_TO_UTF8(ingest.protocol));
			obs_data_set_string(settings, "server", QT_TO_UTF8(ingest.server));
			obs_data_set_string(settings, "key", QT_TO_UTF8(keyEdit->text().trimmed()));
			const char *json = obs_data_get_json(settings);
			ok = destinationEdits.setService(id, "rtmp_common", json ? json : "", &conflicting, &error);
		} else {
			ok = destinationEdits.clearService(id, which == (int)EndpointChoice::ProfileDefault,
							   &conflicting, &error);
		}

		if (ok) {
			problem->setText(QString());
		} else if (!conflicting.empty()) {
			const StreamingDestination *other = destinationEdits.find(conflicting);
			const QString who = other ? QString::fromStdString(other->name)
						  : QString::fromStdString(conflicting);
			problem->setText(
				QTStr("Basic.Settings.Stream.Destinations.Service.Error.DuplicateNamed").arg(who));
			if (report)
				*report = QTStr("Basic.Settings.Stream.Destinations.Service.Error.DuplicateNamed")
						  .arg(who);

			/* The card must not go on offering the state that was just refused. */
			restate_choice();
			showFor();
			describe();
		} else {
			problem->setText(error);
			if (report && !error.isEmpty())
				*report = error;
		}

		/* The model holds whatever is usable - the last good endpoint when a field went bad - and that is
		 * what a connection would use, so it is what the two summary lines say. */
		section->setSummary(DestinationEndpointSummary(id));
		restate_header();
		TouchDestinations();
		return ok;
	};

	connect(choice, &QComboBox::currentIndexChanged, this, [applyFields](int) { applyFields(nullptr); });
	connect(serverEdit, &QLineEdit::textChanged, this, [applyFields](const QString &) { applyFields(nullptr); });
	connect(keyEdit, &QLineEdit::textChanged, this, [applyFields](const QString &) { applyFields(nullptr); });
	connect(providerList, &QComboBox::currentTextChanged, this,
		[applyFields](const QString &) { applyFields(nullptr); });

	/* The Apply/OK gate re-reads the fields, so a key that was typed but never left cannot be quietly
	 * dropped by the save. The entry goes away with the block it belongs to. */
	destinationServiceChecks[id] = [applyFields](QString *report) {
		return applyFields(report);
	};

	section->setSummary(DestinationEndpointSummary(id));
	return section;
}

void OBSBasicSettings::TouchDestinations()
{
	destinationsChanged = true;
	EnableApplyButton(true);
}

/* One line saying where a destination uploads, built from the model rather than from the fields: the model is
 * what a connection would use, a staged edit included, so the card's header line and the section's summary
 * cannot drift from the endpoint the engine will pick up on Apply. */
QString OBSBasicSettings::DestinationEndpointSummary(const std::string &id) const
{
	const auto text_of = [](const StreamingDestinations::ServiceEndpoint &ep) {
		QString where;
		if (!ep.provider.empty())
			where = QString::fromStdString(ep.provider);
		else if (!ep.server.empty())
			where = QString::fromStdString(ep.server);
		else
			where = QString::fromStdString(ep.type);
		if (!ep.key.empty())
			where += QTStr("Basic.Settings.Stream.Destinations.Service.KeySet");
		return where;
	};

	if (destinationEdits.ownsService(id)) {
		const auto ep = destinationEdits.serviceEndpoint(id);
		if (!ep.configured)
			return QTStr("Basic.Settings.Stream.Destinations.Service.Own.NotConfigured");
		return QTStr("Basic.Settings.Stream.Destinations.Service.Own").arg(text_of(ep));
	}

	if (destinationEdits.followsProfileService(id)) {
		const auto profile = destinationEdits.profileServiceEndpoint();
		if (!profile.configured)
			return QTStr("Basic.Settings.Stream.Destinations.Service.Profile.Missing");
		return QTStr("Basic.Settings.Stream.Destinations.Service.Profile").arg(text_of(profile));
	}

	return QTStr("Basic.Settings.Stream.Destinations.Service.NotConfigured");
}

/* The first endpoint a card leaves unusable, in a sentence that names the destination. Empty when every card
 * can be saved as it stands - Apply/OK refuses to write the profile otherwise, because the alternative is a
 * change that silently disappears. */
QString OBSBasicSettings::DestinationServiceProblems()
{
	for (const auto &entry : destinationServiceChecks) {
		QString problem;
		if (entry.second(&problem))
			continue;
		if (problem.isEmpty())
			problem = QTStr("Basic.Settings.Stream.Destinations.Service.Error.Incomplete");

		const StreamingDestination *dest = destinationEdits.find(entry.first);
		const QString who = dest ? QString::fromStdString(dest->name) : QString::fromStdString(entry.first);
		return QTStr("Basic.Settings.Stream.Destinations.Service.Error.Named").arg(who).arg(problem);
	}

	return QString();
}

/* The order of the blocks is not cosmetic: the first enabled destination is what the legacy streaming API
 * reports (Q42) and the primary connection of a session is the one whose id the handler records, so the up and
 * down buttons in the block headers have to write the new order back to the model. Drag-to-reorder used to do
 * the same job through the item view; the buttons cover it without the view's nested scroll area. */
void OBSBasicSettings::MoveDestination(const std::string &id, int delta)
{
	std::vector<std::string> order;
	for (const auto &dest : destinationEdits.destinations())
		order.push_back(dest.id);

	const auto it = std::find(order.begin(), order.end(), id);
	if (it == order.end())
		return;

	const auto from = static_cast<long>(it - order.begin());
	const auto to = from + delta;
	if (to < 0 || static_cast<size_t>(to) >= order.size())
		return;

	const std::string moved = order[static_cast<size_t>(from)];
	order.erase(order.begin() + from);
	order.insert(order.begin() + to, moved);

	if (destinationEdits.reorder(order))
		MarkDestinationsChanged();
}

/* Structural edits only touch the dialog's copy: Apply/OK commits it through SaveDestinationsSettings,
 * Cancel drops it - exactly like every other page of this dialog. */
void OBSBasicSettings::MarkDestinationsChanged()
{
	destinationsChanged = true;
	EnableApplyButton(true);

	/* The rebuild destroys the widgets that may have triggered this very call (the enable checkbox, a
	 * move button), so it runs on the next event-loop turn. */
	QTimer::singleShot(0, this, [this]() { RebuildDestinationBlocks(); });
}

void OBSBasicSettings::LoadDestinationsSettings()
{
	destinationEdits.Load(main->Config(), main->GetCurrentProfile().path);
}

void OBSBasicSettings::SaveDestinationsSettings()
{
	if (!destinationEdits.Save()) {
		blog(LOG_WARNING, "Failed to write the streaming destination list of this profile");
		return;
	}

	/* Publish to OBSBasic only once the ini keys and the files are in place, so a failed write leaves
	 * the rest of the application looking at the previous list. The changed flag itself is cleared by
	 * ClearChanged() in the caller, like every other page. */
	main->Destinations() = destinationEdits;

	/* Keep the runtime mirror (slice 2a) in step with the list that was just committed. */
	main->RefreshStreamRuntimes();
}

/* Name plus id in one dialog. The id is editable when adding (it is what the user will see in file names
 * and in shared_from references, so it is worth letting them choose it) and read-only when renaming,
 * because an id that other destinations reference must never change underneath them (Q22). */
bool OBSBasicSettings::AskDestinationNameAndId(QString &name, QString &id, bool idEditable, const QString &title)
{
	QDialog dialog(this);
	dialog.setWindowTitle(title);

	auto form = new QFormLayout(&dialog);
	auto nameEdit = new QLineEdit(name, &dialog);
	auto idEdit = new QLineEdit(id, &dialog);

	idEdit->setMaxLength(32);
	idEdit->setReadOnly(!idEditable);
	idEdit->setValidator(new QRegularExpressionValidator(QRegularExpression("[a-z0-9_-]*"), &dialog));

	auto hint = new QLabel(QTStr(idEditable ? "Basic.Settings.Stream.Destinations.Id.Help"
						: "Basic.Settings.Stream.Destinations.Id.Fixed"),
			       &dialog);
	hint->setWordWrap(true);

	auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	form->addRow(QTStr("Basic.Settings.Stream.Destinations.Add.Name"), nameEdit);
	form->addRow(QTStr("Basic.Settings.Stream.Destinations.Id.Label"), idEdit);
	form->addRow(hint);
	form->addRow(buttons);

	/* Suggest an id while the name is typed, and stop suggesting as soon as the user touches the id
	 * field themselves. */
	auto idTouched = QSharedPointer<bool>::create(!id.isEmpty());
	connect(idEdit, &QLineEdit::textEdited, this, [idTouched](const QString &) { *idTouched = true; });
	connect(nameEdit, &QLineEdit::textEdited, this, [this, nameEdit, idEdit, idTouched](const QString &) {
		if (*idTouched)
			return;
		const std::string suggestion = destinationEdits.makeId(nameEdit->text().trimmed().toStdString());
		idEdit->setText(QString::fromStdString(suggestion));
	});

	if (dialog.exec() != QDialog::Accepted)
		return false;

	name = nameEdit->text().trimmed();
	id = idEdit->text().trimmed();
	return true;
}

void OBSBasicSettings::AddDestination()
{
	QString name = QString();
	QString id = QString();
	if (!AskDestinationNameAndId(name, id, true, QTStr("Basic.Settings.Stream.Destinations.Add")))
		return;

	if (name.isEmpty() && id.isEmpty())
		return;
	if (name.isEmpty())
		name = id;

	QString error;
	StreamingDestination *dest = nullptr;
	if (id.isEmpty())
		dest = destinationEdits.create(name.toStdString(), name.toStdString(), &error);
	else
		dest = destinationEdits.createWithId(id.toStdString(), name.toStdString(), &error);

	if (!dest) {
		OBSMessageBox::warning(this, QTStr("Basic.Settings.Stream.Destinations.Add"), error);
		return;
	}

	blog(LOG_INFO, "Added streaming destination '%s' (id '%s')", dest->name.c_str(), dest->id.c_str());
	MarkDestinationsChanged();
}

void OBSBasicSettings::RenameDestination(const std::string &id)
{
	const StreamingDestination *dest = destinationEdits.find(id);
	if (!dest)
		return;

	QString name = QString::fromStdString(dest->name);
	QString fixedId = QString::fromStdString(dest->id);
	if (!AskDestinationNameAndId(name, fixedId, false, QTStr("Basic.Settings.Stream.Destinations.Rename")))
		return;

	const QString trimmed = name.trimmed();
	if (trimmed.isEmpty() || trimmed.toStdString() == dest->name)
		return;

	destinationEdits.setDisplayName(id, trimmed.toStdString());
	MarkDestinationsChanged();
}

void OBSBasicSettings::RemoveDestination(const std::string &id)
{
	const StreamingDestination *dest = destinationEdits.find(id);
	if (!dest)
		return;

	const QString name = QString::fromStdString(dest->name);

	if (OBSMessageBox::question(this, QTStr("Basic.Settings.Stream.Destinations.Remove"),
				    QTStr("Basic.Settings.Stream.Destinations.Remove.Confirm").arg(name)) !=
	    QMessageBox::Yes)
		return;

	QString error;
	if (!destinationEdits.destroy(id, &error)) {
		if (!error.isEmpty())
			OBSMessageBox::warning(this, QTStr("Basic.Settings.Stream.Destinations.Remove"), error);
		return;
	}

	blog(LOG_INFO, "Removed streaming destination '%s'", QT_TO_UTF8(name));
	MarkDestinationsChanged();
}

/* What each destination is doing in the running session. The state is read from the runtime entries OBSBasic
 * keeps for the session, so the page cannot drift out of step with what is actually connected, and it changes
 * nothing here: editing settings and driving the session stay separate things. With no session running every
 * label goes blank, which keeps the page looking the way it always did. */
void OBSBasicSettings::RefreshDestinationStates()
{
	if (destinationStateLabels.empty())
		return;

	OBSBasic *main = OBSBasic::Get();
	const bool streaming = main && obs_frontend_streaming_active();

	std::vector<OBSBasic::StreamSessionEntry> snapshot;
	if (streaming)
		snapshot = main->StreamSessionSnapshot(true);

	for (auto &entry : destinationStateLabels) {
		QLabel *label = entry.second;
		if (!label)
			continue;

		if (!streaming) {
			label->setText(QString());
			setClasses(label, "");
			continue;
		}

		const auto it =
			std::find_if(snapshot.begin(), snapshot.end(),
				     [&](const OBSBasic::StreamSessionEntry &e) { return e.id == entry.first; });
		if (it == snapshot.end()) {
			/* Not a connection of the session that is running: the destination is switched off, or it
			 * never got one. */
			label->setText(QTStr("Basic.Settings.Stream.Destinations.State.NotInSession"));
			setClasses(label, "text-warning");
			continue;
		}

		QString text;
		QString styling;
		switch (it->state) {
		case StreamDestinationRuntime::State::Live:
			text = QTStr("Basic.Settings.Stream.Destinations.State.Live");
			styling = "text-success";
			break;
		case StreamDestinationRuntime::State::DelayStarting:
			text = QTStr("Basic.Settings.Stream.Destinations.State.DelayStarting");
			styling = "text-warning";
			break;
		case StreamDestinationRuntime::State::Preparing:
		case StreamDestinationRuntime::State::Connecting:
			text = QTStr("Basic.Settings.Stream.Destinations.State.Connecting");
			styling = "text-warning";
			break;
		case StreamDestinationRuntime::State::Reconnecting:
			text = QTStr("Basic.Settings.Stream.Destinations.State.Reconnecting");
			styling = "text-danger";
			break;
		case StreamDestinationRuntime::State::Stopping:
		case StreamDestinationRuntime::State::DelayStopping:
			text = QTStr("Basic.Settings.Stream.Destinations.State.Stopping");
			break;
		case StreamDestinationRuntime::State::Stopped:
		default:
			/* Part of a session that is still running, but this connection is down: it stays that way
			 * until the user restarts it from the stats window. */
			text = QTStr("Basic.Settings.Stream.Destinations.State.Down");
			styling = "text-warning";
			break;
		}

		if (it->reconnectAttempt > 0)
			text += QTStr("Basic.Settings.Stream.Destinations.State.Attempt")
					.arg(QString::number(it->reconnectAttempt));

		label->setText(text);
		setClasses(label, styling);
	}
}
