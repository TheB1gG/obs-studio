#pragma once

#include <QWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <QListWidget>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QStyledItemDelegate>
#include <QColor>

// Per-folder sort mode
enum class SceneSortMode : int {
	SortNone = 0,    // Manual ordering (no auto-sort)
	SortAZ = 1,      // Alphabetical A→Z
	SortZA = 2,      // Alphabetical Z→A
	SortByLastUsed = 3 // Most recently active scene first
};

// Per-folder display mode
enum class FolderDisplayMode : int {
	Expanded = 0,  // Arrow down, shows all children
	Compact = 1,   // Dot indicator, shows pinned + limited children
	Collapsed = 2  // Arrow right, hides all children
};

#include <obs.h>

// A tree-based scene organiser that supports nested folders and drag-and-drop
// reordering. Replaces the old flat QListWidget-based SceneTree.
//
// Architecture (following StreamUP's proven approach):
//   - Custom QStandardItemModel handles dropMimeData() for cross-parent moves
//   - Custom QTreeView lets Qt's normal InternalMove flow work
//   - QSortFilterProxyModel enables search/filtering
//   - Custom MIME format stores raw QStandardItem* pointers
//
// The model hierarchy is:
//   root (hidden)
//     ├── "Folder A" (folder item)
//     │     ├── "Scene 1" (scene item)
//     │     └── "Sub Folder" (folder item)
//     │           └── "Scene 2" (scene item)
//     └── "Scene 3" (scene item, at root level)
//
// Scenes are identified by their OBS source name. Folders are purely
// organisational — they do not map to any OBS concept.

// Custom model that handles drag-and-drop moves via dropMimeData().
// Uses takeRow() + insertRow() for safe cross-parent moves.
class SceneTreeModel : public QStandardItemModel {
	Q_OBJECT

public:
	explicit SceneTreeModel(QObject *parent = nullptr);

	Qt::DropActions supportedDropActions() const override;
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	QStringList mimeTypes() const override;
	QMimeData *mimeData(const QModelIndexList &indexes) const override;
	bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
	                  const QModelIndex &parent) override;

signals:
	void modelChanged();
};

// Custom view that lets Qt's normal InternalMove flow work.
// The only intervention: after the base dropEvent (which triggers the model's
// dropMimeData), set the action to Copy so the view doesn't try to delete
// originals that are already moved.
class SceneTreeView : public QTreeView {
	Q_OBJECT

public:
	explicit SceneTreeView(QWidget *parent = nullptr);

signals:
	void branchClicked(const QModelIndex &index);

protected:
	void startDrag(Qt::DropActions supportedActions) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dragMoveEvent(QDragMoveEvent *event) override;
	void dropEvent(QDropEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
};

// Item delegate that paints a coloured rounded-rectangle background behind
// rows whose item carries a QColor in the SceneColorRole data slot.
// Rows without a colour are painted by the default style (theme).
class CustomColorDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	CustomColorDelegate(SceneTreeModel *model, QSortFilterProxyModel *proxy, QObject *parent = nullptr);
	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
	SceneTreeModel *model;
	QSortFilterProxyModel *proxy;
};

// Custom proxy model that adds Compact-mode filtering on top of text search.
// In Compact mode, only pinned children + first N non-pinned children are visible.
class SceneTreeProxyModel : public QSortFilterProxyModel {
	Q_OBJECT

public:
	explicit SceneTreeProxyModel(QObject *parent = nullptr);

	QModelIndex mapToSource(const QModelIndex &proxyIndex) const override;

protected:
	bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
	SceneTreeModel *sourceModel;
};

class SceneTree : public QWidget {
	Q_OBJECT

public:
	explicit SceneTree(QWidget *parent = nullptr);
	~SceneTree() override;

	// --- Scene management ---------------------------------------------------

	void AddScene(const QString &name, obs_scene_t *scene, const QString &folderPath = {});
	void RemoveScene(obs_scene_t *scene);
	void SetCurrentScene(obs_scene_t *scene);
	obs_scene_t *GetCurrentScene() const;
	int SceneCount() const;
	void EnumerateScenes(const std::function<void(const QString &, obs_scene_t *)> &callback) const;
	QStandardItem *FindSceneItem(const QString &name) const;
	QString GetSceneFolderPath(const QStandardItem *sceneItem) const;

	// --- Scene / folder colour ----------------------------------------------

	void SetItemColor(QStandardItem *item, const QColor &color);
	QColor GetItemColor(const QStandardItem *item) const;
	void ClearItemColor(QStandardItem *item);

	// --- Reordering ---------------------------------------------------------

	void MoveSceneUp(obs_scene_t *scene);
	void MoveSceneDown(obs_scene_t *scene);
	void MoveSceneToTop(obs_scene_t *scene);
	void MoveSceneToBottom(obs_scene_t *scene);

	// --- Folder management --------------------------------------------------

	bool AddFolder(const QString &name, const QString &parentPath = {});
	void RemoveFolder(const QString &path);
	void RemoveFolderAndScenes(const QString &path);
	bool RenameFolder(const QString &path, const QString &newName);
	void MoveSceneToFolder(obs_scene_t *scene, const QString &folderPath);
	QString GetNextFolderName() const;

	// --- Per-folder sorting -------------------------------------------------

	SceneSortMode GetFolderSortMode(const QStandardItem *folder) const;
	void SetFolderSortMode(QStandardItem *folder, SceneSortMode mode);
	void ApplyFolderSort(QStandardItem *folder);
	void ApplyAllSorts();
	void MarkSceneActive(obs_scene_t *scene);
	void SetFolderSortModeByPath(const QString &path, SceneSortMode mode);
	SceneSortMode GetFolderSortModeByPath(const QString &path) const;

	// --- Folder display mode ------------------------------------------------

	FolderDisplayMode GetFolderDisplayMode(const QStandardItem *folder) const;
	void SetFolderDisplayMode(QStandardItem *folder, FolderDisplayMode mode);
	int GetCompactLimit(const QStandardItem *folder) const;
	void SetCompactLimit(QStandardItem *folder, int limit);
	FolderDisplayMode GetFolderDisplayModeByPath(const QString &path) const;
	void SetFolderDisplayModeByPath(const QString &path, FolderDisplayMode mode);
	void SetCompactLimitByPath(const QString &path, int limit);

	// --- Scene pinning ------------------------------------------------------

	bool IsScenePinned(const QStandardItem *item) const;
	void ToggleScenePin(QStandardItem *item);
	void ToggleScenePinByName(const QString &name);

	// --- Layout persistence (per-profile via scene collection) --------------

	QString SaveLayout() const;
	void LoadLayout(const QString &json);

	// --- UI helpers ---------------------------------------------------------

	void Clear();
	void ExpandAll();
	void CollapseAll();

	QTreeView *GetTreeView() const { return treeView; }
	QString SceneNameAt(const QPoint &pos) const;
	QString FolderPathAt(const QPoint &pos) const;
	QString GetSelectedFolderPath() const;

	// Search: filter the tree by text. Empty string shows all.
	void SetFilterText(const QString &text);

signals:
	void customContextMenuRequested(const QPoint &pos);
	void sceneSelectionChanged();
	void scenesReordered();
	void layoutChanged();
	void addSceneRequested();
	void addFolderRequested();

private:
	SceneTreeView *treeView;
	SceneTreeModel *model;
	SceneTreeProxyModel *proxyModel;
	obs_scene_t *currentScene = nullptr;
	int lastUsedCounter = 0;
	bool isSorting = false;
	bool isSyncingExpand = false;

	// Internal helpers
	QStandardItem *CreateFolderItem(const QString &name);
	QStandardItem *CreateSceneItem(const QString &name, obs_scene_t *scene);
	QStandardItem *FindOrCreateFolderPath(const QString &path);
	QStandardItem *FindFolderByPath(const QString &path) const;
	static bool IsFolderItem(const QStandardItem *item);
	static bool IsSceneItem(const QStandardItem *item);

	void EnumerateScenesImpl(const QStandardItem *parent,
	                         const std::function<void(const QString &, obs_scene_t *)> &callback) const;

	void onItemSelectionChanged();
	void onSearchTextChanged(const QString &text);
	void updateFolderIcon(QStandardItem *folderItem);
};
