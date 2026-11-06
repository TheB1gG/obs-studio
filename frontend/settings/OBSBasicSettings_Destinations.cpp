#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
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
#include <QSharedPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/* A titled, collapsible part of a destination block - the shape the mock uses for its sections. The header line
 * carries a one-liner summarising what is inside, so a collapsed block still answers the question the section
 * is about, which is what makes the whole list scannable. */
class DestinationSection : public QWidget {
public:
	explicit DestinationSection(const QString &title, QWidget *parent = nullptr) : QWidget(parent) {
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

		summary_ = new QLabel(this);
		summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		summary_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

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
	void setSummary(const QString &text) { summary_->setText(text); }

private:
	/* Lines the body up under the section title instead of under the chevron. */
	static constexpr int SECTION_INDENT = 24;

	QToolButton *toggle_ = nullptr;
	QLabel *summary_ = nullptr;
	QWidget *body_ = nullptr;
	QVBoxLayout *bodyLayout_ = nullptr;
};

/* A label that reads as secondary text: smaller rather than coloured, because the themes own the palette and a
 * hard-coded grey looks wrong in the light ones. */
QLabel *SecondaryLabel(const QString &text, QWidget *parent = nullptr)
{
	auto label = new QLabel(text, parent);
	QFont font = label->font();
	font.setPointSizeF(font.pointSizeF() * 0.9);
	label->setFont(font);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
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
		auto title = new QLabel(QString("%1 \xE2\x80\xA2 %2").arg(index + 1).arg(QString::fromStdString(dest.name)));
		QFont titleFont = title->font();
		titleFont.setBold(true);
		title->setFont(titleFont);

		auto summary = new QLabel;
		summary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

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
		 * anything, and the one that needs attention opens only the part that matters. They are added to the
		 * layout in the order their content is built below. */
		auto serviceSection = new DestinationSection(QTStr("Basic.Settings.Stream.Destinations.Section.Service"));
		detailsLayout->addWidget(serviceSection);

		const std::string service = destinations.serviceDescription(dest.id);
		const QString serviceText = service.empty() ? QTStr("Basic.Settings.Stream.Destinations.Service.NotConfigured")
							    : QString::fromStdString(service);
		/* The endpoint gets a line of its own: the section is called Service already, so repeating the word in
		 * front of the address only pushes the part worth reading further to the right. */
		serviceSection->bodyLayout()->addWidget(SecondaryLabel(serviceText));

		const size_t tracks = destinations.trackCount(dest.id);
		const QString tracksText = tracks == 0 ? QTStr("Basic.Settings.Stream.Destinations.Tracks.None")
						       : QTStr("Basic.Settings.Stream.Destinations.Tracks.Count").arg(int(tracks));

		/* What this destination will upload on its own. There is no capacity comparison here on
		 * purpose: the number is information, not a warning. */
		const auto bandwidth = destinations.bitrateFor(dest.id);
		QString bandwidthText;
		if (bandwidth.videoKbps == 0 && bandwidth.audioKbps == 0) {
			bandwidthText = QTStr("Basic.Settings.Stream.Destinations.Bitrate.None");
		} else {
			bandwidthText = QTStr("Basic.Settings.Stream.Destinations.Bitrate.Value")
					.arg(bandwidth.videoKbps)
					.arg(bandwidth.audioKbps)
					.arg(bandwidth.videoKbps + bandwidth.audioKbps);
			if (bandwidth.shared_video)
				bandwidthText += QTStr("Basic.Settings.Stream.Destinations.Bitrate.SharedVideo");
		}
		const QString totalText = bandwidth.videoKbps || bandwidth.audioKbps
						    ? QString("%1 kbps").arg(bandwidth.videoKbps + bandwidth.audioKbps)
						    : QString();

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
		uploadForm->addRow(SecondaryLabel(bandwidthText));
		uploadForm->addRow(QTStr("Basic.Settings.Stream.Destinations.Recording"),
			SecondaryLabel(QTStr("Basic.Settings.Stream.Destinations.Recording.Session")));

		/* Offered, not wired yet: the fetch needs the per-destination service object the engine step
		 * creates. Keeping the button here makes the intended flow visible. It keeps its own width: a
		 * button stretched across the whole page looks like a disabled text field, which is what it is. */
		auto fetchButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Fetch"));
		fetchButton->setEnabled(false);
		fetchButton->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Fetch.Pending"));
		auto fetchRow = new QHBoxLayout;
		fetchRow->setContentsMargins(0, 0, 0, 0);
		fetchRow->addWidget(fetchButton);
		fetchRow->addStretch();
		serviceSection->bodyLayout()->addLayout(fetchRow);

		/* The block's own line says which destination this is, where it goes and what it costs, so the
		 * collapsed list can be scanned for the one the user means. */
		QString headerText = QString::fromStdString(dest.id) + " \xE2\x80\xA2 " + serviceText;
		if (!totalText.isEmpty())
			headerText += " \xE2\x80\xA2 " + totalText;
		summary->setText(headerText);
		uploadSection->setSummary(totalText);

		/* What happens when the link drops. */
		auto connectionSection = new DestinationSection(QTStr("Basic.Settings.Stream.Destinations.Section.Connection"));
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

		DimBlockIfNeeded(block, !dest.enabled);
		destinationsBlocksLayout->addWidget(block);
	}

	if (destinationsEmptyLabel)
		destinationsEmptyLabel->setVisible(list.empty());

	if (destinationsTotalBitrateLabel) {
		const long long total = destinations.totalUploadKbps();
		destinationsTotalBitrateLabel->setText(
			total > 0 ? QTStr("Basic.Settings.Stream.Destinations.TotalUpload").arg(total)
				  : QTStr("Basic.Settings.Stream.Destinations.TotalUpload.None"));
		destinationsTotalBitrateLabel->setToolTip(
			QTStr("Basic.Settings.Stream.Destinations.TotalUpload.ToolTip"));
	}
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
