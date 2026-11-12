#pragma once

#include "AppSettings.h"
#include <codecairn/WorkspaceOptionsStore.h>
#include <codecairn/ProjectDiscovery.h>
#include <QDialog>
#include <QFutureWatcher>
#include <QTimer>
#include <atomic>
#include <memory>

class QDialogButtonBox;
class QFileSystemModel;
class QLineEdit;
class QLabel;
class QPushButton;
class QSortFilterProxyModel;
class QTreeView;
class ProjectFileListModel;

class ProjectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectDialog(const QString &root = {}, const WorkspaceOptions &options = {},
                           QWidget *parent = nullptr,
                           const AppSettings &settings = defaultAppSettings());
    ~ProjectDialog() override;
    QString root() const;
    QString projectFile() const;
    WorkspaceOptions options() const;
    void setSaveError(const QString &error);
    void accept() override;

private:
    using ScanResult = ProjectScanResult;
    using ScanProgress = ProjectScanProgress;
    void changeRoot();
    void updateProjectFile();
    void addSelected(bool baseFolder = false);
    void removeSelected();
    void requestScan();
    void startScan();
    void publishFiles();
    void updateControls();
    QString validationError() const;
    WorkspaceOptions options_;
    QString activeRoot_;
    QString defaultName_;
    QString selectionError_;
    QStringList scanErrors_;
    QLineEdit *rootEdit_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *fileEdit_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QLabel *countLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QTreeView *tree_ = nullptr;
    QTreeView *included_ = nullptr;
    QFileSystemModel *model_ = nullptr;
    ProjectFileListModel *files_ = nullptr;
    QSortFilterProxyModel *filter_ = nullptr;
    QPushButton *add_ = nullptr;
    QPushButton *addBase_ = nullptr;
    QPushButton *remove_ = nullptr;
    QPushButton *clear_ = nullptr;
    QPushButton *stop_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
    QFutureWatcher<ScanResult> scanJob_;
    QTimer progressTimer_;
    std::shared_ptr<std::atomic_bool> cancel_;
    std::shared_ptr<ScanProgress> progress_;
    quint64 generation_ = 0;
    bool pendingScan_ = false;
    bool scanBusy_ = false;
    bool complete_ = true;
    bool customFile_ = false;
};
