#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGraphicsOpacityEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSharedPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

/* The destinations are shown as one block per destination inside a QListWidget, because that gives
 * drag-to-reorder (InternalMove) for free: the item carries the destination id in Qt::UserRole, so the
 * visual order can be written back to the model after a drop. The up/down buttons do the same thing
 * without a mouse, which QListWidget dragging cannot offer. */
class DestinationListWidget : public QListWidget {
public:
	using QListWidget::QListWidget;

	std::function<void()> reordered;

protected:
	void dropEvent(QDropEvent *event) override
	{
		QListWidget::dropEvent(event);
		if (reordered)
			reordered();
	}
};

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

	auto intro = new QLabel(QTStr("Basic.Settings.Stream.Destinations.Desc"));
	intro->setWordWrap(true);
	destinationsPageLayout->addWidget(intro);

	destinationsEmptyLabel = new QLabel(QTStr("Basic.Settings.Stream.Destinations.Empty"));
	destinationsEmptyLabel->setWordWrap(true);
	destinationsEmptyLabel->setAlignment(Qt::AlignCenter);
	destinationsPageLayout->addWidget(destinationsEmptyLabel);

	auto list = new DestinationListWidget;
	list->setFrameShape(QFrame::NoFrame);
	list->setResizeMode(QListWidget::Adjust);
	list->setSpacing(6);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	list->setDragDropMode(QAbstractItemView::InternalMove);
	list->setDefaultDropAction(Qt::MoveAction);
	list->reordered = [this]() {
		/* Still inside the view's own move handling here: rebuilding destroys the items the view is
		 * working with, so it has to wait for the next event-loop turn. */
		QTimer::singleShot(0, this, [this]() { ApplyDestinationOrder(); });
	};
	destinationsList = list;
	destinationsPageLayout->addWidget(list, 1);

	auto bar = new QHBoxLayout;
	addDestinationButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Add"));
	connect(addDestinationButton, &QPushButton::clicked, this, &OBSBasicSettings::AddDestination);
	bar->addWidget(addDestinationButton);
	bar->addStretch();
	destinationsTotalBitrateLabel = new QLabel;
	bar->addWidget(destinationsTotalBitrateLabel);
	destinationsPageLayout->addLayout(bar);

	LoadDestinationsSettings();
	RebuildDestinationBlocks();
}

void OBSBasicSettings::RebuildDestinationBlocks()
{
	if (!destinationsList)
		return;

	destinationsList->clear();

	StreamingDestinations &destinations = destinationEdits;
	const auto list = destinations.destinations();
	const int count = static_cast<int>(list.size());

	for (int index = 0; index < count; index++) {
		const StreamingDestination &dest = list[index];
		const std::string id = dest.id;

		auto item = new QListWidgetItem(destinationsList);
		item->setData(Qt::UserRole, QString::fromStdString(id));

		auto group = new QGroupBox;
		auto groupLayout = new QVBoxLayout(group);

		auto header = new QHBoxLayout;

		auto moveUp = new QToolButton;
		moveUp->setArrowType(Qt::UpArrow);
		moveUp->setToolTip(QTStr("Basic.Settings.Stream.Destinations.MoveUp"));
		moveUp->setEnabled(index > 0);
		connect(moveUp, &QToolButton::clicked, this, [this, id]() { MoveDestination(id, -1); });

		auto moveDown = new QToolButton;
		moveDown->setArrowType(Qt::DownArrow);
		moveDown->setToolTip(QTStr("Basic.Settings.Stream.Destinations.MoveDown"));
		moveDown->setEnabled(index + 1 < count);
		connect(moveDown, &QToolButton::clicked, this, [this, id]() { MoveDestination(id, 1); });

		auto toggle = new QToolButton;
		toggle->setCheckable(true);
		toggle->setChecked(true);
		toggle->setArrowType(Qt::DownArrow);
		toggle->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Toggle"));

		auto title = new QLabel(QString::fromStdString(dest.name));
		QFont titleFont = title->font();
		titleFont.setBold(true);
		title->setFont(titleFont);

		auto idLabel =
			new QLabel(QTStr("Basic.Settings.Stream.Destinations.Id").arg(QString::fromStdString(dest.id)));

		auto enable = new QCheckBox(QTStr("Basic.Settings.Stream.Destinations.Enable"));
		enable->setChecked(dest.enabled);
		connect(enable, &QCheckBox::toggled, this, [this, id](bool on) {
			destinationEdits.setEnabled(id, on);
			MarkDestinationsChanged();
		});

		auto renameButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Rename"));
		connect(renameButton, &QPushButton::clicked, this, [this, id]() { RenameDestination(id); });

		auto removeButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Remove"));
		connect(removeButton, &QPushButton::clicked, this, [this, id]() { RemoveDestination(id); });

		header->addWidget(moveUp);
		header->addWidget(moveDown);
		header->addWidget(toggle);
		header->addWidget(title);
		header->addWidget(idLabel);
		header->addStretch();
		header->addWidget(enable);
		header->addWidget(renameButton);
		header->addWidget(removeButton);
		groupLayout->addLayout(header);

		auto body = new QWidget;
		auto bodyLayout = new QGridLayout(body);
		bodyLayout->setContentsMargins(0, 0, 0, 0);

		int row = 0;
		auto addInfo = [&](const QString &label, const QString &value) {
			bodyLayout->addWidget(new QLabel(label), row, 0);
			auto *valueLabel = new QLabel(value);
			valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
			bodyLayout->addWidget(valueLabel, row, 1);
			row++;
		};

		const std::string service = destinations.serviceDescription(dest.id);
		addInfo(QTStr("Basic.Settings.Stream.Destinations.Service"),
			service.empty() ? QTStr("Basic.Settings.Stream.Destinations.Service.NotConfigured")
					: QString::fromStdString(service));

		const size_t tracks = destinations.trackCount(dest.id);
		addInfo(QTStr("Basic.Settings.Stream.Destinations.Tracks"),
			tracks == 0 ? QTStr("Basic.Settings.Stream.Destinations.Tracks.None")
				    : QTStr("Basic.Settings.Stream.Destinations.Tracks.Count").arg(int(tracks)));

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
		addInfo(QTStr("Basic.Settings.Stream.Destinations.Bitrate"), bandwidthText);

		addInfo(QTStr("Basic.Settings.Stream.Destinations.Recording"),
			QTStr("Basic.Settings.Stream.Destinations.Recording.Session"));

		/* Offered, not wired yet: the fetch needs the per-destination service object the engine step
		 * creates. Keeping the button here makes the intended flow visible. */
		auto fetchButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Fetch"));
		fetchButton->setEnabled(false);
		fetchButton->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Fetch.Pending"));
		bodyLayout->addWidget(fetchButton, row, 0, 1, 2, Qt::AlignLeft);
		row++;

		/* Q14: reconnect values per destination. Only the values can differ - the attempt counter, the
		 * backoff timer and the thread waiting on it already belong to one output each in libobs, so an
		 * override here can never spend another destination's retries. With the override off the
		 * destination follows the profile-wide values from the hidden Advanced settings, which is what
		 * every destination does today. */
		const auto profile_policy = destinations.profileReconnectPolicy();
		const auto policy = destinations.reconnectPolicy(dest.id);

		auto reconnectOverride = new QCheckBox(QTStr("Basic.Settings.Stream.Destinations.Reconnect.Override"));
		reconnectOverride->setChecked(policy.overridden);
		reconnectOverride->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Reconnect.ToolTip"));
		bodyLayout->addWidget(reconnectOverride, row, 0, 1, 2);
		row++;

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
		bodyLayout->addWidget(reconnectDetail, row, 0, 1, 2);
		row++;

		/* What the destination uses when it is not overriding anything; hidden the moment the values
		 * stop being the profile's. */
		auto reconnectSummary = new QLabel(
			profile_policy.enabled ? QTStr("Basic.Settings.Stream.Destinations.Reconnect.Default")
							 .arg(profile_policy.retryDelaySec)
							 .arg(profile_policy.maxRetries)
					       : QTStr("Basic.Settings.Stream.Destinations.Reconnect.DefaultOff"));
		bodyLayout->addWidget(reconnectSummary, row, 1);
		row++;

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
		/* The list item keeps a fixed size hint, so a sub-block that appears inside the block has to
		 * resize its item too or the new rows are clipped. Deferred one event-loop turn so the layout
		 * has settled by the time the hint is taken. */
		connect(reconnectOverride, &QCheckBox::toggled, this, [this, item, group](bool) {
			QTimer::singleShot(0, this, [item, group]() { item->setSizeHint(group->sizeHint()); });
		});
		connect(reconnectEnable, &QCheckBox::toggled, this, pushReconnect);
		connect(retryDelay, &QSpinBox::valueChanged, this, pushReconnect);
		connect(maxRetries, &QSpinBox::valueChanged, this, pushReconnect);

		reconnectDetail->setVisible(policy.overridden);
		reconnectSummary->setVisible(!policy.overridden);

		groupLayout->addWidget(body);

		connect(toggle, &QToolButton::toggled, body, &QWidget::setVisible);
		connect(toggle, &QToolButton::toggled, group,
			[toggle](bool on) { toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow); });

		DimBlockIfNeeded(group, !dest.enabled);

		item->setSizeHint(group->sizeHint());
		destinationsList->setItemWidget(item, group);
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

/* The order of the list is not cosmetic: the first enabled destination is what the legacy streaming API
 * reports (Q42), so both the drag and the buttons have to write it back to the model. */
void OBSBasicSettings::ApplyDestinationOrder()
{
	if (!destinationsList)
		return;

	std::vector<std::string> order;
	for (int i = 0; i < destinationsList->count(); i++) {
		auto item = destinationsList->item(i);
		if (item)
			order.push_back(item->data(Qt::UserRole).toString().toStdString());
	}

	if (order.size() != static_cast<size_t>(destinationsList->count()))
		return;

	if (destinationEdits.reorder(order))
		MarkDestinationsChanged();
}

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
