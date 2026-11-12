#include "ProjectDialog.h"
#include "Diagnostics.h"
#include "UiAppearance.h"
#include "WorkspacePaths.h"

#include <QAbstractListModel>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardPaths>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
class AvailableFiles final : public QFileSystemModel {
public:
    explicit AvailableFiles(QObject *parent) : QFileSystemModel(parent) {
        setReadOnly(true);
        setResolveSymlinks(false);
        setFilter(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    }
    bool hasChildren(const QModelIndex &index = {}) const override {
        return !fileInfo(index).isSymLink() && QFileSystemModel::hasChildren(index);
    }
    bool canFetchMore(const QModelIndex &index) const override {
        return !fileInfo(index).isSymLink() && QFileSystemModel::canFetchMore(index);
    }
    void fetchMore(const QModelIndex &index) override {
        if (!fileInfo(index).isSymLink())
            QFileSystemModel::fetchMore(index);
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        auto flags = QFileSystemModel::flags(index);
        if (fileInfo(index).isSymLink())
            flags &= ~Qt::ItemIsEnabled;
        return flags;
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (role == Qt::DecorationRole && index.column() == 0)
            return UiAppearance::icon(isDir(index) ? "Folder" : "file");
        return QFileSystemModel::data(index, role);
    }
};
}

class ProjectFileListModel final : public QAbstractListModel {
public:
    explicit ProjectFileListModel(QObject *parent) : QAbstractListModel(parent) {}
    QString root;
    QStringList files;
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(files.size());
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= files.size())
            return {};
        const auto &path = files[index.row()];
        if (role == Qt::DisplayRole)
            return QDir::toNativeSeparators(path);
        if (role == Qt::UserRole)
            return path;
        if (role == Qt::ToolTipRole)
            return QDir(root).filePath(path);
        if (role == Qt::DecorationRole)
            return UiAppearance::icon("file");
        return {};
    }
    void replace(QStringList paths = {}) {
        beginResetModel();
        files = std::move(paths);
        endResetModel();
    }
    void append(const QStringList &paths) {
        if (paths.isEmpty())
            return;
        const int start = rowCount();
        beginInsertRows({}, start, start + static_cast<int>(paths.size()) - 1);
        files.append(paths);
        endInsertRows();
    }
};

ProjectDialog::ProjectDialog(const QString &root, const WorkspaceOptions &options, QWidget *parent,
                             const AppSettings &settings)
    : QDialog(parent), options_(options), scanJob_(this) {
    const bool editing = options.managed;
    setWindowTitle(editing ? tr("Manage Project Files") : tr("New Project"));
    setObjectName("projectDialog");
    setFont(UiAppearance::uiFont(settings));
    setStyleSheet(UiAppearance::styleSheet(settings) +
                  QString("QDialog#projectDialog QPushButton { border: 1px solid %1; "
                          "border-radius: 0; padding: 2px 8px; background: %2; }"
                          "QDialog#projectDialog QPushButton:hover { border-color: %3; }"
                          "QDialog#projectDialog QPushButton:disabled { color: %4; }")
                      .arg(settings.color("ui.strongBorder").name(),
                           settings.color("ui.inputBackground").name(),
                           settings.color("ui.hoverBorder").name(),
                           settings.color("menu.disabled").name()));
    resize(860, 520);
    options_.managed = true;
    if (!editing) {
        options_.includedPaths.clear();
        options_.excludedPaths.clear();
    }
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    auto *form = new QFormLayout;
    form->setSpacing(4);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    const auto entry = [this, &settings](const QString &name, const QString &text) {
        auto *edit = new QLineEdit(text, this);
        edit->setObjectName(name);
        edit->setFixedHeight(UiAppearance::bandHeight(settings));
        return edit;
    };
    const auto button = [this, &settings](const QString &name, const QString &text) {
        auto *value = new QPushButton(text, this);
        value->setObjectName(name);
        value->setAutoDefault(false);
        value->setMinimumHeight(UiAppearance::bandHeight(settings));
        return value;
    };
    auto *rootRow = new QHBoxLayout;
    rootRow->setSpacing(4);
    rootEdit_ = entry("projectRoot", root);
    rootEdit_->setPlaceholderText(tr("Choose a base folder"));
    auto *browseRoot = button("projectBrowseRoot", tr("Browse..."));
    rootRow->addWidget(rootEdit_, 1);
    rootRow->addWidget(browseRoot);
    form->addRow(tr("Base folder:"), rootRow);
    nameEdit_ = entry("projectName", options.name);
    form->addRow(tr("Project name:"), nameEdit_);
    auto *fileRow = new QHBoxLayout;
    fileRow->setSpacing(4);
    fileEdit_ = entry("projectFile", options.projectFile);
    fileEdit_->setPlaceholderText(tr("Choose where to save the .cairn project"));
    fileEdit_->setToolTip(tr("The project file can be outside the source tree. "
                             "Indexes and sessions stay in CodeCairn's per-user state folder."));
    auto *browseFile = button("projectBrowseFile", tr("Browse..."));
    fileEdit_->setReadOnly(editing);
    browseFile->setEnabled(!editing);
    customFile_ = editing;
    fileRow->addWidget(fileEdit_, 1);
    fileRow->addWidget(browseFile);
    form->addRow(tr("Project file:"), fileRow);
    layout->addLayout(form);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(settings.ui.splitterWidth);
    const auto panel = [splitter] {
        auto *widget = new QWidget(splitter);
        auto *column = new QVBoxLayout(widget);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);
        return column;
    };
    auto *available = panel();
    available->addWidget(new QLabel(tr("Available files and folders"), this));
    tree_ = new QTreeView(this);
    tree_->setObjectName("projectTree");
    model_ = new AvailableFiles(this);
    tree_->setModel(model_);
    UiAppearance::configureTree(tree_, settings);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setHeaderHidden(true);
    for (int i = 1; i < model_->columnCount(); ++i)
        tree_->hideColumn(i);
    available->addWidget(tree_, 1);
    auto *addRow = new QHBoxLayout;
    addRow->setSpacing(4);
    add_ = button("projectAddSelected", tr("Add Selected"));
    add_->setToolTip(tr("Add selected files and folders. Folders include all descendants."));
    addBase_ = button("projectAddBase", tr("Add Base Folder"));
    addBase_->setToolTip(tr("Include every nonexcluded file under the base folder."));
    addRow->addWidget(add_);
    addRow->addWidget(addBase_);
    addRow->addStretch();
    available->addLayout(addRow);

    auto *selected = panel();
    countLabel_ = new QLabel(tr("Included files (0)"), this);
    countLabel_->setObjectName("projectFileCount");
    selected->addWidget(countLabel_);
    auto *search = entry("projectIncludedFilter", {});
    search->setPlaceholderText(tr("Filter included paths"));
    selected->addWidget(search);
    included_ = new QTreeView(this);
    included_->setObjectName("projectIncludedFiles");
    files_ = new ProjectFileListModel(this);
    filter_ = new QSortFilterProxyModel(this);
    filter_->setSourceModel(files_);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    included_->setModel(filter_);
    UiAppearance::configureTree(included_, settings);
    included_->setRootIsDecorated(false);
    included_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    included_->setHeaderHidden(true);
    selected->addWidget(included_, 1);
    auto *removeRow = new QHBoxLayout;
    removeRow->setSpacing(4);
    remove_ = button("projectRemoveSelected", tr("Remove Selected"));
    clear_ = button("projectClearSelection", tr("Clear All"));
    removeRow->addWidget(remove_);
    removeRow->addWidget(clear_);
    removeRow->addStretch();
    selected->addLayout(removeRow);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    auto *statusRow = new QHBoxLayout;
    statusLabel_ = new QLabel(this);
    statusLabel_->setText(tr("No files included. Add files or folders from the left."));
    statusLabel_->setObjectName("projectScanStatus");
    statusRow->addWidget(statusLabel_, 1);
    stop_ = button("projectStopScan", tr("Stop"));
    statusRow->addWidget(stop_);
    layout->addLayout(statusRow);
    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName("projectError");
    errorLabel_->setWordWrap(true);
    layout->addWidget(errorLabel_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(editing ? tr("Save") : tr("Create"));
    buttons_->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(buttons_);

    connect(buttons_, &QDialogButtonBox::accepted, this, &ProjectDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &ProjectDialog::reject);
    connect(browseRoot, &QPushButton::clicked, this, [this] {
        const auto folder =
            QFileDialog::getExistingDirectory(this, tr("Project Base Folder"), rootEdit_->text());
        if (!folder.isEmpty()) {
            rootEdit_->setText(folder);
            changeRoot();
        }
    });
    connect(browseFile, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(
            this, tr("Choose Project File Location"), fileEdit_->text(),
            tr("CodeCairn projects (*.cairn)"), nullptr, QFileDialog::DontConfirmOverwrite);
        if (!path.isEmpty()) {
            customFile_ = true;
            fileEdit_->setText(path.endsWith(".cairn", Qt::CaseInsensitive) ? path
                                                                            : path + ".cairn");
        }
    });
    connect(rootEdit_, &QLineEdit::editingFinished, this, &ProjectDialog::changeRoot);
    connect(rootEdit_, &QLineEdit::textChanged, this, [this] {
        if (scanErrors_.isEmpty())
            selectionError_.clear();
        updateControls();
    });
    connect(nameEdit_, &QLineEdit::textChanged, this, [this] {
        if (scanErrors_.isEmpty())
            selectionError_.clear();
        updateProjectFile();
        updateControls();
    });
    connect(fileEdit_, &QLineEdit::textEdited, this, [this] { customFile_ = true; });
    connect(fileEdit_, &QLineEdit::textChanged, this, [this] {
        if (scanErrors_.isEmpty())
            selectionError_.clear();
        updateControls();
    });
    connect(search, &QLineEdit::textChanged, filter_, &QSortFilterProxyModel::setFilterFixedString);
    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateControls(); });
    connect(included_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateControls(); });
    connect(add_, &QPushButton::clicked, this, [this] { addSelected(); });
    connect(addBase_, &QPushButton::clicked, this, [this] { addSelected(true); });
    connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!model_->isDir(index))
            addSelected();
    });
    connect(remove_, &QPushButton::clicked, this, &ProjectDialog::removeSelected);
    connect(clear_, &QPushButton::clicked, this, [this] {
        options_.includedPaths.clear();
        options_.excludedPaths.clear();
        requestScan();
    });
    connect(stop_, &QPushButton::clicked, this, [this] {
        if (cancel_)
            cancel_->store(true);
        pendingScan_ = false;
        statusLabel_->setText(tr("Stopping file discovery..."));
    });
    progressTimer_.setInterval(100);
    connect(&progressTimer_, &QTimer::timeout, this, &ProjectDialog::publishFiles);
    connect(&scanJob_, &QFutureWatcher<ScanResult>::finished, this, [this] {
        scanBusy_ = false;
        const auto result = scanJob_.result();
        if (result.generation == generation_) {
            progressTimer_.stop();
            scanErrors_ = result.errors;
            complete_ = !result.cancelled && result.errors.isEmpty();
            files_->replace(result.files);
            statusLabel_->setText(
                result.cancelled
                    ? tr("Discovery stopped. Add again to retry, or clear the selection.")
                    : tr("%1 files included").arg(files_->rowCount()));
            if (!scanErrors_.isEmpty())
                selectionError_ = scanErrors_.join('\n');
        }
        startScan();
        updateControls();
    });
    changeRoot();
    updateControls();
}

ProjectDialog::~ProjectDialog() {
    if (cancel_)
        cancel_->store(true);
    scanJob_.waitForFinished();
}

QString ProjectDialog::root() const {
    const auto text = rootEdit_->text().trimmed();
    return text.isEmpty() ? QString{} : workspacePath(text);
}

QString ProjectDialog::projectFile() const {
    const auto text = fileEdit_->text().trimmed();
    return text.isEmpty() ? QString{} : QDir::cleanPath(text);
}

WorkspaceOptions ProjectDialog::options() const {
    auto result = options_;
    result.name = nameEdit_->text().trimmed();
    return result;
}

void ProjectDialog::setSaveError(const QString &error) {
    qCWarning(storageLog) << "project.selection_failed" << error;
    selectionError_ = error;
    updateControls();
}

void ProjectDialog::changeRoot() {
    const auto requested = root();
    const bool valid = !requested.isEmpty() && QDir::isAbsolutePath(rootEdit_->text().trimmed()) &&
                       QFileInfo(requested).isDir() &&
                       !QFileInfo(rootEdit_->text().trimmed()).isSymLink();
    tree_->setEnabled(valid);
    if (requested != activeRoot_) {
        if (!activeRoot_.isEmpty()) {
            options_.includedPaths.clear();
            options_.excludedPaths.clear();
        }
        activeRoot_ = requested;
        files_->root = requested;
        if (valid) {
            tree_->setRootIndex(model_->setRootPath(requested));
            tree_->selectionModel()->clearSelection();
            const auto name = QFileInfo(requested).fileName();
            if (nameEdit_->text().isEmpty() || nameEdit_->text() == defaultName_)
                nameEdit_->setText(name);
            defaultName_ = name;
        }
        requestScan();
    }
    updateControls();
}

void ProjectDialog::updateProjectFile() {
    if (customFile_)
        return;
    auto name = nameEdit_->text().trimmed();
    name.replace(QRegularExpression("[\\\\/:*?\"<>|\\x00-\\x1f]"), "_");
    const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    fileEdit_->setText(name.isEmpty() || documents.isEmpty()
                           ? QString{}
                           : QDir(documents).filePath(name + ".cairn"));
}

void ProjectDialog::addSelected(bool baseFolder) {
    if (scanBusy_)
        return;
    changeRoot();
    QStringList selected;
    if (baseFolder)
        selected.append(".");
    else
        for (const auto &index : tree_->selectionModel()->selectedRows())
            selected.append(QDir(root()).relativeFilePath(model_->filePath(index)));
    const auto error = includeProjectPaths(root(), options_, selected);
    if (!error.isEmpty()) {
        setSaveError(error);
        return;
    }
    requestScan();
}

void ProjectDialog::removeSelected() {
    QStringList paths;
    for (const auto &index : included_->selectionModel()->selectedRows())
        paths.append(index.data(Qt::UserRole).toString());
    excludeProjectPaths(options_, paths);
    requestScan();
}

void ProjectDialog::requestScan() {
    ++generation_;
    if (cancel_)
        cancel_->store(true);
    progressTimer_.stop();
    progress_.reset();
    pendingScan_ = true;
    complete_ = false;
    selectionError_.clear();
    scanErrors_.clear();
    files_->replace();
    startScan();
    updateControls();
}

void ProjectDialog::startScan() {
    if (scanBusy_ || !pendingScan_)
        return;
    pendingScan_ = false;
    if (options_.includedPaths.isEmpty() || !QFileInfo(root()).isDir()) {
        complete_ = true;
        statusLabel_->setText(tr("No files included. Add files or folders from the left."));
        return;
    }
    const auto base = root();
    const auto options = options_;
    const auto generation = generation_;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    progress_ = std::make_shared<ScanProgress>();
    statusLabel_->setText(tr("Finding included files..."));
    progressTimer_.start();
    scanBusy_ = true;
    scanJob_.setFuture(
        QtConcurrent::run([base, options, generation, cancel = cancel_, progress = progress_] {
            return discoverProjectFiles(base, options, generation, cancel, progress);
        }));
}

void ProjectDialog::publishFiles() {
    if (pendingScan_ || !progress_)
        return;
    QStringList paths;
    {
        QMutexLocker lock(&progress_->mutex);
        paths.swap(progress_->pending);
    }
    files_->append(paths);
    countLabel_->setText(tr("Included files (%1)").arg(files_->rowCount()));
    statusLabel_->setText(tr("Finding included files: %1 found").arg(progress_->found.load()));
}

QString ProjectDialog::validationError() const {
    if (root().isEmpty() || !QDir::isAbsolutePath(rootEdit_->text().trimmed()) ||
        !QFileInfo(root()).isDir() || QFileInfo(rootEdit_->text().trimmed()).isSymLink())
        return tr("Choose an existing, nonsymlinked base folder.");
    if (nameEdit_->text().trimmed().isEmpty())
        return tr("Enter a project name.");
    const auto file = fileEdit_->text().trimmed();
    if (file.isEmpty() || !QDir::isAbsolutePath(file) ||
        QFileInfo(file).suffix().compare("cairn", Qt::CaseInsensitive))
        return tr("Choose an absolute .cairn project file path.");
    if (!QFileInfo(QFileInfo(file).absolutePath()).isDir() || QFileInfo(file).isSymLink())
        return tr("Choose an existing folder and a nonsymlinked project file.");
    if (options_.projectFile.isEmpty() && QFileInfo::exists(file))
        return tr("That project file already exists. Choose another name or use Open Project.");
    if (scanBusy_ || pendingScan_ || !complete_)
        return tr("Wait for file discovery to finish, or clear and retry the selection.");
    if (files_->rowCount() == 0)
        return tr("Add at least one file or folder containing files.");
    if (!options_.error.isEmpty())
        return options_.error;
    return {};
}

void ProjectDialog::updateControls() {
    const bool busy = scanBusy_ || pendingScan_;
    const bool validRoot = !root().isEmpty() && QFileInfo(root()).isDir() &&
                           QDir::isAbsolutePath(rootEdit_->text().trimmed()) &&
                           root() == activeRoot_ &&
                           !QFileInfo(rootEdit_->text().trimmed()).isSymLink();
    add_->setEnabled(validRoot && !busy && !tree_->selectionModel()->selectedRows().isEmpty());
    addBase_->setEnabled(validRoot && !busy);
    remove_->setEnabled(!busy && !included_->selectionModel()->selectedRows().isEmpty());
    clear_->setEnabled(!options_.includedPaths.isEmpty());
    stop_->setEnabled(busy);
    countLabel_->setText(tr("Included files (%1)").arg(files_->rowCount()));
    const auto validation = validationError();
    const bool started = !rootEdit_->text().isEmpty() || !nameEdit_->text().isEmpty() ||
                         !fileEdit_->text().isEmpty();
    errorLabel_->setText(!selectionError_.isEmpty() ? selectionError_
                         : started && !busy         ? validation
                                                    : QString{});
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(validation.isEmpty());
}

void ProjectDialog::accept() {
    changeRoot();
    const auto error = validationError();
    if (!error.isEmpty()) {
        errorLabel_->setText(error);
        return;
    }
    QDialog::accept();
}
