#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

/* Build the widgets of the destinations page inside the page that the .ui provides. The blocks
 * themselves are created in code because their number is whatever the profile has. */
void OBSBasicSettings::SetupDestinationsPage()
{
	destinationsPageLayout = ui->destinationsPageLayout;

	auto intro = new QLabel(QTStr("Basic.Settings.Stream.Destinations.Desc"));
	intro->setWordWrap(true);
	destinationsPageLayout->addWidget(intro);

	destinationsScrollArea = new QScrollArea;
	destinationsScrollArea->setWidgetResizable(true);
	destinationsScrollArea->setFrameShape(QFrame::NoFrame);

	destinationsListBody = new QWidget;
	destinationsListLayout = new QVBoxLayout(destinationsListBody);

	destinationsEmptyLabel = new QLabel(QTStr("Basic.Settings.Stream.Destinations.Empty"));
	destinationsEmptyLabel->setWordWrap(true);
	destinationsListLayout->addWidget(destinationsEmptyLabel);
	destinationsListLayout->addStretch();

	destinationsScrollArea->setWidget(destinationsListBody);
	destinationsPageLayout->addWidget(destinationsScrollArea, 1);

	auto bar = new QHBoxLayout;
	addDestinationButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Add"));
	connect(addDestinationButton, &QPushButton::clicked, this, &OBSBasicSettings::AddDestination);
	bar->addWidget(addDestinationButton);
	bar->addStretch();
	destinationsPageLayout->addLayout(bar);

	LoadDestinationsSettings();
	RebuildDestinationBlocks();
}

void OBSBasicSettings::RebuildDestinationBlocks()
{
	if (!destinationsListLayout)
		return;

	for (QPointer<QWidget> &block : destinationBlocks) {
		if (block) {
			/* hide + remove first, deleteLater second: a rebuild can be triggered from a slot of
			 * one of these very widgets, so deleting it right there would be a use-after-free. */
			block->hide();
			destinationsListLayout->removeWidget(block);
			block->deleteLater();
		}
	}
	destinationBlocks.clear();

	StreamingDestinations &destinations = destinationEdits;
	const auto &list = destinations.destinations();

	for (const StreamingDestination &dest : list) {
		const std::string id = dest.id;

		auto group = new QGroupBox;
		auto groupLayout = new QVBoxLayout(group);

		auto header = new QHBoxLayout;

		auto toggle = new QToolButton;
		toggle->setCheckable(true);
		toggle->setChecked(true);
		toggle->setArrowType(Qt::DownArrow);
		toggle->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Toggle"));

		auto title = new QLabel(QString::fromStdString(dest.name));
		QFont titleFont = title->font();
		titleFont.setBold(true);
		title->setFont(titleFont);

		auto idLabel = new QLabel(QTStr("Basic.Settings.Stream.Destinations.Id")
						   .arg(QString::fromStdString(dest.id)));

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

		addInfo(QTStr("Basic.Settings.Stream.Destinations.Recording"),
			QTStr("Basic.Settings.Stream.Destinations.Recording.Session"));

		/* Offered, not wired yet: the fetch needs the per-destination service object that comes
		 * with the engine step. Keeping the button here makes the intended flow visible. */
		auto fetchButton = new QPushButton(QTStr("Basic.Settings.Stream.Destinations.Fetch"));
		fetchButton->setEnabled(false);
		fetchButton->setToolTip(QTStr("Basic.Settings.Stream.Destinations.Fetch.Pending"));
		bodyLayout->addWidget(fetchButton, row, 0, 1, 2, Qt::AlignLeft);

		groupLayout->addWidget(body);

		connect(toggle, &QToolButton::toggled, body, &QWidget::setVisible);
		connect(toggle, &QToolButton::toggled, group, [toggle](bool on) {
			toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
		});

		/* The layout ends with a stretch item; blocks go in before it so the list stays top-aligned. */
		destinationsListLayout->insertWidget(destinationsListLayout->count() - 1, group);
		destinationBlocks.push_back(group);
	}

	if (destinationsEmptyLabel)
		destinationsEmptyLabel->setVisible(list.empty());
}

/* Structural edits only touch the dialog's copy of the list; Apply/OK decides whether they reach the
 * profile (SaveDestinationsSettings, called by SaveSettings) or are dropped, exactly like the other
 * pages. The blocks are rebuilt immediately so the change is visible before applying. */
void OBSBasicSettings::MarkDestinationsChanged()
{
	destinationsChanged = true;
	EnableApplyButton(true);
	RebuildDestinationBlocks();
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
}

void OBSBasicSettings::AddDestination()
{
	bool ok = false;
	const QString text = QInputDialog::getText(this, QTStr("Basic.Settings.Stream.Destinations.Add"),
						   QTStr("Basic.Settings.Stream.Destinations.Add.Name"),
						   QLineEdit::Normal, QString(), &ok);
	if (!ok)
		return;

	const QString name = text.trimmed();
	if (name.isEmpty())
		return;

	const std::string typed = name.toStdString();
	StreamingDestination *dest = destinationEdits.create(typed, typed, nullptr);
	if (!dest)
		return;

	blog(LOG_INFO, "Added streaming destination '%s' (id '%s')", dest->name.c_str(), dest->id.c_str());
	MarkDestinationsChanged();
}

void OBSBasicSettings::RenameDestination(const std::string &id)
{
	const StreamingDestination *dest = destinationEdits.find(id);
	if (!dest)
		return;

	bool ok = false;
	const QString text = QInputDialog::getText(this, QTStr("Basic.Settings.Stream.Destinations.Rename"),
						   QTStr("Basic.Settings.Stream.Destinations.Add.Name"),
						   QLineEdit::Normal, QString::fromStdString(dest->name), &ok);
	if (!ok)
		return;

	const QString name = text.trimmed();
	if (name.isEmpty() || name.toStdString() == dest->name)
		return;

	destinationEdits.setDisplayName(id, name.toStdString());
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
