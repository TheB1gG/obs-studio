#include "SceneTree.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QTimer>
#include <QDropEvent>
#include <QMimeData>
#include <QStyle>
#include <QPainter>
#include <QPixmap>
#include <QApplication>
#include <QPushButton>
#include <functional>
#include <algorithm>

// Custom data roles for scene items
enum SceneItemRole {
	SceneNameRole = Qt::UserRole + 10,
	SceneObsRefRole = Qt::UserRole + 11,
	ItemTypeRole = Qt::UserRole + 12, // "scene" or "folder"
	SceneColorRole = Qt::UserRole + 13,
	FolderSortModeRole = Qt::UserRole + 14, // SceneSortMode stored as int
	SceneLastUsedRole = Qt::UserRole + 15,  // int counter for last-used ordering
	FolderDisplayModeRole = Qt::UserRole + 16, // FolderDisplayMode stored as int
	FolderCompactLimitRole = Qt::UserRole + 17, // int: how many non-pinned to show in Compact mode
	ScenePinnedRole = Qt::UserRole + 18,     // bool: whether scene is pinned
};

static constexpr const char *kItemTypeScene = "scene";
static constexpr const char *kItemTypeFolder = "folder";
static constexpr const char *kMimeFormat = "application/x-obs-scenetree";

// Helper: create a small filled-circle icon for pin/compact indicators
static QIcon MakeDotIcon(const QColor &color, int size = 10)
{
	QPixmap pm(size, size);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawEllipse(1, 1, size - 2, size - 2);
	return QIcon(pm);
}

// Helper: composite a small dot onto an existing icon (for compact folder indicator)
static QIcon MakeCompactFolderIcon(const QIcon &baseIcon, const QColor &dotColor)
{
	QPixmap base = baseIcon.pixmap(16, 16);
	QPixmap pm(base.size());
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.drawPixmap(0, 0, base);
	// Draw a small dot in the bottom-right corner
	int dotSize = 6;
	int x = base.width() - dotSize;
	int y = base.height() - dotSize;
	p.setPen(Qt::NoPen);
	p.setBrush(dotColor);
	p.drawEllipse(x, y, dotSize, dotSize);
	return QIcon(pm);
}

// ============================================================================
// SceneTreeModel — handles drag-and-drop moves via dropMimeData()
// ============================================================================

SceneTreeModel::SceneTreeModel(QObject *parent) : QStandardItemModel(parent)
{
	setHorizontalHeaderLabels({QString()});
}

Qt::DropActions SceneTreeModel::supportedDropActions() const
{
	return Qt::MoveAction;
}

Qt::ItemFlags SceneTreeModel::flags(const QModelIndex &index) const
{
	if (!index.isValid())
		return Qt::ItemIsDropEnabled;

	return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
}

QStringList SceneTreeModel::mimeTypes() const
{
	return QStringList() << kMimeFormat;
}

QMimeData *SceneTreeModel::mimeData(const QModelIndexList &indexes) const
{
	if (indexes.isEmpty())
		return nullptr;

	QMimeData *mimeData = new QMimeData();

	// Collect unique items, discarding children of folders also being dragged
	QList<QStandardItem *> items;
	for (const QModelIndex &index : indexes) {
		if (index.column() != 0 || !index.isValid())
			continue;
		if (QStandardItem *item = itemFromIndex(index)) {
			if (!items.contains(item))
				items.append(item);
		}
	}

	QList<QStandardItem *> topLevel;
	for (QStandardItem *item : items) {
		bool insideDraggedFolder = false;
		for (QStandardItem *ancestor = item->parent(); ancestor; ancestor = ancestor->parent()) {
			if (items.contains(ancestor)) {
				insideDraggedFolder = true;
				break;
			}
		}
		if (!insideDraggedFolder)
			topLevel.append(item);
	}

	if (topLevel.isEmpty()) {
		delete mimeData;
		return nullptr;
	}

	QByteArray bytes;
	const int count = topLevel.size();
	bytes.append(reinterpret_cast<const char *>(&count), sizeof(int));
	for (QStandardItem *item : topLevel) {
		bytes.append(reinterpret_cast<const char *>(&item), sizeof(QStandardItem *));
	}

	mimeData->setData(kMimeFormat, bytes);
	return mimeData;
}

bool SceneTreeModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                  const QModelIndex &parent)
{
	Q_UNUSED(action)
	Q_UNUSED(column)

	if (!data->hasFormat(kMimeFormat))
		return false;

	// Resolve the target parent item
	QStandardItem *parentItem = itemFromIndex(parent);
	if (!parentItem)
		parentItem = invisibleRootItem();

	// If dropping on a scene (not a folder), place beside it in its parent
	if (parentItem->data(ItemTypeRole).toString() == kItemTypeScene) {
		QStandardItem *sceneParent = parentItem->parent();
		if (!sceneParent)
			sceneParent = invisibleRootItem();
		row = parentItem->row() + 1;
		parentItem = sceneParent;
	}

	// Ensure valid row
	if (row < 0)
		row = parentItem->rowCount();

	QByteArray bytes = data->data(kMimeFormat);
	if (bytes.size() < static_cast<int>(sizeof(int)))
		return false;

	const char *ptr = bytes.constData();
	const int count = *reinterpret_cast<const int *>(ptr);
	ptr += sizeof(int);

	for (int i = 0; i < count && (ptr + sizeof(QStandardItem *)) <= (bytes.constData() + bytes.size()); ++i) {
		QStandardItem *originalItem = *reinterpret_cast<QStandardItem * const *>(ptr);
		ptr += sizeof(QStandardItem *);

		if (!originalItem)
			continue;

		// Prevent dropping a folder into its own subtree
		bool intoItself = false;
		for (QStandardItem *walk = parentItem; walk; walk = walk->parent()) {
			if (walk == originalItem) {
				intoItself = true;
				break;
			}
		}
		if (intoItself)
			continue;

		QStandardItem *oldParent = originalItem->parent();
		if (!oldParent)
			oldParent = invisibleRootItem();
		const int oldRow = originalItem->row();

		// Use takeRow for atomic removal + retrieval
		QList<QStandardItem *> taken = oldParent->takeRow(oldRow);
		if (taken.isEmpty())
			continue;

		// Adjust row if we removed from the same parent above the target
		if (oldParent == parentItem && oldRow < row)
			--row;

		row = std::clamp(row, 0, parentItem->rowCount());
		parentItem->insertRow(row, taken);
		++row;
	}

	emit modelChanged();
	return true;
}

// ============================================================================
// SceneTreeView — lets Qt's normal InternalMove flow work
// ============================================================================

SceneTreeView::SceneTreeView(QWidget *parent) : QTreeView(parent) {}

void SceneTreeView::startDrag(Qt::DropActions supportedActions)
{
	// Prune selection: if a folder and its children are both selected,
	// deselect the children (they travel with the folder).
	if (QItemSelectionModel *selection = selectionModel()) {
		const QModelIndexList selected = selection->selectedRows();
		for (const QModelIndex &index : selected) {
			for (QModelIndex walk = index.parent(); walk.isValid(); walk = walk.parent()) {
				if (selected.contains(walk)) {
					selection->select(index, QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
					break;
				}
			}
		}
	}

	QTreeView::startDrag(supportedActions);
}

void SceneTreeView::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasFormat(kMimeFormat))
		event->acceptProposedAction();
	QTreeView::dragEnterEvent(event);
}

void SceneTreeView::dragMoveEvent(QDragMoveEvent *event)
{
	if (event->mimeData()->hasFormat(kMimeFormat))
		event->acceptProposedAction();
	else
		event->ignore();
	QTreeView::dragMoveEvent(event);
}

void SceneTreeView::dropEvent(QDropEvent *event)
{
	// Let the base class handle the drop — this calls model->dropMimeData()
	// which performs the actual move.
	QTreeView::dropEvent(event);

	// The model has already moved the rows. Tell the view to treat this as a
	// copy so it doesn't try to delete the "originals" (which are already gone).
	if (event->isAccepted())
		event->setDropAction(Qt::CopyAction);
}

void SceneTreeView::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		QModelIndex index = indexAt(event->pos());
		if (index.isValid()) {
			// Check if click is in the branch/arrow area
			int depth = 0;
			QModelIndex parent = index.parent();
			while (parent.isValid()) { depth++; parent = parent.parent(); }
			int branchWidth = (depth + 1) * indentation();
			if (event->pos().x() < branchWidth) {
				emit branchClicked(index);
				event->accept();
				return;
			}
		}
	}
	QTreeView::mousePressEvent(event);
}

// ============================================================================
// CustomColorDelegate — paints coloured row backgrounds for items with a
// colour set in SceneColorRole. Rows without a colour use the default style.
// ============================================================================

CustomColorDelegate::CustomColorDelegate(SceneTreeModel *model, QSortFilterProxyModel *proxy, QObject *parent)
	: QStyledItemDelegate(parent), model(model), proxy(proxy) {}

void CustomColorDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	if (!index.isValid() || !model) {
		QStyledItemDelegate::paint(painter, option, index);
		return;
	}

	// Map from proxy index to source index
	QModelIndex sourceIndex = index;
	if (proxy)
		sourceIndex = proxy->mapToSource(index);

	QStandardItem *item = model->itemFromIndex(sourceIndex);
	if (!item) {
		QStyledItemDelegate::paint(painter, option, index);
		return;
	}

	QColor customColor = item->data(SceneColorRole).value<QColor>();

	if (!customColor.isValid()) {
		// No colour set — let the theme paint this row normally.
		QStyledItemDelegate::paint(painter, option, index);
	} else {
		const bool isSelected = option.state & QStyle::State_Selected;
		const bool isHovered = option.state & QStyle::State_MouseOver;

		QColor bgColor = customColor;
		if (isSelected) {
			bgColor = customColor.lighter(120);
		} else if (isHovered) {
			bgColor = customColor.lighter(110);
		}

		// Ensure text contrast
		QColor textColor = (bgColor.lightness() > 160) ? QColor(30, 30, 30) : QColor(240, 240, 240);

		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);

		QRect rect = option.rect;
		rect.adjust(2, 1, -2, -1);
		painter->setPen(Qt::NoPen);
		painter->setBrush(bgColor);
		painter->drawRoundedRect(rect, 4, 4);
		painter->restore();

		// Let the base class paint icon + text with our contrast colour
		QStyleOptionViewItem modifiedOption = option;
		modifiedOption.palette.setColor(QPalette::Text, textColor);
		modifiedOption.palette.setColor(QPalette::HighlightedText, textColor);
		modifiedOption.backgroundBrush = QBrush(Qt::NoBrush);
		modifiedOption.state &= ~QStyle::State_HasFocus;
		modifiedOption.state &= ~QStyle::State_Selected;
		modifiedOption.state &= ~QStyle::State_MouseOver;

		QStyledItemDelegate::paint(painter, modifiedOption, index);
	}

	// Paint a dot indicator for folders in Compact mode (replaces the expand arrow)
	if (item->data(ItemTypeRole).toString() == kItemTypeFolder &&
	    item->data(FolderDisplayModeRole).toInt() == static_cast<int>(FolderDisplayMode::Compact)) {
		// Calculate depth from the proxy index
		int depth = 0;
		QModelIndex parent = index.parent();
		while (parent.isValid()) {
			depth++;
			parent = parent.parent();
		}

		const int indent = 16; // matches treeView->setIndentation(16)
		const int arrowX = depth * indent + indent / 2;
		const int centerY = option.rect.top() + option.rect.height() / 2;

		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);

		// Erase the existing arrow by painting a small rect with background color
		QColor bgColor = option.palette.color(QPalette::Base);
		painter->setPen(Qt::NoPen);
		painter->setBrush(bgColor);
		painter->drawRect(arrowX - 6, centerY - 6, 12, 12);

		// Paint the dot
		QColor dotColor = option.palette.color(QPalette::Text);
		painter->setBrush(dotColor);
		painter->drawEllipse(QPoint(arrowX, centerY), 3, 3);
		painter->restore();
	}

	// Paint a pin indicator for pinned scenes (small golden dot before text)
	if (item->data(ItemTypeRole).toString() == kItemTypeScene &&
	    item->data(ScenePinnedRole).toBool()) {
		int depth = 0;
		QModelIndex parent = index.parent();
		while (parent.isValid()) {
			depth++;
			parent = parent.parent();
		}

		const int indent = 16;
		const int textX = option.rect.left() + (depth + 1) * indent + 4;
		const int centerY = option.rect.top() + option.rect.height() / 2;

		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);
		painter->setPen(Qt::NoPen);
		painter->setBrush(QColor(255, 193, 7)); // Golden/amber pin color
		painter->drawEllipse(QPoint(textX - 2, centerY), 3, 3);
		painter->restore();
	}
}

// ============================================================================
// SceneTreeProxyModel — adds Compact-mode filtering on top of text search
// ============================================================================

SceneTreeProxyModel::SceneTreeProxyModel(QObject *parent) : QSortFilterProxyModel(parent) {}

QModelIndex SceneTreeProxyModel::mapToSource(const QModelIndex &proxyIndex) const
{
	if (!proxyIndex.isValid())
		return {};
	// Silently return invalid if the index doesn't belong to this proxy
	// (prevents Qt's "index from wrong model passed to mapToSource" warning)
	if (proxyIndex.model() != static_cast<const QAbstractItemModel *>(this))
		return {};
	return QSortFilterProxyModel::mapToSource(proxyIndex);
}

bool SceneTreeProxyModel::filterAcceptsRow(int row, const QModelIndex &parent) const
{
	// First apply the standard text filter (search)
	if (!QSortFilterProxyModel::filterAcceptsRow(row, parent))
		return false;

	// Get the source model
	auto *srcModel = static_cast<SceneTreeModel *>(const_cast<QAbstractItemModel *>(QSortFilterProxyModel::sourceModel()));
	if (!srcModel)
		return true;

	// 'parent' is a SOURCE model index (Qt passes source indices to filterAcceptsRow)
	QStandardItem *parentItem = srcModel->itemFromIndex(parent);
	if (!parentItem)
		parentItem = srcModel->invisibleRootItem();

	// Only apply Compact filtering to folder items
	if (parentItem->data(Qt::UserRole + 12).toString() != "folder")
		return true;

	int displayMode = parentItem->data(Qt::UserRole + 16).toInt();
	if (displayMode != 1) // Not Compact mode
		return true;

	// Get the child item at this row
	QStandardItem *child = parentItem->child(row, 0);
	if (!child)
		return false;

	// Pinned items are always visible
	bool isPinned = child->data(Qt::UserRole + 18).toBool();
	if (isPinned)
		return true;

	// Non-pinned: count how many non-pinned siblings before this row are visible
	int limit = parentItem->data(Qt::UserRole + 17).toInt();
	if (limit <= 0)
		limit = 3;

	int nonPinnedVisible = 0;
	for (int i = 0; i < row; i++) {
		QStandardItem *sibling = parentItem->child(i, 0);
		if (!sibling)
			continue;
		if (!sibling->data(Qt::UserRole + 18).toBool()) {
			// This sibling is non-pinned — check if it would be accepted by text filter
			if (QSortFilterProxyModel::filterAcceptsRow(i, parent)) {
				nonPinnedVisible++;
				if (nonPinnedVisible >= limit)
					return false; // Over the limit
			}
		}
	}

	return true;
}

// ============================================================================
// SceneTree — main component
// ============================================================================

SceneTree::SceneTree(QWidget *parent_) : QWidget(parent_)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// Create model and proxy
	model = new SceneTreeModel(this);
	proxyModel = new SceneTreeProxyModel(this);
	proxyModel->setSourceModel(model);
	proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
	proxyModel->setRecursiveFilteringEnabled(true);

	// Create tree view
	treeView = new SceneTreeView(this);
	treeView->setModel(proxyModel);
	treeView->setFrameShape(QFrame::NoFrame);
	treeView->setDragDropMode(QAbstractItemView::InternalMove);
	treeView->setDefaultDropAction(Qt::MoveAction);
	treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	treeView->setContextMenuPolicy(Qt::CustomContextMenu);
	treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
	treeView->setDropIndicatorShown(true);
	treeView->setExpandsOnDoubleClick(false);
	treeView->header()->hide();
	treeView->setIndentation(16);
	treeView->setRootIsDecorated(true);

	// Install colour delegate
	auto *colorDelegate = new CustomColorDelegate(model, proxyModel, this);
	treeView->setItemDelegate(colorDelegate);

	layout->addWidget(treeView);

	// Search bar at the bottom
	auto *searchEdit = new QLineEdit(this);
	searchEdit->setPlaceholderText("Search scenes...");
	searchEdit->setClearButtonEnabled(true);
	searchEdit->setFrame(false);
	auto *searchLayout = new QHBoxLayout();
	searchLayout->setContentsMargins(4, 2, 4, 2);
	searchLayout->addWidget(searchEdit);
	layout->addLayout(searchLayout);

	connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged,
	        this, &SceneTree::onItemSelectionChanged);
	connect(treeView, &QTreeView::clicked, this, [this](const QModelIndex &index) {
		// Only mark scene active on left-click (not right-click for context menu)
		QModelIndex sourceIndex = proxyModel->mapToSource(index);
		if (QStandardItem *item = model->itemFromIndex(sourceIndex)) {
			if (IsSceneItem(item)) {
				obs_scene_t *scene = static_cast<obs_scene_t *>(item->data(SceneObsRefRole).value<void *>());
				MarkSceneActive(scene);
			}
		}
	});

	// Arrow click cycles display mode: Collapsed → Expanded → Compact → Collapsed → ...
	connect(treeView, &SceneTreeView::branchClicked, this, [this](const QModelIndex &index) {
		QModelIndex sourceIndex = proxyModel->mapToSource(index);
		if (!sourceIndex.isValid())
			return;
		QStandardItem *item = model->itemFromIndex(sourceIndex);
		if (!item || !IsFolderItem(item))
			return;

		// Cycle: Collapsed → Expanded → Compact → Collapsed
		bool isExpanded = treeView->isExpanded(index);
		int currentMode = item->data(FolderDisplayModeRole).toInt();

		if (!isExpanded) {
			// Currently collapsed → go to Expanded
			SetFolderDisplayMode(item, FolderDisplayMode::Expanded);
		} else if (currentMode == static_cast<int>(FolderDisplayMode::Expanded)) {
			// Currently Expanded → go to Compact
			SetFolderDisplayMode(item, FolderDisplayMode::Compact);
		} else {
			// Currently Compact → go to Collapsed
			SetFolderDisplayMode(item, FolderDisplayMode::Collapsed);
		}
	});
	connect(treeView, &QWidget::customContextMenuRequested, this,
	        &SceneTree::customContextMenuRequested);
	connect(model, &SceneTreeModel::modelChanged, this, [this]() {
		ApplyAllSorts();
		emit scenesReordered();
	});
	connect(searchEdit, &QLineEdit::textChanged, this, &SceneTree::onSearchTextChanged);
}

SceneTree::~SceneTree() = default;

// --- Item creation helpers ---------------------------------------------------

QStandardItem *SceneTree::CreateFolderItem(const QString &name)
{
	auto *item = new QStandardItem(name);
	item->setData(QString(kItemTypeFolder), ItemTypeRole);
	item->setEditable(false);
	item->setDragEnabled(true);
	item->setDropEnabled(true);
	// Default to closed folder icon; updated on expand/collapse
	updateFolderIcon(item);
	return item;
}

QStandardItem *SceneTree::CreateSceneItem(const QString &name, obs_scene_t *scene)
{
	auto *item = new QStandardItem(name);
	item->setData(QString(kItemTypeScene), ItemTypeRole);
	item->setData(QVariant::fromValue<void *>(static_cast<void *>(scene)), SceneObsRefRole);
	item->setData(name, SceneNameRole);
	item->setEditable(true);
	item->setDragEnabled(true);
	item->setDropEnabled(true);
	return item;
}

// --- Path helpers -------------------------------------------------------------

QStandardItem *SceneTree::FindFolderByPath(const QString &path) const
{
	if (path.isEmpty())
		return model->invisibleRootItem();

	auto parts = path.split('/');
	const QStandardItem *current = model->invisibleRootItem();

	for (const auto &part : parts) {
		bool found = false;
		for (int i = 0; i < current->rowCount(); i++) {
			auto *child = current->child(i, 0);
			if (child && IsFolderItem(child) && child->text() == part) {
				current = child;
				found = true;
				break;
			}
		}
		if (!found)
			return nullptr;
	}

	return const_cast<QStandardItem *>(current);
}

QStandardItem *SceneTree::FindOrCreateFolderPath(const QString &path)
{
	if (path.isEmpty())
		return model->invisibleRootItem();

	auto parts = path.split('/');
	QStandardItem *current = model->invisibleRootItem();

	for (const auto &part : parts) {
		QStandardItem *found = nullptr;
		for (int i = 0; i < current->rowCount(); i++) {
			auto *child = current->child(i, 0);
			if (child && IsFolderItem(child) && child->text() == part) {
				found = child;
				break;
			}
		}
		if (!found) {
			found = CreateFolderItem(part);
			current->appendRow(found);
		}
		current = found;
	}

	return current;
}

QString SceneTree::GetSceneFolderPath(const QStandardItem *sceneItem) const
{
	QString path;
	const QStandardItem *parent = sceneItem->parent();
	while (parent && parent != model->invisibleRootItem()) {
		path.prepend(parent->text() + "/");
		parent = parent->parent();
	}
	return path;
}

// --- Type checks ---------------------------------------------------------------

bool SceneTree::IsFolderItem(const QStandardItem *item)
{
	if (!item)
		return false;
	return item->data(ItemTypeRole).toString() == kItemTypeFolder;
}

bool SceneTree::IsSceneItem(const QStandardItem *item)
{
	if (!item)
		return false;
	return item->data(ItemTypeRole).toString() == kItemTypeScene;
}

// --- Enumeration ---------------------------------------------------------------

void SceneTree::EnumerateScenes(const std::function<void(const QString &, obs_scene_t *)> &callback) const
{
	EnumerateScenesImpl(model->invisibleRootItem(), callback);
}

void SceneTree::EnumerateScenesImpl(const QStandardItem *parent,
                                    const std::function<void(const QString &, obs_scene_t *)> &callback) const
{
	for (int i = 0; i < parent->rowCount(); i++) {
		auto *child = parent->child(i, 0);
		if (!child)
			continue;
		if (IsSceneItem(child)) {
			obs_scene_t *scene = static_cast<obs_scene_t *>(child->data(SceneObsRefRole).value<void *>());
			if (scene)
				callback(child->text(), scene);
		} else if (IsFolderItem(child)) {
			EnumerateScenesImpl(child, callback);
		}
	}
}

// --- Scene management ----------------------------------------------------------

void SceneTree::AddScene(const QString &name, obs_scene_t *scene, const QString &folderPath)
{
	if (!scene)
		return;

	if (FindSceneItem(name))
		return;

	QStandardItem *parent = FindOrCreateFolderPath(folderPath);
	auto *item = CreateSceneItem(name, scene);
	parent->appendRow(item);

	QModelIndex sourceIndex = model->indexFromItem(item);
	QModelIndex proxyIndex = proxyModel->mapFromSource(sourceIndex);
	treeView->scrollTo(proxyIndex, QAbstractItemView::EnsureVisible);
}

void SceneTree::RemoveScene(obs_scene_t *scene)
{
	if (!scene)
		return;

	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	if (!name)
		return;

	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (!item)
		return;

	if (currentScene == scene)
		currentScene = nullptr;

	auto *parent = item->parent();
	if (parent) {
		parent->removeRow(item->row());
	} else {
		// Top-level item: parent() is null for children of the invisible root.
		model->removeRow(item->row(), QModelIndex());
	}
}

void SceneTree::SetCurrentScene(obs_scene_t *scene)
{
	currentScene = scene;

	if (!scene) {
		treeView->clearSelection();
		return;
	}

	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (item) {
		QModelIndex sourceIndex = model->indexFromItem(item);
		QModelIndex proxyIndex = proxyModel->mapFromSource(sourceIndex);
		treeView->setCurrentIndex(proxyIndex);
		treeView->scrollTo(proxyIndex, QAbstractItemView::EnsureVisible);
	}

	// Track last-used for sorting
	MarkSceneActive(scene);
}

obs_scene_t *SceneTree::GetCurrentScene() const
{
	return currentScene;
}

int SceneTree::SceneCount() const
{
	int count = 0;
	EnumerateScenes([&count](const QString &, obs_scene_t *) { count++; });
	return count;
}

QStandardItem *SceneTree::FindSceneItem(const QString &name) const
{
	QStandardItem *result = nullptr;
	std::function<void(const QStandardItem *)> search = [&](const QStandardItem *parent) {
		for (int i = 0; i < parent->rowCount() && !result; i++) {
			auto *child = parent->child(i, 0);
			if (!child)
				continue;
			if (IsSceneItem(child) && child->text() == name) {
				result = child;
				return;
			}
			if (IsFolderItem(child))
				search(child);
		}
	};
	search(model->invisibleRootItem());
	return result;
}

// --- Reordering ----------------------------------------------------------------

void SceneTree::MoveSceneUp(obs_scene_t *scene)
{
	if (!scene)
		return;
	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (!item || item->row() == 0)
		return;

	auto *parent = item->parent();
	int row = item->row();
	auto taken = parent->takeRow(row);
	parent->insertRow(row - 1, taken);
	emit scenesReordered();
}

void SceneTree::MoveSceneDown(obs_scene_t *scene)
{
	if (!scene)
		return;
	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (!item)
		return;

	auto *parent = item->parent();
	int row = item->row();
	if (row >= parent->rowCount() - 1)
		return;

	auto taken = parent->takeRow(row);
	parent->insertRow(row + 1, taken);
	emit scenesReordered();
}

void SceneTree::MoveSceneToTop(obs_scene_t *scene)
{
	if (!scene)
		return;
	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (!item || item->row() == 0)
		return;

	auto *parent = item->parent();
	auto taken = parent->takeRow(item->row());
	parent->insertRow(0, taken);
	emit scenesReordered();
}

void SceneTree::MoveSceneToBottom(obs_scene_t *scene)
{
	if (!scene)
		return;
	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (!item)
		return;

	auto *parent = item->parent();
	int row = item->row();
	int lastRow = parent->rowCount() - 1;
	if (row == lastRow)
		return;

	auto taken = parent->takeRow(row);
	parent->appendRow(taken);
	emit scenesReordered();
}

// --- Folder management ---------------------------------------------------------

bool SceneTree::AddFolder(const QString &name, const QString &parentPath)
{
	if (name.isEmpty())
		return false;

	QStandardItem *parent = FindOrCreateFolderPath(parentPath);
	for (int i = 0; i < parent->rowCount(); i++) {
		auto *child = parent->child(i, 0);
		if (child && child->text() == name)
			return false;
	}

	auto *folder = CreateFolderItem(name);
	parent->appendRow(folder);
	QModelIndex sourceIndex = model->indexFromItem(folder);
	isSyncingExpand = true;
	treeView->expand(proxyModel->mapFromSource(sourceIndex));
	isSyncingExpand = false;
	emit layoutChanged();
	return true;
}

void SceneTree::RemoveFolder(const QString &path)
{
	blog(LOG_INFO, "[RemoveFolder] enter path=%s", path.toUtf8().constData());

	if (path.isEmpty())
		return;

	auto *folder = FindFolderByPath(path);
	blog(LOG_INFO, "[RemoveFolder] found folder=%p text=%s",
	      (void *)folder, folder ? folder->text().toUtf8().constData() : "null");
	if (!folder || folder == model->invisibleRootItem())
		return;

	auto *parent = folder->parent();
	blog(LOG_INFO, "[RemoveFolder] folder->parent()=%p invisibleRoot=%p",
	      (void *)parent, (void *)model->invisibleRootItem());
	if (!parent)
		parent = model->invisibleRootItem();

	int folderRow = -1;
	for (int i = 0; i < parent->rowCount(); i++) {
		if (parent->child(i, 0) == folder) {
			folderRow = i;
			break;
		}
	}
	blog(LOG_INFO, "[RemoveFolder] folderRow=%d parentRowCount=%d", folderRow, parent->rowCount());
	if (folderRow == -1)
		return;

	// Collect child info before removal (we'll recreate fresh items after)
	struct ChildInfo {
		QString name;
		bool isFolder;
	};
	QList<ChildInfo> children;
	for (int i = 0; i < folder->rowCount(); i++) {
		auto *child = folder->child(i, 0);
		if (!child)
			continue;
		children.append({child->text(), IsFolderItem(child)});
	}
	blog(LOG_INFO, "[RemoveFolder] collected %d children", (int)children.size());

	// Remove the folder row from the model
	parent->removeRow(folderRow);
	blog(LOG_INFO, "[RemoveFolder] removeRow done");

	// Recreate fresh items and add them to parent
	for (const auto &info : children) {
		if (info.isFolder) {
			parent->appendRow(CreateFolderItem(info.name));
		} else {
			obs_source_t *source = obs_get_source_by_name(info.name.toUtf8().constData());
			if (source) {
				auto *item = CreateSceneItem(info.name, reinterpret_cast<obs_scene_t *>(source));
				parent->appendRow(item);
				obs_source_release(source);
			}
		}
	}
	blog(LOG_INFO, "[RemoveFolder] re-created %d items", (int)children.size());

	emit layoutChanged();
}

void SceneTree::RemoveFolderAndScenes(const QString &path)
{
	blog(LOG_INFO, "[RemoveFolderAndScenes] enter path=%s", path.toUtf8().constData());

	if (path.isEmpty())
		return;

	auto *folder = FindFolderByPath(path);
	blog(LOG_INFO, "[RemoveFolderAndScenes] found folder=%p text=%s",
	      (void *)folder, folder ? folder->text().toUtf8().constData() : "null");
	if (!folder || folder == model->invisibleRootItem())
		return;

	// Collect scene sources to remove
	QVector<obs_source_t *> sourcesToRemove;
	std::function<void(QStandardItem *)> collect = [&](QStandardItem *node) {
		for (int i = 0; i < node->rowCount(); i++) {
			auto *child = node->child(i, 0);
			if (!child)
				continue;
			if (IsSceneItem(child)) {
				const char *name = child->text().toUtf8().constData();
				obs_source_t *source = obs_get_source_by_name(name);
				blog(LOG_INFO, "[RemoveFolderAndScenes] scene=%s source=%p", name, (void *)source);
				if (source)
					sourcesToRemove.append(source);
			} else if (IsFolderItem(child)) {
				collect(child);
			}
		}
	};
	collect(folder);
	blog(LOG_INFO, "[RemoveFolderAndScenes] collected %d sources", (int)sourcesToRemove.size());

	auto *parent = folder->parent();
	if (!parent)
		parent = model->invisibleRootItem();

	int folderRow = -1;
	for (int i = 0; i < parent->rowCount(); i++) {
		if (parent->child(i, 0) == folder) {
			folderRow = i;
			break;
		}
	}
	blog(LOG_INFO, "[RemoveFolderAndScenes] folderRow=%d", folderRow);
	if (folderRow == -1)
		return;

	blog(LOG_INFO, "[RemoveFolderAndScenes] calling parent->removeRow(%d)", folderRow);
	parent->removeRow(folderRow);
	blog(LOG_INFO, "[RemoveFolderAndScenes] removeRow done");

	emit layoutChanged();

	// Queue source removals
	QVector<obs_source_t *> *sourcesCopy = new QVector<obs_source_t *>(sourcesToRemove);
	QMetaObject::invokeMethod(this, [sourcesCopy]() {
		for (auto *source : *sourcesCopy) {
			blog(LOG_INFO, "[RemoveFolderAndScenes] obs_source_remove %s",
			      obs_source_get_name(source));
			obs_source_remove(source);
			obs_source_release(source);
		}
		delete sourcesCopy;
	}, Qt::QueuedConnection);
}

QString SceneTree::GetNextFolderName() const
{
	auto *root = model->invisibleRootItem();

	// Collect all top-level folder names
	QSet<QString> existing;
	for (int i = 0; i < root->rowCount(); i++) {
		auto *child = root->child(i, 0);
		if (child && IsFolderItem(child))
			existing.insert(child->text());
	}

	if (!existing.contains("Folder"))
		return "Folder";

	int i = 2;
	while (existing.contains(QString("Folder %1").arg(i)))
		i++;
	return QString("Folder %1").arg(i);
}

bool SceneTree::RenameFolder(const QString &path, const QString &newName)
{
	if (newName.isEmpty())
		return false;

	auto *folder = FindFolderByPath(path);
	if (!folder || folder == model->invisibleRootItem())
		return false;

	folder->setText(newName);
	emit layoutChanged();
	return true;
}

void SceneTree::MoveSceneToFolder(obs_scene_t *scene, const QString &folderPath)
{
	if (!scene)
		return;

	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (!item)
		return;

	auto *oldParent = item->parent();
	if (!oldParent)
		oldParent = model->invisibleRootItem();

	QList<QStandardItem *> taken = oldParent->takeRow(item->row());
	if (taken.isEmpty())
		return;

	QStandardItem *newParent = FindOrCreateFolderPath(folderPath);
	newParent->appendRow(taken);
	emit layoutChanged();
}

// --- Layout persistence --------------------------------------------------------

QString SceneTree::SaveLayout() const
{
	QJsonArray array;

	std::function<void(const QStandardItem *, QJsonArray &)> save =
		[&](const QStandardItem *parent, QJsonArray &arr) {
			for (int i = 0; i < parent->rowCount(); i++) {
				auto *child = parent->child(i, 0);
				if (!child)
					continue;
				if (IsSceneItem(child)) {
					QJsonObject obj;
					obj["type"] = kItemTypeScene;
					obj["name"] = child->text();
					QColor color = child->data(SceneColorRole).value<QColor>();
					if (color.isValid())
						obj["color"] = color.name(QColor::HexArgb);
					if (child->data(ScenePinnedRole).toBool())
						obj["pinned"] = true;
					arr.append(obj);
				} else if (IsFolderItem(child)) {
					QJsonObject obj;
					obj["type"] = kItemTypeFolder;
					obj["name"] = child->text();
					int sortMode = child->data(FolderSortModeRole).toInt();
					if (sortMode != 0)
						obj["sortMode"] = sortMode;
					int displayMode = child->data(FolderDisplayModeRole).toInt();
					if (displayMode != 0)
						obj["displayMode"] = displayMode;
					int compactLimit = child->data(FolderCompactLimitRole).toInt();
					if (compactLimit > 0)
						obj["compactLimit"] = compactLimit;
					QJsonArray children;
					save(child, children);
					obj["children"] = children;
					arr.append(obj);
				}
			}
		};

	save(model->invisibleRootItem(), array);
	return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

void SceneTree::LoadLayout(const QString &json)
{
	if (json.isEmpty())
		return;

	QJsonParseError parseError;
	QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isArray())
		return;

	auto *root = model->invisibleRootItem();

	// Step 1: Collect scene data (name + obs_scene_t*) and signal handler data
	// from items already in the tree. These were added by SourceCreated during
	// obs_load_sources. We must preserve the signal handlers (item_add, reorder,
	// refresh) because they are stored in the QStandardItem's data and would be
	// lost when we clear and rebuild the tree below.
	QMap<QString, obs_scene_t *> sceneData;
	QMap<QString, QVariant> signalData;
	std::function<void(const QStandardItem *)> collect = [&](const QStandardItem *parent) {
		for (int i = 0; i < parent->rowCount(); i++) {
			auto *child = parent->child(i, 0);
			if (!child)
				continue;
			if (IsSceneItem(child)) {
				obs_scene_t *scene = static_cast<obs_scene_t *>(
					child->data(SceneObsRefRole).value<void *>());
				sceneData[child->text()] = scene;
				QVariant sigVar = child->data(static_cast<int>(Qt::UserRole + 1)); // QtDataRole::OBSSignals
				if (sigVar.isValid())
					signalData[child->text()] = sigVar;
			} else if (IsFolderItem(child)) {
				collect(child);
			}
		}
	};
	collect(root);

	// Step 2: Clear the tree.
	root->removeRows(0, root->rowCount());

	// Step 3: Rebuild from JSON using collected scene data (no re-parenting).
	std::function<void(const QJsonArray &, QStandardItem *)> load =
		[&](const QJsonArray &arr, QStandardItem *parent) {
			for (const auto val : arr) {
				QJsonObject obj = val.toObject();
				QString type = obj["type"].toString();
				QString name = obj["name"].toString();

				if (type == kItemTypeScene) {
					auto it = sceneData.find(name);
					if (it != sceneData.end() && it.value()) {
						auto *item = CreateSceneItem(name, it.value());
						if (obj["pinned"].toBool()) {
							item->setData(true, ScenePinnedRole);
							item->setIcon(MakeDotIcon(QColor(255, 193, 7)));
						}
						parent->appendRow(item);
						sceneData.erase(it);
					}
				} else if (type == kItemTypeFolder) {
					auto *folder = CreateFolderItem(name);
					int sortMode = obj["sortMode"].toInt(0);
					if (sortMode != 0)
						folder->setData(sortMode, FolderSortModeRole);
					int displayMode = obj["displayMode"].toInt(0);
					if (displayMode != 0)
						folder->setData(displayMode, FolderDisplayModeRole);
					int compactLimit = obj["compactLimit"].toInt(0);
					if (compactLimit > 0)
						folder->setData(compactLimit, FolderCompactLimitRole);
					parent->appendRow(folder);
					QJsonArray children = obj["children"].toArray();
					if (!children.isEmpty())
						load(children, folder);
				}
			}
		};

	load(doc.array(), root);

	// Step 4: Any scenes not placed by the JSON go to root level.
	for (auto it = sceneData.cbegin(); it != sceneData.cend(); ++it) {
		if (it.value()) {
			auto *item = CreateSceneItem(it.key(), it.value());
			root->appendRow(item);
		}
	}

	// Step 5: Re-apply preserved signal handler data to the new scene items.
	// Without this, the "item_add"/"reorder"/"refresh" handlers would be lost
	// and the sources dock would not update when sources are added/removed.
	if (!signalData.isEmpty()) {
		std::function<void(QStandardItem *)> restore = [&](QStandardItem *parent) {
			for (int i = 0; i < parent->rowCount(); i++) {
				auto *child = parent->child(i, 0);
				if (!child)
					continue;
				if (IsSceneItem(child)) {
					auto sigIt = signalData.find(child->text());
					if (sigIt != signalData.end())
						child->setData(sigIt.value(), static_cast<int>(Qt::UserRole + 1));
				} else if (IsFolderItem(child)) {
					restore(child);
				}
			}
		};
		restore(root);
	}

	isSyncingExpand = true;
	treeView->expandAll();
	isSyncingExpand = false;
	emit layoutChanged();
}

// --- UI helpers -----------------------------------------------------------------

void SceneTree::Clear()
{
	model->clear();
	currentScene = nullptr;
}

void SceneTree::ExpandAll()
{
	isSyncingExpand = true;
	treeView->expandAll();
	isSyncingExpand = false;
}

void SceneTree::CollapseAll()
{
	isSyncingExpand = true;
	treeView->collapseAll();
	isSyncingExpand = false;
}

QString SceneTree::SceneNameAt(const QPoint &pos) const
{
	QModelIndex proxyIndex = treeView->indexAt(pos);
	if (!proxyIndex.isValid())
		return {};

	QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
	auto *item = model->itemFromIndex(sourceIndex);
	if (item && IsSceneItem(item))
		return item->text();
	return {};
}

QString SceneTree::FolderPathAt(const QPoint &pos) const
{
	QModelIndex proxyIndex = treeView->indexAt(pos);

	if (!proxyIndex.isValid()) {
		proxyIndex = treeView->currentIndex();
		if (!proxyIndex.isValid())
			return {};
	}

	QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
	auto *item = model->itemFromIndex(sourceIndex);

	if (item && IsFolderItem(item)) {
		QString path = item->text();
		const QStandardItem *parent = item->parent();
		while (parent && parent != model->invisibleRootItem()) {
			path.prepend(parent->text() + "/");
			parent = parent->parent();
		}
		qWarning() << "[SceneTree] FolderPathAt returning path:" << path.toUtf8().constData();
		return path;
	}
		return {};
}

void SceneTree::SetFilterText(const QString &text)
{
	proxyModel->setFilterFixedString(text);
}

// --- Signal handlers -------------------------------------------------------------

void SceneTree::onItemSelectionChanged()
{
	// Ignore selection changes caused by search filtering or sorting — the
	// active scene must only change when the user explicitly selects a scene.
	if (isSorting || isFiltering)
		return;

	QModelIndex proxyIndex = treeView->currentIndex();
	if (!proxyIndex.isValid()) {
		currentScene = nullptr;
		emit sceneSelectionChanged();
		return;
	}

	QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
	auto *item = model->itemFromIndex(sourceIndex);
	if (item && IsSceneItem(item)) {
		currentScene = static_cast<obs_scene_t *>(item->data(SceneObsRefRole).value<void *>());
		emit sceneSelectionChanged();
	}
}

QString SceneTree::GetSelectedFolderPath() const
{
	QModelIndex proxyIndex = treeView->currentIndex();
	if (!proxyIndex.isValid())
		return {};

	QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
	const auto *item = model->itemFromIndex(sourceIndex);
	if (!item || !IsFolderItem(item))
		return {};

	// Build the path from root to this folder
	QStringList parts;
	const QStandardItem *cur = item;
	while (cur && cur != model->invisibleRootItem()) {
		parts.prepend(cur->text());
		cur = cur->parent();
	}
	return parts.join('/');
}

// --- Colour management ---------------------------------------------------------

void SceneTree::SetItemColor(QStandardItem *item, const QColor &color)
{
	if (!item)
		return;
	item->setData(color.isValid() ? QVariant::fromValue<QColor>(color) : QVariant(), SceneColorRole);
	treeView->viewport()->update();
}

QColor SceneTree::GetItemColor(const QStandardItem *item) const
{
	if (!item)
		return {};
	return item->data(SceneColorRole).value<QColor>();
}

void SceneTree::ClearItemColor(QStandardItem *item)
{
	if (!item)
		return;
	item->setData(QVariant(), SceneColorRole);
	treeView->viewport()->update();
}

// --- Per-folder sorting ----------------------------------------------------------

SceneSortMode SceneTree::GetFolderSortMode(const QStandardItem *folder) const
{
	if (!folder || !IsFolderItem(folder))
		return SceneSortMode::SortNone;
	return static_cast<SceneSortMode>(folder->data(FolderSortModeRole).toInt());
}

void SceneTree::SetFolderSortMode(QStandardItem *folder, SceneSortMode mode)
{
	if (!folder || !IsFolderItem(folder))
		return;
	folder->setData(static_cast<int>(mode), FolderSortModeRole);
	ApplyFolderSort(folder);
	emit layoutChanged();
}

void SceneTree::ApplyFolderSort(QStandardItem *folder)
{
	if (!folder || !IsFolderItem(folder))
		return;

	SceneSortMode mode = GetFolderSortMode(folder);
	if (mode == SceneSortMode::SortNone)
		return; // Manual ordering — do nothing

	int rowCount = folder->rowCount();
	if (rowCount <= 1)
		return;

	isSorting = true;

	// Take all rows out using takeRow (does NOT delete items)
	QVector<QStandardItem *> items;
	for (int i = 0; i < rowCount; i++) {
		auto taken = folder->takeRow(0);
		if (!taken.isEmpty())
			items.append(taken.first());
	}

	// Sort the detached items (stable sort preserves order of equal elements)
	// Pinned items always come first, then non-pinned sorted by mode
	std::stable_sort(items.begin(), items.end(), [mode](QStandardItem *a, QStandardItem *b) {
		bool aPinned = a->data(ScenePinnedRole).toBool();
		bool bPinned = b->data(ScenePinnedRole).toBool();

		// Pinned items always before non-pinned
		if (aPinned && !bPinned)
			return true;
		if (!aPinned && bPinned)
			return false;

		// Both pinned or both non-pinned: sort by mode
		if (mode == SceneSortMode::SortAZ)
			return a->text().toLower() < b->text().toLower();
		if (mode == SceneSortMode::SortZA)
			return a->text().toLower() > b->text().toLower();
		if (mode == SceneSortMode::SortByLastUsed)
			return a->data(SceneLastUsedRole).toInt() > b->data(SceneLastUsedRole).toInt();
		return false;
	});

	// Re-insert in sorted order
	for (auto *item : items)
		folder->appendRow(item);

	isSorting = false;
}

void SceneTree::ApplyAllSorts()
{
	std::function<void(QStandardItem *)> walk = [&](QStandardItem *parent) {
		for (int i = 0; i < parent->rowCount(); i++) {
			auto *child = parent->child(i, 0);
			if (!child)
				continue;
			if (IsFolderItem(child)) {
				if (GetFolderSortMode(child) != SceneSortMode::SortNone)
					ApplyFolderSort(child);
				walk(child);
			}
		}
	};
	walk(model->invisibleRootItem());
}

void SceneTree::MarkSceneActive(obs_scene_t *scene)
{
	if (!scene)
		return;
	const char *name = obs_source_get_name(obs_scene_get_source(scene));
	auto *item = FindSceneItem(QString::fromUtf8(name));
	if (item) {
		lastUsedCounter++;
		item->setData(lastUsedCounter, SceneLastUsedRole);

		// If the parent folder uses SortByLastUsed, defer re-sort to avoid
		// reentrancy (model changes during currentChanged would trigger
		// another currentChanged).
		QStandardItem *parent = item->parent();
		if (parent && IsFolderItem(parent)) {
			if (GetFolderSortMode(parent) == SceneSortMode::SortByLastUsed) {
				QTimer::singleShot(0, this, [this, parent]() {
					// Verify the folder is still in the model (has a parent or is root child)
					if (parent->parent() || parent->row() != -1)
						ApplyFolderSort(parent);
				});
			}
		}
	}
}

void SceneTree::SetFolderSortModeByPath(const QString &path, SceneSortMode mode)
{
	auto *folder = FindFolderByPath(path);
	if (folder)
		SetFolderSortMode(folder, mode);
}

SceneSortMode SceneTree::GetFolderSortModeByPath(const QString &path) const
{
	const auto *folder = FindFolderByPath(path);
	return GetFolderSortMode(folder);
}

// --- Folder display mode ---------------------------------------------------------

FolderDisplayMode SceneTree::GetFolderDisplayMode(const QStandardItem *folder) const
{
	if (!folder || !IsFolderItem(folder))
		return FolderDisplayMode::Expanded;
	return static_cast<FolderDisplayMode>(folder->data(FolderDisplayModeRole).toInt());
}

void SceneTree::SetFolderDisplayMode(QStandardItem *folder, FolderDisplayMode mode)
{
	if (!folder || !IsFolderItem(folder))
		return;
	folder->setData(static_cast<int>(mode), FolderDisplayModeRole);

	// Apply the visual state to the tree view
	QModelIndex sourceIndex = model->indexFromItem(folder);
	QModelIndex proxyIndex = proxyModel->mapFromSource(sourceIndex);
	if (proxyIndex.isValid()) {
		isSyncingExpand = true;
		switch (mode) {
		case FolderDisplayMode::Expanded:
			treeView->expand(proxyIndex);
			break;
		case FolderDisplayMode::Compact:
			treeView->expand(proxyIndex); // Must be expanded for children to be visible
			break;
		case FolderDisplayMode::Collapsed:
			treeView->collapse(proxyIndex);
			break;
		}
		isSyncingExpand = false;
	}

	updateFolderIcon(folder);
	proxyModel->invalidate();
	emit layoutChanged();
}

int SceneTree::GetCompactLimit(const QStandardItem *folder) const
{
	if (!folder || !IsFolderItem(folder))
		return 3; // Default: show 3 non-pinned scenes
	int limit = folder->data(FolderCompactLimitRole).toInt();
	return (limit > 0) ? limit : 3;
}

void SceneTree::SetCompactLimit(QStandardItem *folder, int limit)
{
	if (!folder || !IsFolderItem(folder))
		return;
	folder->setData(std::max(1, limit), FolderCompactLimitRole);
	proxyModel->invalidate();
	emit layoutChanged();
}

FolderDisplayMode SceneTree::GetFolderDisplayModeByPath(const QString &path) const
{
	const auto *folder = FindFolderByPath(path);
	return GetFolderDisplayMode(folder);
}

void SceneTree::SetFolderDisplayModeByPath(const QString &path, FolderDisplayMode mode)
{
	auto *folder = FindFolderByPath(path);
	if (folder)
		SetFolderDisplayMode(folder, mode);
}

void SceneTree::SetCompactLimitByPath(const QString &path, int limit)
{
	auto *folder = FindFolderByPath(path);
	if (folder)
		SetCompactLimit(folder, limit);
}

// --- Scene pinning -----------------------------------------------------------------

bool SceneTree::IsScenePinned(const QStandardItem *item) const
{
	if (!item || !IsSceneItem(item))
		return false;
	return item->data(ScenePinnedRole).toBool();
}

void SceneTree::ToggleScenePin(QStandardItem *item)
{
	if (!item || !IsSceneItem(item))
		return;
	bool newPinState = !item->data(ScenePinnedRole).toBool();
	item->setData(newPinState, ScenePinnedRole);

	// Set or clear the pin indicator icon
	if (newPinState)
		item->setIcon(MakeDotIcon(QColor(255, 193, 7))); // Golden dot
	else
		item->setIcon(QIcon()); // Clear icon

	// Re-sort the parent folder so pinned items move to top
	QStandardItem *parent = item->parent();
	if (parent) {
		isSorting = true;
		ApplyFolderSort(parent);
		isSorting = false;
	}
	emit layoutChanged();
}

void SceneTree::ToggleScenePinByName(const QString &name)
{
	auto *item = FindSceneItem(name);
	if (item)
		ToggleScenePin(item);
}

// --- Search --------------------------------------------------------------------

void SceneTree::onSearchTextChanged(const QString &text)
{
	// Guard so the incidental current-index changes that happen while rows are
	// hidden/shown by the filter do not switch the active scene.
	isFiltering = true;
	proxyModel->setFilterFixedString(text);
	isSyncingExpand = true;
	treeView->expandAll();
	isSyncingExpand = false;
	isFiltering = false;
}

// --- Folder icons ----------------------------------------------------------------

void SceneTree::updateFolderIcon(QStandardItem *folderItem)
{
	if (!folderItem || !IsFolderItem(folderItem))
		return;

	QStyle *style = treeView->style();
	if (!style)
		style = QApplication::style();

	bool expanded = false;
	QModelIndex idx = model->indexFromItem(folderItem);
	if (idx.isValid())
		expanded = treeView->isExpanded(proxyModel->mapFromSource(idx));

	QIcon baseIcon = style->standardIcon(expanded ? QStyle::SP_DirOpenIcon : QStyle::SP_DirClosedIcon);

	// If folder is in Compact mode, add a small indicator dot
	int displayMode = folderItem->data(FolderDisplayModeRole).toInt();
	if (displayMode == static_cast<int>(FolderDisplayMode::Compact))
		folderItem->setIcon(MakeCompactFolderIcon(baseIcon, QColor(100, 180, 255)));
	else
		folderItem->setIcon(baseIcon);
}
