#include "MainWindow.h"
#include "ProjectSymbolQuery.h"
#include <codecairn/Diagnostics.h>
#include "LanguageServerClient.h"
#include "SymbolCatalogModel.h"
#include "ProjectIndexStore.h"
#include "TestStorage.h"
#include <codecairn/Indexing.h>
#include <codecairn/WorkspaceOptionsStore.h>

#include <QElapsedTimer>
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <sqlite3.h>

#include "ProjectIndexer.h"
using namespace codecairn::detail;

class SymbolScalingTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void largeCatalogRemainsBrowsableAndNavigable() {
        QTemporaryDir directory;
        const auto root = workspacePath(directory.path());
        const auto path = QDir(root).filePath("symbols.c");
        const QByteArray source = "int needle(void) { return 1; }\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        const auto catalogPath = QDir(root).filePath("catalog.c");
        QVERIFY(writeSourceFile(catalogPath, "int unused;\n", false).isEmpty());
        const auto built = buildWorkspaceIndex(root, IndexRefresh::DiscoverChanges, {},
                                               std::make_shared<std::atomic_bool>(false));
        QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
        const int requested = qEnvironmentVariableIntValue("CODECAIRN_SYMBOL_SCALE");
        const int count = requested > 0 ? requested : 100000;
        {
            ProjectIndexStore db(workspaceDatabase(root), true);
            db.ensureSchema();
            db.exec("BEGIN; DELETE FROM symbols;");
            const auto sql =
                QByteArray(
                    "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM n WHERE i+1<") +
                QByteArray::number(count) +
                ") INSERT INTO symbols SELECT CASE WHEN i=0 THEN ? ELSE ? END,"
                "CASE WHEN i=0 THEN 'needle' ELSE "
                "printf('symbol_%09d',i) END,'Function','','',1,0,30,4,10,0,"
                "'Function',0,'syntax' FROM n;";
            IndexStatement insert(db, sql.constData());
            insert.bind(1, path);
            insert.bind(2, catalogPath);
            insert.next();
            db.exec("COMMIT;");
        }
        MainWindow window(false);
        window.show();
        window.openProject(root);
        auto *index = window.findChild<ProjectIndexer *>();
        QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
        index->cancel();
        SymbolQuery summaryQuery;
        summaryQuery.limit = 4097;
        summaryQuery.includeCategorySymbols = true;
        summaryQuery.categoryLimit = 4097;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto summary = queryWorkspaceSymbols(root, summaryQuery);
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QCOMPARE(summary.total, count);
        QCOMPARE(summary.symbols.size(), qMin(count, 4097));
        QCOMPARE(summary.categorySymbols.size(), qMin(count, 4097));
        qInfo() << "symbols=" << count << "overviewMs=" << elapsed.elapsed();
        QVERIFY(elapsed.elapsed() < 10000);

        SymbolQuery lookup;
        lookup.text = "needle";
        lookup.exact = true;
        lookup.includeCategories = false;
        elapsed.restart();
        const auto target = queryWorkspaceSymbols(root, lookup);
        QVERIFY2(target.error.isEmpty(), qPrintable(target.error));
        QCOMPARE(target.total, 1);
        QCOMPARE(target.symbols.first().symbol.name, QString("needle"));
        qInfo() << "exactLookupMs=" << elapsed.elapsed();
        QVERIFY(elapsed.elapsed() < 2000);

        SymbolCatalogModel changing(false);
        changing.setCatalog(root, summaryQuery, summary);
        changing.entry(changing.index(count - 1, 0));
        auto growing = summary;
        QTimer countChanges;
        countChanges.setInterval(5);
        connect(&countChanges, &QTimer::timeout, &window, [&] {
            ++growing.total;
            ++growing.categories["Functions"];
            changing.setCatalog(root, summaryQuery, growing);
        });
        countChanges.start();
        QTimer::singleShot(50, &countChanges, &QTimer::stop);
        QTRY_VERIFY_WITH_TIMEOUT(changing.entry(changing.index(count - 1, 0)).has_value(), 10000);
        countChanges.stop();

        auto *queries = window.findChild<codecairn::detail::ProjectSymbolQuery *>();
        QSignalSpy replies(queries, &codecairn::detail::ProjectSymbolQuery::ready);
        QTimer refreshes;
        refreshes.setInterval(5);
        connect(&refreshes, &QTimer::timeout, &window, [&] { queries->search(summaryQuery); });
        refreshes.start();
        QTRY_VERIFY_WITH_TIMEOUT(!replies.isEmpty(), 10000);
        refreshes.stop();
        const auto reply = qvariant_cast<SymbolPage>(replies.last().first());
        QVERIFY(!reply.cancelled);
        QCOMPARE(reply.total, count);

        window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
        auto *view = window.findChild<QTreeView *>("virtualSymbolList");
        auto *model = qobject_cast<SymbolCatalogModel *>(view->model());
        QVERIFY(model);
        QTRY_VERIFY(view->isVisible());
        QCOMPARE(model->rowCount(), count);
        auto last = model->index(count - 1, 0);
        view->scrollTo(last);
        QTRY_VERIFY_WITH_TIMEOUT(model->entry(last).has_value(), 10000);
        QCOMPARE(model->entry(last)->symbol.name,
                 QString("symbol_%1").arg(count - 1, 9, 10, QLatin1Char('0')));
        QVERIFY(model->cachedRows() <= 16 * 128);
        const auto memory = Diagnostics::processMemory();
        QVERIFY2(memory.error.isEmpty(), qPrintable(memory.error));
        qInfo() << "virtualBrowserResidentBytes=" << memory.residentBytes;
        const auto capture = qEnvironmentVariable("CODECAIRN_SYMBOL_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(50);
            QVERIFY(window.grab().save(capture));
        }

        view->scrollTo(model->index(0, 0));
        QTRY_VERIFY(model->entry(model->index(0, 0)).has_value());
        view->setFocus();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                          view->visualRect(model->index(0, 0)).center());
        QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), source, 10000);
        QCOMPARE(window.contextEditor()->send(SCI_GETSELECTIONSTART), 4);
        QCOMPARE(window.contextEditor()->send(SCI_GETSELECTIONEND), 10);

        auto *categories = window.findChild<QTreeView *>("virtualCategoryTree");
        auto *categoryModel = qobject_cast<SymbolCatalogModel *>(categories->model());
        QCOMPARE(categoryModel->rowCount(), 1);
        QCOMPARE(categoryModel->rowCount(categoryModel->index(0, 0)), count);
        auto *filter = window.findChild<QLineEdit *>("symbolFilter");
        elapsed.restart();
        filter->setText("needle");
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel *>("symbolScope")->text(),
                                  QString("1 symbols"), 20000);
        qInfo() << "filterMs=" << elapsed.elapsed() << "cachedSymbolRows=" << model->cachedRows();
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), count, 10000);
        window.closeProject();
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(categoryModel->rowCount(), 0);
    }

    void growingCatalogRefetchesShortAndShiftedPages() {
        QTemporaryDir directory;
        const auto root = workspacePath(directory.path());
        const auto path = QDir(root).filePath("source.c");
        QVERIFY(writeSourceFile(path, "int source;\n", false).isEmpty());
        QVERIFY(buildWorkspaceIndex(root, IndexRefresh::DiscoverChanges, {},
                                    std::make_shared<std::atomic_bool>(false))
                    .error.isEmpty());
        {
            ProjectIndexStore db(workspaceDatabase(root), true);
            db.exec("BEGIN; DELETE FROM symbols;"
                    "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM n WHERE i<299)"
                    "INSERT INTO symbols SELECT path,printf('name_%03d',i),'Variable','','',1,"
                    "0,11,4,10,0,'Variable',0,'syntax' FROM files,n; COMMIT;");
        }
        SymbolQuery query;
        query.limit = 128;
        auto summary = queryWorkspaceSymbols(root, query);
        QCOMPARE(summary.total, 300);
        SymbolCatalogModel model(false);
        model.setCatalog(root, query, summary);
        QTRY_VERIFY(model.entry(model.index(299, 0)).has_value());
        QCOMPARE(model.entry(model.index(299, 0))->symbol.name, QString("name_299"));
        {
            ProjectIndexStore db(workspaceDatabase(root), true);
            db.exec("INSERT INTO symbols SELECT path,'zzz_new','Variable','','',1,"
                    "0,11,4,10,0,'Variable',0,'syntax' FROM files;");
        }
        summary = queryWorkspaceSymbols(root, query);
        QCOMPARE(summary.total, 301);
        model.setCatalog(root, query, summary);
        QTRY_VERIFY(model.entry(model.index(300, 0)).has_value());
        QCOMPARE(model.entry(model.index(300, 0))->symbol.name, QString("zzz_new"));
        {
            ProjectIndexStore db(workspaceDatabase(root), true);
            db.exec("INSERT INTO symbols SELECT path,'aaa_new','Variable','','',1,"
                    "0,11,4,10,0,'Variable',0,'syntax' FROM files;");
        }
        summary = queryWorkspaceSymbols(root, query);
        model.setCatalog(root, query, summary);
        QTRY_VERIFY(model.entry(model.index(299, 0)).has_value());
        QCOMPARE(model.entry(model.index(299, 0))->symbol.name, QString("name_298"));
    }

    void deferredEnterStaysWithItsSelectionAndFiltersRestoreIt() {
        QTemporaryDir directory;
        QByteArray source;
        for (int i = 0; i < 6500; ++i)
            source += "int function_" + QByteArray::number(i).rightJustified(5, '0') +
                      "(void) { return 1; }\n";
        const auto path = directory.filePath("many.c");
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        MainWindow window(false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openProject(directory.path());
        auto *index = window.findChild<ProjectIndexer *>();
        QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
        window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
        auto *view = window.findChild<QTreeView *>("virtualSymbolList");
        auto *model = qobject_cast<SymbolCatalogModel *>(view->model());
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 6500, 10000);
        QTRY_VERIFY(model->entry(model->index(0, 0)).has_value());
        QSignalSpy rows(model, &SymbolCatalogModel::rowsReady);
        view->setFocus();
        view->setCurrentIndex(model->index(3000, 0));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(view, &enter);
        view->setCurrentIndex(model->index(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!rows.isEmpty(), 10000);
        QTest::qWait(100);
        QCOMPARE(window.documentCount(), 0);
        auto *filter = window.findChild<QLineEdit *>("symbolFilter");
        filter->setFocus();
        filter->setText("function_05000");
        auto *small = window.findChild<QTreeWidget *>("symbolList");
        QTRY_COMPARE_WITH_TIMEOUT(small->topLevelItemCount(), 1, 10000);
        QTest::keyClick(filter, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(window.documentCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(filter->text().isEmpty(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 6500, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(view->currentIndex().row(), 5000, 10000);
        QTRY_VERIFY(model->entry(view->currentIndex()).has_value());
        QCOMPARE(model->entry(view->currentIndex())->symbol.name, QString("function_05000"));
    }

    void isolatedLinuxIndexSnapshot() {
        const auto snapshot = qEnvironmentVariable("CODECAIRN_AUDIT_INDEX_SNAPSHOT");
        const auto root = qEnvironmentVariable("CODECAIRN_AUDIT_WORKSPACE_ROOT");
        if (snapshot.isEmpty() || root.isEmpty())
            QSKIP("Set an isolated database snapshot and source root for real Linux validation.");
        QTemporaryDir directory;
        const auto projectFile = directory.filePath("linux-audit.cairn");
        WorkspaceOptions options;
        options.managed = true;
        options.name = "Linux audit";
        options.includedPaths = {"."};
        QVERIFY(WorkspaceOptionsStore::saveProject(projectFile, root, options).isEmpty());
        QVERIFY(WorkspaceOptionsStore::initialize(root, projectFile).isEmpty());
        {
            ProjectIndexStore source(snapshot, false);
            ProjectIndexStore target(workspaceDatabase(root, projectFile), true);
            auto *backup = sqlite3_backup_init(target.handle(), "main", source.handle(), "main");
            QVERIFY2(backup, sqlite3_errmsg(target.handle()));
            const int copied = sqlite3_backup_step(backup, -1);
            QCOMPARE(sqlite3_backup_finish(backup), SQLITE_OK);
            QCOMPARE(copied, SQLITE_DONE);
            target.ensureSchema();
        }
        SymbolQuery query;
        query.projectFile = projectFile;
        query.limit = 4097;
        query.categoryLimit = 4097;
        query.includeCategorySymbols = true;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto summary = queryWorkspaceSymbols(root, query);
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY(summary.total > 100000);
        qInfo() << "Linux snapshot symbols=" << summary.total << "overviewMs=" << elapsed.elapsed();
        MainWindow window(false);
        window.show();
        window.openManagedProject(projectFile);
        auto *index = window.findChild<ProjectIndexer *>();
        index->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 30000);
        auto *view = window.findChild<QTreeView *>("virtualSymbolList");
        auto *model = qobject_cast<SymbolCatalogModel *>(view->model());
        window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), summary.total, 30000);
        const auto last = model->index(summary.total - 1, 0);
        view->scrollTo(last);
        QTRY_VERIFY_WITH_TIMEOUT(model->entry(last).has_value(), 30000);
        QVERIFY(!model->entry(last)->symbol.name.isEmpty());
        QVERIFY(model->cachedRows() <= 16 * 128);

        SymbolQuery lookup;
        lookup.projectFile = projectFile;
        lookup.includeCategories = false;
        lookup.text = "amdgpu_device_init";
        lookup.exact = true;
        elapsed.restart();
        const auto matches = queryWorkspaceSymbols(root, lookup);
        QVERIFY2(matches.error.isEmpty(), qPrintable(matches.error));
        QVERIFY(matches.total > 0);
        const auto definition =
            std::find_if(matches.symbols.cbegin(), matches.symbols.cend(),
                         [](const auto &entry) { return entry.symbol.definition; });
        QVERIFY(definition != matches.symbols.cend());
        qInfo() << "Linux exactLookupMs=" << elapsed.elapsed();
        auto *filter = window.findChild<QLineEdit *>("symbolFilter");
        filter->setText(lookup.text);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("symbolScope")->text() !=
                                     QString("%1 symbols").arg(summary.total),
                                 30000);
        auto *small = window.findChild<QTreeWidget *>("symbolList");
        QTRY_VERIFY_WITH_TIMEOUT(small->topLevelItemCount() > 0, 10000);
        QTreeWidgetItem *chosen = nullptr;
        for (int i = 0; i < small->topLevelItemCount(); ++i)
            if (small->topLevelItem(i)->text(0) == lookup.text &&
                small->topLevelItem(i)->data(0, Qt::UserRole + 1).toString() == definition->path) {
                chosen = small->topLevelItem(i);
                break;
            }
        QVERIFY(chosen);
        const auto path = chosen->data(0, Qt::UserRole + 1).toString();
        const auto content = readSourceFile(path);
        QVERIFY2(content.error.isEmpty(), qPrintable(content.error));
        small->scrollToItem(chosen);
        QTest::mouseClick(small->viewport(), Qt::LeftButton, Qt::NoModifier,
                          small->visualItemRect(chosen).center());
        QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), content.bytes, 30000);
        qInfo() << "Linux context preview verified for" << lookup.text;
        auto *editor = window.activeEditor();
        QVERIFY(editor);
        window.findChild<LanguageServerClient *>()->stop();
        const auto occurrence = content.bytes.indexOf("amdgpu_device_fini");
        QVERIFY(occurrence >= 0);
        editor->setFocus();
        editor->selectRange(occurrence, occurrence + qstrlen("amdgpu_device_fini"));
        QTRY_VERIFY_WITH_TIMEOUT(
            window.findChild<QLabel *>("contextTitle")->text().contains("Syntax-index candidate"),
            30000);
        QTRY_VERIFY_WITH_TIMEOUT(window.contextEditor()->bytes().contains("amdgpu_device_fini"),
                                 30000);
        qInfo() << "Linux source-caret fallback lookup verified without clangd";
    }

    void isolatedLinuxScreenshotReproduction() {
        const auto snapshot = qEnvironmentVariable("CODECAIRN_AUDIT_INDEX_SNAPSHOT");
        const auto root = qEnvironmentVariable("CODECAIRN_AUDIT_WORKSPACE_ROOT");
        if (snapshot.isEmpty() || root.isEmpty())
            QSKIP("Set an isolated Linux index snapshot and source root.");
        QTemporaryDir directory;
        const auto projectFile = directory.filePath("linux.cairn");
        WorkspaceOptions options;
        options.managed = true;
        options.name = "Linux screenshot";
        options.includedPaths = {"drivers/gpu"};
        QVERIFY(WorkspaceOptionsStore::saveProject(projectFile, root, options).isEmpty());
        QVERIFY(WorkspaceOptionsStore::initialize(root, projectFile).isEmpty());
        {
            ProjectIndexStore source(snapshot, false);
            ProjectIndexStore target(workspaceDatabase(root, projectFile), true);
            auto *backup = sqlite3_backup_init(target.handle(), "main", source.handle(), "main");
            QVERIFY(backup);
            const auto code = sqlite3_backup_step(backup, -1);
            QCOMPARE(sqlite3_backup_finish(backup), SQLITE_OK);
            QCOMPARE(code, SQLITE_DONE);
        }
        MainWindow window(false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openManagedProject(projectFile);
        auto *index = window.findChild<ProjectIndexer *>();
        index->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 30000);
        const auto path = QDir(root).filePath("drivers/gpu/drm/hyperv/hyperv_drm_modeset.c");
        const auto source = readSourceFile(path);
        QVERIFY2(source.error.isEmpty(), qPrintable(source.error));
        QSignalSpy analyzed(&window, &MainWindow::analysisReady);
        QVERIFY(window.openFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!analyzed.isEmpty(), 10000);
        auto *editor = window.activeEditor();
        const auto occurrence = source.bytes.indexOf("crtc_state = drm_atomic_get_new_crtc_state");
        QVERIFY(occurrence >= 0);
        editor->setFocus();
        editor->selectRange(occurrence, occurrence + qstrlen("crtc_state"));
        const auto declaration = source.bytes.indexOf("crtc_state = NULL");
        QVERIFY(declaration >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->send(SCI_GETSELECTIONSTART), declaration,
                                  10000);
        qInfo() << "selected crtc_state context:"
                << window.findChild<QLabel *>("contextTitle")->text()
                << "selection=" << window.contextEditor()->send(SCI_GETSELECTIONSTART)
                << window.contextEditor()->send(SCI_GETSELECTIONEND)
                << "firstLine=" << window.contextEditor()->send(SCI_GETFIRSTVISIBLELINE);
        const auto capture = qEnvironmentVariable("CODECAIRN_SYMBOL_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        const auto call = source.bytes.indexOf("drm_atomic_get_new_crtc_state(state, crtc)");
        QVERIFY(call >= 0);
        editor->selectRange(call, call + qstrlen("drm_atomic_get_new_crtc_state"));
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("contextTitle")
                                     ->text()
                                     .startsWith("Unresolved: drm_atomic_get_new_crtc_state"),
                                 10000);
        QVERIFY(window.contextEditor()->bytes().isEmpty());
        qInfo() << "selected external helper context:"
                << window.findChild<QLabel *>("contextTitle")->text()
                << "selection=" << window.contextEditor()->send(SCI_GETSELECTIONSTART)
                << window.contextEditor()->send(SCI_GETSELECTIONEND);

        auto *tabs = window.findChild<QTabWidget *>("browserTabs");
        tabs->setCurrentIndex(1);
        auto *filter = window.findChild<QLineEdit *>("symbolFilter");
        filter->setFocus();
        QTest::keyClicks(filter, "hv_drm");
        QElapsedTimer elapsed;
        elapsed.start();
        auto *scope = window.findChild<QLabel *>("symbolScope");
        QTRY_COMPARE_WITH_TIMEOUT(scope->text(), QString("76 symbols"), 10000);
        qInfo() << "hv_drm screenshot searchMs=" << elapsed.elapsed();
        QCOMPARE(filter->text(), QString("hv_drm"));
        auto *small = window.findChild<QTreeWidget *>("symbolList");
        QCOMPARE(small->topLevelItemCount(), 76);
        for (int i = 0; i < small->topLevelItemCount(); ++i)
            QVERIFY(small->topLevelItem(i)->text(0).contains("hv_drm", Qt::CaseInsensitive));
        QVERIFY(elapsed.elapsed() < 10000);
    }
};

QTEST_MAIN(SymbolScalingTests)
#include "SymbolScalingTests.moc"
