#include "ProjectDialog.h"
#include "WorkspaceOptionsStore.h"
#include "WorkspacePaths.h"
#include "TestStorage.h"
#include <QDir>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileSystemModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTreeView>
#include <QUuid>

namespace {
class ProjectFixture {
public:
    ProjectFixture()
        : path(QDir::current().filePath("project-fixture-" +
                                        QUuid::createUuid().toString(QUuid::WithoutBraces))) {
        if (!QDir().mkpath(path))
            qFatal("Could not create project fixture.");
    }
    ~ProjectFixture() {
        if (!QDir(path).removeRecursively())
            qWarning("Could not remove project fixture.");
    }
    QString file(const QString &relative) const { return QDir(path).filePath(relative); }
    void write(const QString &relative, const QByteArray &bytes = "contents") {
        const auto target = file(relative);
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            qFatal("Could not create project fixture directory.");
        QFile output(target);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size())
            qFatal("Could not write project fixture.");
    }
    QString path;
};

WorkspaceOptions selected(const QStringList &paths = {"src"}) {
    WorkspaceOptions options;
    options.managed = true;
    options.name = "My Project";
    options.includedPaths = paths;
    options.excludedDirectories = QStringList{};
    return options;
}
}

class ProjectTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void descriptorRoundTripAndIdentity();
    void scopedMembershipAndTraversal();
    void invalidAndSymlinkRules();
    void descriptorWriteSafety();
    void freshOptionsAndAncestorAliases();
    void lazyDialogSelectionAndValidation();
    void blankDialogUsesConfiguredAppearance();
    void projectFieldsFillAvailableWidth();
    void cancellationAndRootChangesDiscardOldResults();
    void projectLocationSurvivesRenamingAndRootChanges();
};

void ProjectTests::descriptorRoundTripAndIdentity() {
    ProjectFixture fixture;
    fixture.write("base/src/main.cpp");
    const auto root = fixture.file("base");
    const auto first = fixture.file("one.cairn");
    const auto second = fixture.file("two.cairn");
    auto options = selected();
    options.excludedPaths = {"src/generated"};
    options.compilationDirectory = "compile";
    QVERIFY(WorkspaceOptionsStore::saveProject(first, root, options).isEmpty());
    QVERIFY(WorkspaceOptionsStore::saveProject(second, root, options).isEmpty());
    const auto loaded = WorkspaceOptionsStore::loadProject(first);
    QVERIFY2(loaded.error.isEmpty(), qPrintable(loaded.error));
    QCOMPARE(loaded.root, root);
    QVERIFY(loaded.options.managed);
    QCOMPARE(loaded.options.name, options.name);
    QCOMPARE(loaded.options.includedPaths, options.includedPaths);
    QCOMPARE(loaded.options.excludedPaths, options.excludedPaths);
    QCOMPARE(loaded.options.excludedDirectories, options.excludedDirectories);
    QCOMPARE(workspaceOptionsPath(root, first), first);
    QVERIFY(workspaceDatabase(root, first) != workspaceDatabase(root, second));
    QVERIFY(workspaceDatabase(root, first) != workspaceDatabase(root));
    QVERIFY(workspaceClangdDirectory(root, first) != workspaceClangdDirectory(root, second));
    QCOMPARE(QFileInfo(workspaceDatabase(root, first)).absolutePath(),
             workspaceStorageDirectory(root, {}, first));
    QVERIFY(WorkspaceOptionsStore::initialize(root, first).isEmpty());
    QVERIFY(QFileInfo(workspaceStorageDirectory(root, {}, first)).isDir());
#ifdef Q_OS_UNIX
    const auto secondStorage = workspaceStorageDirectory(root, {}, second);
    QVERIFY(QFile::link(fixture.path, secondStorage));
    QVERIFY(!WorkspaceOptionsStore::initialize(root, second).isEmpty());
    QVERIFY(QFile::remove(secondStorage));
#endif
    QVERIFY(WorkspaceOptionsStore::saveOptions(root, "new-build", {"vendor"}, first).isEmpty());
    const auto updated = WorkspaceOptionsStore::loadOptions(root, first);
    QCOMPARE(updated.name, options.name);
    QCOMPARE(updated.includedPaths, options.includedPaths);
    QCOMPARE(updated.excludedPaths, options.excludedPaths);
    QCOMPARE(updated.compilationDirectory, "new-build");
    QCOMPARE(updated.excludedDirectories.value(), QStringList{"vendor"});
    QVERIFY(!WorkspaceOptionsStore::loadOptions(fixture.path, first).error.isEmpty());
}

void ProjectTests::scopedMembershipAndTraversal() {
    ProjectFixture fixture;
    for (const auto &file : {"src/app/a.cpp", "src/app/generated/a.cpp", "src/lib/b.cpp",
                             "notes.txt", "other.txt", "vendor/a.cpp"})
        fixture.write(file);
    auto options = selected({"src/app", "notes.txt"});
    options.excludedPaths = {"src/app/generated"};
    QVERIFY(projectIncludesPath(fixture.path, fixture.file("src/app/a.cpp"), options));
    QVERIFY(projectIncludesPath(fixture.path, fixture.file("notes.txt"), options));
    QVERIFY(!projectIncludesPath(fixture.path, fixture.file("src/lib/b.cpp"), options));
    QVERIFY(!projectIncludesPath(fixture.path, fixture.file("src/app/generated/a.cpp"), options));
    QVERIFY(!projectIncludesPath(fixture.path, fixture.file("other.txt"), options));
    QVERIFY(projectTraversesDirectory(fixture.path, fixture.path, options));
    QVERIFY(projectTraversesDirectory(fixture.path, fixture.file("src"), options));
    QVERIFY(!projectTraversesDirectory(fixture.path, fixture.file("src/lib"), options));
    QVERIFY(!projectTraversesDirectory(fixture.path, fixture.file("src/app/generated"), options));
    options.includedPaths = {"."};
    options.excludedDirectories = QStringList{"vendor"};
    QVERIFY(!projectIncludesPath(fixture.path, fixture.file("vendor/a.cpp"), options));
    QVERIFY(!projectIncludesPath(fixture.path, fixture.file("../outside.cpp"), options));
    options.includedPaths = {"src/app"};
    options.excludedPaths = {"src"};
    QVERIFY(!projectTraversesDirectory(fixture.path, fixture.path, options));
    WorkspaceOptions folder;
    QVERIFY(projectIncludesPath(fixture.path, fixture.file("other.txt"), folder));
}

void ProjectTests::invalidAndSymlinkRules() {
    ProjectFixture fixture;
    fixture.write("src/a.cpp");
    const auto descriptor = fixture.file("test.cairn");
    for (const auto &rule :
         {"../src", "/absolute", "src/../other", "", "a//b", "C:/src", "src\\child", "./src"}) {
        auto options = selected({rule});
        QVERIFY2(!WorkspaceOptionsStore::saveProject(descriptor, fixture.path, options).isEmpty(),
                 rule);
        QVERIFY(!projectIncludesPath(fixture.path, fixture.file("src/a.cpp"), options));
    }
    auto options = selected({"."});
    options.excludedPaths = {"../escape"};
    QVERIFY(!WorkspaceOptionsStore::saveProject(descriptor, fixture.path, options).isEmpty());
    options.excludedPaths.clear();
#ifdef Q_OS_UNIX
    QVERIFY(QFile::link(fixture.file("src"), fixture.file("linked")));
    QVERIFY(!projectIncludesPath(fixture.path, fixture.file("linked/a.cpp"), options));
    QVERIFY(!projectTraversesDirectory(fixture.path, fixture.file("linked"), options));
#endif
    QVERIFY(WorkspaceOptionsStore::saveProject(descriptor, fixture.path, options).isEmpty());
    QFile file(descriptor);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto object = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    object["includedPaths"] = QJsonArray{"../bad"};
    fixture.write("test.cairn", QJsonDocument(object).toJson());
    QVERIFY(!WorkspaceOptionsStore::loadProject(descriptor).error.isEmpty());
}

void ProjectTests::descriptorWriteSafety() {
    ProjectFixture fixture;
    fixture.write("src/a.cpp");
    const auto descriptor = fixture.file("test.cairn");
    QVERIFY(WorkspaceOptionsStore::saveProject(descriptor, fixture.path, selected()).isEmpty());
    auto stale = WorkspaceOptionsStore::loadProject(descriptor).options;
    ProjectDialog openDialog(fixture.path, stale);
    QCOMPARE(openDialog.options().projectFile, stale.projectFile);
    QCOMPARE(openDialog.options().projectDigest, stale.projectDigest);
    auto changed = stale;
    changed.name = "Changed";
    QVERIFY(WorkspaceOptionsStore::saveProject(descriptor, fixture.path, changed).isEmpty());
    QVERIFY(!WorkspaceOptionsStore::saveProject(descriptor, fixture.path, stale).isEmpty());
    QVERIFY(!WorkspaceOptionsStore::saveProject(descriptor, fixture.path, openDialog.options())
                 .isEmpty());
    QCOMPARE(WorkspaceOptionsStore::loadProject(descriptor).options.name, "Changed");
    auto current = WorkspaceOptionsStore::loadProject(descriptor).options;
    QLockFile lock(descriptor + ".lock");
    QVERIFY(lock.tryLock(0));
    QVERIFY(!WorkspaceOptionsStore::saveProject(descriptor, fixture.path, current).isEmpty());
    lock.unlock();
    fixture.write("test.cairn", "{broken");
    QVERIFY(!WorkspaceOptionsStore::saveProject(descriptor, fixture.path, current).isEmpty());
    QFile file(descriptor);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{broken"));
    file.close();
    QVERIFY(QFile::remove(descriptor));
    QVERIFY(!WorkspaceOptionsStore::saveProject(descriptor, fixture.path, current).isEmpty());
    QVERIFY(WorkspaceOptionsStore::saveProject(fixture.file("copy.cairn"), fixture.path, current)
                .isEmpty());
}

void ProjectTests::freshOptionsAndAncestorAliases() {
    ProjectFixture fixture;
    fixture.write("actual/base/first.c");
    WorkspaceOptions options;
    options.managed = true;
    options.name = "First subset";
    options.includedPaths = {"first.c"};
    auto parent = fixture.file("actual");
#ifdef Q_OS_UNIX
    QVERIFY(QFile::link(parent, fixture.file("alias")));
    parent = fixture.file("alias");
#endif
    const auto root = QDir(parent).filePath("base");
    const auto descriptor = QDir(parent).filePath("first.cairn");
    const auto error = WorkspaceOptionsStore::saveProject(descriptor, root, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto loaded = WorkspaceOptionsStore::loadProject(descriptor);
    QVERIFY2(loaded.error.isEmpty(), qPrintable(loaded.error));
    QVERIFY(!loaded.options.excludedDirectories.has_value());
    QCOMPARE(loaded.root, workspacePath(root));
    QVERIFY(projectIncludesPath(root, QDir(root).filePath("first.c"), loaded.options));
    QVERIFY(
        projectIncludesPath(root, workspacePath(QDir(root).filePath("first.c")), loaded.options));
    QVERIFY(projectTraversesDirectory(root, root, loaded.options));
    ProjectDialog dialog(root);
    dialog.findChild<QPushButton *>("projectAddBase")->click();
    QTRY_COMPARE(dialog.findChild<QTreeView *>("projectIncludedFiles")->model()->rowCount(), 1);
    const auto uiError = WorkspaceOptionsStore::saveProject(QDir(parent).filePath("ui.cairn"),
                                                            dialog.root(), dialog.options());
    QVERIFY2(uiError.isEmpty(), qPrintable(uiError));
}

void ProjectTests::lazyDialogSelectionAndValidation() {
    ProjectFixture fixture;
    fixture.write("src/a.cpp");
    fixture.write("src/nested/b.cpp");
    ProjectDialog dialog(fixture.path);
    QCOMPARE(dialog.root(), fixture.path);
    QCOMPARE(dialog.options().name, QFileInfo(fixture.path).fileName());
    QVERIFY(dialog.options().includedPaths.isEmpty());
    auto *tree = dialog.findChild<QTreeView *>("projectTree");
    QVERIFY(tree);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    QVERIFY(model);
    QTRY_VERIFY(model->index(fixture.file("src")).isValid());
    QVERIFY(!tree->isExpanded(model->index(fixture.file("src"))));
    dialog.accept();
    QVERIFY(!dialog.findChild<QLabel *>("projectError")->text().isEmpty());
    const auto sourceIndex = model->index(fixture.file("src"));
    tree->selectionModel()->select(sourceIndex,
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto *add = dialog.findChild<QPushButton *>("projectAddSelected");
    QVERIFY(add->isEnabled());
    add->click();
    auto *included = dialog.findChild<QTreeView *>("projectIncludedFiles");
    QTRY_COMPARE(included->model()->rowCount(), 2);
    QCOMPARE(dialog.options().includedPaths, QStringList{"src"});
    QCOMPARE(included->model()->index(0, 0).data(Qt::UserRole).toString(), "src/a.cpp");
    QCOMPARE(included->model()->index(1, 0).data(Qt::UserRole).toString(), "src/nested/b.cpp");
    auto *filter = dialog.findChild<QLineEdit *>("projectIncludedFilter");
    filter->setText("nested");
    QCOMPARE(included->model()->rowCount(), 1);
    included->selectionModel()->select(included->model()->index(0, 0),
                                       QItemSelectionModel::ClearAndSelect |
                                           QItemSelectionModel::Rows);
    dialog.findChild<QPushButton *>("projectRemoveSelected")->click();
    QTRY_COMPARE(included->model()->rowCount(), 0);
    filter->clear();
    QTRY_COMPARE(included->model()->rowCount(), 1);
    QCOMPARE(dialog.options().excludedPaths, QStringList{"src/nested/b.cpp"});
    QTRY_VERIFY(!dialog.findChild<QPushButton *>("projectStopScan")->isEnabled());
    tree->selectionModel()->select(model->index(fixture.file("src/nested/b.cpp")),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    add->click();
    QTRY_COMPARE(included->model()->rowCount(), 2);
    QTRY_VERIFY(!dialog.findChild<QPushButton *>("projectStopScan")->isEnabled());
    dialog.findChild<QLineEdit *>("projectName")->setText("Renamed");
    const auto descriptor = fixture.file("saved.cairn");
    dialog.findChild<QLineEdit *>("projectFile")->setText(descriptor);
    const auto capture = qEnvironmentVariable("CODECAIRN_PROJECT_DIALOG_CAPTURE");
    if (!capture.isEmpty()) {
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(dialog.grab().save(capture));
    }
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.options().name, "Renamed");
    QCOMPARE(dialog.projectFile(), descriptor);
    QVERIFY(
        WorkspaceOptionsStore::saveProject(descriptor, dialog.root(), dialog.options()).isEmpty());
    const auto saved = WorkspaceOptionsStore::loadProject(descriptor);
    ProjectDialog edited(fixture.path, saved.options);
    QCOMPARE(edited.options().includedPaths, dialog.options().includedPaths);
    QCOMPARE(edited.options().excludedPaths, dialog.options().excludedPaths);
    QTRY_COMPARE(edited.findChild<QTreeView *>("projectIncludedFiles")->model()->rowCount(), 2);
    QCOMPARE(edited.projectFile(), descriptor);
    QVERIFY(edited.findChild<QLineEdit *>("projectFile")->isReadOnly());
}

void ProjectTests::blankDialogUsesConfiguredAppearance() {
    auto settings = defaultAppSettings();
    settings.ui.fontSizePx = 15;
    settings.colors["ui.panelBackground"] = QColor("#182838");
    ProjectDialog dialog({}, {}, nullptr, settings);
    QVERIFY(dialog.root().isEmpty());
    QVERIFY(dialog.projectFile().isEmpty());
    QVERIFY(dialog.options().name.isEmpty());
    QVERIFY(dialog.options().includedPaths.isEmpty());
    QCOMPARE(dialog.font().pixelSize(), 15);
    QVERIFY(dialog.styleSheet().contains("#182838"));
    auto *included = dialog.findChild<QTreeView *>("projectIncludedFiles");
    QCOMPARE(included->font().pixelSize(), 15);
    QCOMPARE(included->iconSize(), QSize(settings.ui.symbolIconSize, settings.ui.symbolIconSize));
    QCOMPARE(included->model()->rowCount(), 0);
    QVERIFY(!dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(!dialog.findChild<QPushButton *>("projectAddBase")->isEnabled());
}

void ProjectTests::projectFieldsFillAvailableWidth() {
    ProjectDialog dialog;
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *root = dialog.findChild<QLineEdit *>("projectRoot");
    auto *name = dialog.findChild<QLineEdit *>("projectName");
    auto *file = dialog.findChild<QLineEdit *>("projectFile");
    auto *browse = dialog.findChild<QPushButton *>("projectBrowseRoot");
    QCOMPARE(root->x(), name->x());
    QCOMPARE(file->x(), name->x());
    QCOMPARE(root->width(), file->width());
    QVERIFY(root->width() > dialog.width() * 0.65);
    QVERIFY(name->width() > dialog.width() * 0.75);
    QCOMPARE(name->geometry().right(), browse->geometry().right());
    const int previousWidth = root->width();
    dialog.resize(dialog.width() + 200, dialog.height());
    QTRY_COMPARE(root->width(), previousWidth + 200);
    QCOMPARE(name->geometry().right(), browse->geometry().right());
}

void ProjectTests::projectLocationSurvivesRenamingAndRootChanges() {
    ProjectFixture fixture;
    fixture.write("base/main.c");
    fixture.write("another/other.c");
    QVERIFY(QDir().mkpath(fixture.file("projects")));
    ProjectDialog dialog(fixture.file("base"));
    QVERIFY(!pathInWorkspace(dialog.projectFile(), fixture.file("base")));
    const auto descriptor = fixture.file("projects/custom.cairn");
    auto *file = dialog.findChild<QLineEdit *>("projectFile");
    file->setText(descriptor);
    QVERIFY(QMetaObject::invokeMethod(file, "textEdited", Qt::DirectConnection,
                                      Q_ARG(QString, descriptor)));
    dialog.findChild<QLineEdit *>("projectName")->setText("Custom name");
    QCOMPARE(dialog.projectFile(), descriptor);
    auto *root = dialog.findChild<QLineEdit *>("projectRoot");
    root->setText(fixture.file("another"));
    QVERIFY(QMetaObject::invokeMethod(root, "editingFinished", Qt::DirectConnection));
    QCOMPARE(dialog.options().name, QString("Custom name"));
    QCOMPARE(dialog.projectFile(), descriptor);
    dialog.findChild<QPushButton *>("projectAddBase")->click();
    QTRY_VERIFY(!dialog.findChild<QPushButton *>("projectStopScan")->isEnabled());
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(
        WorkspaceOptionsStore::saveProject(dialog.projectFile(), dialog.root(), dialog.options())
            .isEmpty());
    QVERIFY(QFileInfo::exists(descriptor));
    QCOMPARE(QDir(fixture.file("another")).entryList(QDir::Files), QStringList{"other.c"});
}

void ProjectTests::cancellationAndRootChangesDiscardOldResults() {
    ProjectFixture fixture;
    for (int i = 0; i < 2000; ++i)
        fixture.write(QString("first/file_%1.c").arg(i));
    fixture.write("second/only.c");
    ProjectDialog dialog(fixture.file("first"));
    auto *included = dialog.findChild<QTreeView *>("projectIncludedFiles");
    auto *add = dialog.findChild<QPushButton *>("projectAddBase");
    add->click();
    dialog.findChild<QPushButton *>("projectStopScan")->click();
    QTRY_VERIFY(!dialog.findChild<QPushButton *>("projectStopScan")->isEnabled());
    QVERIFY(!dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(dialog.findChild<QLabel *>("projectScanStatus")->text().contains("stopped"));
    dialog.findChild<QPushButton *>("projectClearSelection")->click();
    QCOMPARE(included->model()->rowCount(), 0);
    add->click();
    auto *root = dialog.findChild<QLineEdit *>("projectRoot");
    root->setText(fixture.file("second"));
    QVERIFY(QMetaObject::invokeMethod(root, "editingFinished", Qt::DirectConnection));
    QTRY_VERIFY(!dialog.findChild<QPushButton *>("projectStopScan")->isEnabled());
    QCOMPARE(included->model()->rowCount(), 0);
    QVERIFY(dialog.options().includedPaths.isEmpty());
    add->click();
    QTRY_COMPARE(included->model()->rowCount(), 1);
    QCOMPARE(included->model()->index(0, 0).data(Qt::UserRole).toString(), "only.c");
    dialog.findChild<QPushButton *>("projectClearSelection")->click();
    QCOMPARE(included->model()->rowCount(), 0);
}

QTEST_MAIN(ProjectTests)
#include "ProjectTests.moc"
