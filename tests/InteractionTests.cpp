#include "MainWindow.h"
#include "NavigationTree.h"
#include "TestFixtures.h"
#include "TestStorage.h"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QPushButton>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QMouseEvent>

namespace {
QTreeWidgetItem *symbolItem(QTreeWidget *tree, const QString &name) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == name)
            return *it;
    return nullptr;
}

void setReferenceMethod(QDialog *dialog, SearchMethod method) {
    auto *combo = dialog->findChild<QComboBox *>("referenceMethod");
    QVERIFY(combo);
    const int index = combo->findData(static_cast<int>(method));
    QVERIFY(index >= 0);
    combo->setCurrentIndex(index);
}

void clickRow(QTreeWidget *tree, const QString &text) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == text) {
            tree->scrollToItem(*it);
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                              tree->visualItemRect(*it).center());
            return;
        }
    QFAIL(qPrintable("Missing row: " + text));
}
QPoint sourcePoint(CodeEditor *editor, int position) {
    return {static_cast<int>(editor->send(SCI_POINTXFROMPOSITION, 0, position)) + 2,
            static_cast<int>(editor->send(SCI_POINTYFROMPOSITION, 0, position) +
                             editor->send(SCI_TEXTHEIGHT, 0) / 2)};
}
}

class InteractionTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void enterActivatesNavigationTree_data();
    void enterActivatesNavigationTree();
    void previewTabsPinAndPreserveDirtyDocuments();
    void outlineClickAndKeyboardNavigate();
    void sourceAndContextLinks();
    void linkGestureReleasesSelection_data();
    void linkGestureReleasesSelection();
    void modifierReleaseAndSelectionNormalization();
    void panelButtonsAndTransientZoom();
    void referenceShortcutAndResults();
    void referenceHistoryAndClosingResults();
    void referenceLinksOpenUnopenedFilesTrackEditsAndRejectStaleLinks();
    void panelConfigurationIsValidated();
    void editorSelectionTracksOutline();
    void panelFiltersAreTemporary();
    void confirmedPanelSelectionClearsFilter_data();
    void confirmedPanelSelectionClearsFilter();
    void escapeCancelsPendingSymbolActivation();
    void navigationBackClearsPanelFilters();
};

void InteractionTests::enterActivatesNavigationTree_data() {
    QTest::addColumn<int>("key");
    QTest::newRow("return") << int(Qt::Key_Return);
    QTest::newRow("enter") << int(Qt::Key_Enter);
}

void InteractionTests::enterActivatesNavigationTree() {
    QFETCH(int, key);
    NavigationTree tree;
    auto *item = new QTreeWidgetItem(&tree, {"entry"});
    tree.setCurrentItem(item);
    QSignalSpy activated(&tree, &QTreeWidget::itemActivated);
    QTest::keyClick(&tree, static_cast<Qt::Key>(key));
    QCOMPARE(activated.size(), 1);
    activated.clear();
    item->setDisabled(true);
    QTest::keyClick(&tree, static_cast<Qt::Key>(key));
    QVERIFY(activated.isEmpty());
}

void InteractionTests::previewTabsPinAndPreserveDirtyDocuments() {
    QTemporaryDir dir;
    const QByteArray first = "int first() { return 1; }\n", second = "int second() { return 2; }\n";
    QVERIFY(writeSourceFile(dir.filePath("first.cpp"), first, false).isEmpty());
    QVERIFY(writeSourceFile(dir.filePath("second.cpp"), second, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(openTypographyFixture(window));
    window.openProject(dir.path());
    auto *files = window.findChild<QTreeWidget *>("fileList");
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 2, 15000);
    clickRow(files, "first.cpp");
    QTRY_COMPARE(window.activeEditor()->bytes(), first);
    QCOMPARE(window.documentCount(), 2);
    clickRow(files, "second.cpp");
    QTRY_COMPARE(window.activeEditor()->bytes(), second);
    QCOMPARE(window.documentCount(), 2);
    auto *dirty = window.activeEditor();
    dirty->setFocus();
    QTest::keyClicks(dirty, "// edit ");
    QVERIFY(dirty->isModified());
    clickRow(files, "first.cpp");
    QTRY_COMPARE(window.activeEditor()->bytes(), first);
    QCOMPARE(window.documentCount(), 3);
    QVERIFY(dirty->isModified());
    files->setFocus();
    QTest::keyClick(files, Qt::Key_Return);
    QTRY_VERIFY(window.activeEditor()->hasFocus());
    QCOMPARE(window.activeEditor()->bytes(), first);
    QTest::keyClick(window.activeEditor(), Qt::Key_Left, Qt::AltModifier);
    QTRY_COMPARE(window.activeEditor(), dirty);
    QTest::keyClick(window.activeEditor(), Qt::Key_Right, Qt::AltModifier);
    QTRY_COMPARE(window.activeEditor()->bytes(), first);
    QCOMPARE(readSourceFile(dir.filePath("second.cpp")).bytes, second);
}

void InteractionTests::outlineClickAndKeyboardNavigate() {
    QTemporaryDir dir;
    const QByteArray source = "int alpha() { return 1; }\nint beta() { return 2; }\n";
    QVERIFY(writeSourceFile(dir.filePath("outline.cpp"), source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(dir.filePath("outline.cpp")));
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QTRY_COMPARE(outline->topLevelItemCount(), 2);
    clickRow(outline, "alpha");
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), source.indexOf("alpha"));
    outline->setFocus();
    QTest::keyClick(outline, Qt::Key_Down);
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), source.indexOf("beta"));
    QVERIFY(outline->hasFocus());
    window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 2, 15000);
    clickRow(symbols, "alpha");
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), source.indexOf("alpha"));
    symbols->setFocus();
    QTest::keyClick(symbols, Qt::Key_Down);
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), source.indexOf("beta"));
    QVERIFY(symbols->hasFocus());
    outline->setCurrentItem(outline->topLevelItem(0));
    QTest::qWait(20);
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), source.indexOf("beta"));
}

void InteractionTests::sourceAndContextLinks() {
    QTemporaryDir dir;
    const QByteArray source = "def callee():\n    return 1\n\ndef caller():\n    return callee()\n";
    QVERIFY(writeSourceFile(dir.filePath("links.py"), source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(dir.filePath("links.py")));
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QTRY_COMPARE(outline->topLevelItemCount(), 2);
    auto *editor = window.activeEditor();
    const int use = static_cast<int>(source.lastIndexOf("callee"));
    editor->selectRange(use, use + 6);
    QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), source, 15000);
    QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->send(SCI_GETSELECTIONSTART),
                              source.indexOf("callee"), 15000);
    const auto point = sourcePoint(editor, use + 1);
    QTest::mouseMove(editor->viewport(), point);
    QTest::keyPress(editor, Qt::Key_Control);
    QTRY_COMPARE_WITH_TIMEOUT(editor->viewport()->cursor().shape(), Qt::PointingHandCursor, 15000);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::ControlModifier, point);
    QTest::keyRelease(editor, Qt::Key_Control);
    QTRY_COMPARE_WITH_TIMEOUT(editor->send(SCI_GETSELECTIONSTART), source.indexOf("callee"), 10000);
    auto *context = window.contextEditor();
    QSignalSpy contextHover(context, &CodeEditor::linkHovered);
    const auto contextPoint = sourcePoint(context, static_cast<int>(source.indexOf("callee")) + 1);
    QTest::mouseMove(context->viewport(), contextPoint);
    QTRY_VERIFY2_WITH_TIMEOUT(
        context->viewport()->cursor().shape() == Qt::PointingHandCursor,
        qPrintable(QString("Context hover events: %1; last byte: %2; under mouse: %3")
                       .arg(contextHover.count())
                       .arg(contextHover.isEmpty() ? -2 : contextHover.last()[0].toInt())
                       .arg(context->viewport()->underMouse())),
        10000);
    QTest::mouseClick(context->viewport(), Qt::LeftButton, Qt::NoModifier, contextPoint);
    QTRY_VERIFY(editor->hasFocus());
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), source.indexOf("callee"));
}

void InteractionTests::linkGestureReleasesSelection_data() {
    QTest::addColumn<bool>("control");
    QTest::addColumn<bool>("drag");
    QTest::newRow("context-click") << false << false;
    QTest::newRow("context-drag") << false << true;
    QTest::newRow("control-click") << true << false;
    QTest::newRow("control-drag") << true << true;
}

void InteractionTests::linkGestureReleasesSelection() {
    QFETCH(bool, control);
    QFETCH(bool, drag);
    QWidget window;
    QHBoxLayout layout(&window);
    CodeEditor context, source;
    layout.addWidget(&context);
    layout.addWidget(&source);
    window.resize(800, 300);
    context.loadBytes("int callee();\nint second;\nint third;\n");
    context.setReadOnly(true);
    context.setLinkBrowsing(true);
    window.show();
    window.activateWindow();
    context.setFocus();
    QTRY_VERIFY(context.hasFocus());
    QSignalSpy activated(&context, &CodeEditor::definitionActivated);
    int releases = 0;
    connect(&context, &ScintillaEditBase::buttonReleased, &context, [&releases] { ++releases; });
    connect(&context, &CodeEditor::definitionActivated, &source, [&source] { source.setFocus(); });
    connect(&context, &CodeEditor::linkHovered, &context,
            [&context](int position) { context.setLinkAvailable(position, position >= 0); });
    const auto pressPoint = sourcePoint(&context, 5);
    QMouseEvent hover(QEvent::MouseMove, pressPoint, context.viewport()->mapToGlobal(pressPoint),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(context.viewport(), &hover);
    const auto modifiers = control ? Qt::ControlModifier : Qt::NoModifier;
    QTest::mousePress(context.viewport(), Qt::LeftButton, modifiers, pressPoint);
    const auto releasePoint = drag ? sourcePoint(&context, 24) : pressPoint;
    // Release Control before the mouse to check that press ownership is remembered.
    QTest::mouseRelease(context.viewport(), Qt::LeftButton, Qt::NoModifier, releasePoint);
    QCOMPARE(activated.count(), drag ? 0 : 1);
    if (!drag)
        QVERIFY(source.hasFocus());
    if (drag && !control) {
        QCOMPARE(context.send(SCI_GETSELECTIONSTART), 5);
        QCOMPARE(context.send(SCI_GETSELECTIONEND), 24);
    }
    const auto anchor = context.send(SCI_GETANCHOR);
    const auto caret = context.send(SCI_GETCURRENTPOS);
    source.setFocus();
    const auto movePoint = sourcePoint(&context, 33);
    QMouseEvent move(QEvent::MouseMove, movePoint, context.viewport()->mapToGlobal(movePoint),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(context.viewport(), &move);
    QCOMPARE(context.send(SCI_GETANCHOR), anchor);
    QCOMPARE(context.send(SCI_GETCURRENTPOS), caret);
    QCOMPARE(releases, control ? 0 : 1);
}

void InteractionTests::modifierReleaseAndSelectionNormalization() {
    CodeEditor editor;
    editor.resize(600, 300);
    editor.show();
    editor.loadBytes("int callee();\n");
    editor.selectRange(10, 4);
    QCOMPARE(editor.lookupPosition(), 4);
    editor.selectRange(4, 10);
    QCOMPARE(editor.lookupPosition(), 4);
    QSignalSpy hover(&editor, &CodeEditor::linkHovered);
    QSignalSpy activated(&editor, &CodeEditor::definitionActivated);
    const auto point = sourcePoint(&editor, 6);
    QTest::mouseMove(editor.viewport(), point);
    QTest::keyPress(&editor, Qt::Key_Control);
    QTRY_VERIFY(!hover.isEmpty());
    const int position = hover.last()[0].toInt();
    QVERIFY(position >= 4 && position < 10);
    editor.setLinkAvailable(position, true);
    QCOMPARE(editor.viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mousePress(editor.viewport(), Qt::LeftButton, Qt::ControlModifier, point);
    QTest::keyRelease(&editor, Qt::Key_Control);
    QCOMPARE(editor.viewport()->cursor().shape(), Qt::IBeamCursor);
    editor.setLinkAvailable(position, true);
    QCOMPARE(editor.viewport()->cursor().shape(), Qt::IBeamCursor);
    QTest::mouseRelease(editor.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(activated.count(), 1);
}

void InteractionTests::panelButtonsAndTransientZoom() {
    MainWindow window(false);
    window.show();
    QVERIFY(openTypographyFixture(window));
    for (const auto &panel : {"outline", "files", "symbols", "context"}) {
        if (QString(panel) == "files" || QString(panel) == "symbols")
            window.findChild<QTabWidget *>("browserTabs")
                ->setCurrentIndex(QString(panel) == "files" ? 0 : 1);
        QToolButton *button = nullptr;
        for (auto *candidate : window.findChildren<QToolButton *>())
            if (candidate->defaultAction() &&
                candidate->defaultAction()->objectName() == QString(panel) + "Options")
                button = candidate;
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
        auto *dialog = window.findChild<QDialog *>(QString(panel) + "OptionsDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->isVisible());
        QVERIFY(dialog->windowTitle() != "Source Appearance");
        QTest::keyClick(dialog, Qt::Key_Escape);
    }
    auto *editor = window.activeEditor();
    editor->setFocus();
    const auto configuredSize = window.findChild<QDoubleSpinBox *>("fontSize")->value();
    window.findChild<QAction *>("zoomIn")->trigger();
    QCOMPARE(editor->send(SCI_GETZOOM), 1);
    QCOMPARE(window.findChild<QDoubleSpinBox *>("fontSize")->value(), configuredSize);
    QTest::keyClick(editor, Qt::Key_0, Qt::ControlModifier);
    QTRY_COMPARE(editor->send(SCI_GETZOOM), 0);
    for (const auto *action : window.findChild<QToolBar *>("mainToolbar")->actions())
        QVERIFY(action->objectName() != "zoomIn" && action->objectName() != "zoomOut");
}

void InteractionTests::referenceShortcutAndResults() {
    QTemporaryDir dir;
    const QByteArray source = "int callee() { return 1; }\nint caller() { return callee(); }\n";
    QVERIFY(writeSourceFile(dir.filePath("references.cpp"), source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(dir.filePath("references.cpp")));
    QTRY_COMPARE(window.findChild<QTreeWidget *>("outline")->topLevelItemCount(), 2);
    window.activeEditor()->selectRange(4, 10);
    window.activeEditor()->setFocus();
    QTest::keyClick(window.activeEditor(), Qt::Key_Slash, Qt::ControlModifier);
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    auto *method = dialog->findChild<QComboBox *>("referenceMethod");
    method->setFocus();
    QTest::keyClick(method, Qt::Key_Down);
    QCOMPARE(method->currentData().toInt(), static_cast<int>(SearchMethod::Occurrences));
    QTest::mouseClick(dialog->findChild<QPushButton *>("runReferenceLookup"), Qt::LeftButton);
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(results);
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QCOMPARE(results->topLevelItem(0)->childCount(), 2);
    auto *tabs = window.findChild<QTabWidget *>("sourceTabs");
    auto *page = window.findChild<QWidget *>("searchResultsPage");
    QCOMPARE(tabs->currentWidget(), page);
    QTest::qWait(50);
    QVERIFY(window.grab().save(
        QDir(QCoreApplication::applicationDirPath()).filePath("reference-results.png")));
    QCOMPARE(window.documentCount(), 1);
    auto *call = results->topLevelItem(0)->child(1);
    QVERIFY(call->text(0).startsWith("caller in references.cpp line 2 : "));
    QCOMPARE(call->text(1), "references.cpp");
    QCOMPARE(call->text(2), "2");
    QVERIFY(call->text(3).contains("return callee()"));
    results->setCurrentItem(call);
    QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), source, 10000);
    QCOMPARE(tabs->currentWidget(), page);
    results->itemActivated(call, 0);
    QTRY_VERIFY(window.activeEditor());
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), source.indexOf("int caller"));
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONEND), source.size() - 1);
    QVERIFY(tabs->indexOf(page) >= 0);
    window.findChild<QAction *>("jumpReference")->trigger();
    QCOMPARE(tabs->currentWidget(), page);
    window.findChild<QAction *>("jumpReference")->trigger();
    QTRY_VERIFY(window.activeEditor());
    QCOMPARE(readSourceFile(dir.filePath("references.cpp")).bytes, source);
}

void InteractionTests::referenceHistoryAndClosingResults() {
    QTemporaryDir dir;
    const QByteArray source = "int callee() { return 1; }\nint caller() { return callee(); }\n";
    QVERIFY(writeSourceFile(dir.filePath("references.cpp"), source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(dir.filePath("references.cpp")));
    auto *editor = window.activeEditor();
    for (const auto &word : {QByteArray("callee"), QByteArray("caller")}) {
        editor->selectRange(static_cast<int>(source.indexOf(word)),
                            static_cast<int>(source.indexOf(word) + word.size()));
        editor->setFocus();
        window.findChild<QAction *>("lookupReferences")->trigger();
        auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
        QVERIFY(dialog);
        setReferenceMethod(dialog, SearchMethod::Occurrences);
        QTest::mouseClick(dialog->findChild<QPushButton *>("runReferenceLookup"), Qt::LeftButton);
        auto *results = window.findChild<QTreeWidget *>("referenceResults");
        const int groups = word == "callee" ? 1 : 2;
        QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), groups, 10000);
        QCOMPARE(results->topLevelItem(0)->childCount(), 2);
        if (groups == 1) {
            results->itemActivated(results->topLevelItem(0)->child(0), 0);
            QTRY_COMPARE(window.activeEditor(), editor);
        }
    }
    window.findChild<QAction *>("firstReference")->trigger();
    QTRY_COMPARE(window.activeEditor(), editor);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), 0);
    window.findChild<QAction *>("nextReference")->trigger();
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), source.indexOf("int caller"));
    editor->selectRange(4, 10);
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(dialog);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    window.findChild<QAction *>("cancelReferenceSearch")->trigger();
    QTest::qWait(150);
    QCOMPARE(window.findChild<QTreeWidget *>("referenceResults")->topLevelItemCount(), 2);
    QVERIFY(window.findChild<QLabel *>("referenceResultsStatus")->text().contains("cancelled"));
    auto *tabs = window.findChild<QTabWidget *>("sourceTabs");
    tabs->setCurrentWidget(window.findChild<QWidget *>("searchResultsPage"));
    window.findChild<QAction *>("closeFile")->trigger();
    QVERIFY(!window.findChild<QWidget *>("searchResultsPage"));
    QCOMPARE(window.documentCount(), 1);
    QCoreApplication::processEvents();
    QVERIFY(!window.findChild<QWidget *>("searchResultsPage"));
}

void InteractionTests::panelConfigurationIsValidated() {
    const auto parsed = parseSettings(R"({
        "panels":{"outline":{"fontSizePx":18,"foreground":"#abcdef"}},
        "outline":{"locals":true,"members":false},
        "context":{"tracking":"enclosing","scale":1.25},
        "references":{"semantic":false,"scope":1,"comments":true}
    })");
    QVERIFY2(parsed.settings.has_value(), qPrintable(parsed.error));
    const auto json = settingsToJson(*parsed.settings);
    const auto roundTrip = parseSettings(QJsonDocument(json).toJson());
    QVERIFY(roundTrip.settings.has_value());
    QCOMPARE(settingsToJson(*roundTrip.settings), json);
    QVERIFY(!parseSettings(R"({"context":{"tracking":"unknown"}})").settings);
    QVERIFY(!parseSettings(R"({"outline":{"locals":"yes"}})").settings);
    QVERIFY(!parseSettings(R"({"references":{"scope":3}})").settings);
    QTemporaryDir dir;
    const auto path = dir.filePath("settings.json");
    QVERIFY(writeSourceFile(path, QJsonDocument(json).toJson(), false).isEmpty());
    MainWindow window(false, nullptr, path);
    window.show();
    QVERIFY(openTypographyFixture(window));
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QTRY_COMPARE(outline->font().pixelSize(), 18);
    QVERIFY(!window.findChild<QAction *>("cut")->isEnabled());
}

void InteractionTests::referenceLinksOpenUnopenedFilesTrackEditsAndRejectStaleLinks() {
    QTemporaryDir dir;
    const auto api = dir.filePath("api.cpp"), implementation = dir.filePath("implementation.cpp");
    const QByteArray source = "int callee();\nint caller() { return callee(); }\n";
    QVERIFY(writeSourceFile(api, "int callee();\n", false).isEmpty());
    QVERIFY(writeSourceFile(implementation, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QSignalSpy indexed(&window, &MainWindow::indexReady);
    QVERIFY(window.openFile(api));
    QTRY_VERIFY_WITH_TIMEOUT(!indexed.isEmpty(), 10000);
    window.activeEditor()->selectRange(4, 10);
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(dialog);
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QCOMPARE(window.documentCount(), 1);
    QTreeWidgetItem *call = nullptr;
    for (int i = 0; i < results->topLevelItem(0)->childCount(); ++i) {
        auto *item = results->topLevelItem(0)->child(i);
        if (item->text(0).startsWith("caller in "))
            call = item;
    }
    QVERIFY(call);
    results->setCurrentItem(call);
    results->itemActivated(call, 0);
    QTRY_COMPARE_WITH_TIMEOUT(window.documentCount(), 2, 10000);
    auto *editor = window.activeEditor();
    QCOMPARE(editor->bytes(), source);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), source.indexOf("int caller"));
    editor->sends(SCI_INSERTTEXT, 0, "// changed\n");
    results->itemActivated(call, 0);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART),
             source.indexOf("int caller") + QByteArray("// changed\n").size());
    const auto position = source.lastIndexOf("callee") + QByteArray("// changed\n").size();
    QCOMPARE(call->text(2), "3");
    editor->send(SCI_DELETERANGE, position, 1);
    QSignalSpy messages(window.statusBar(), &QStatusBar::messageChanged);
    results->itemActivated(call, 0);
    bool rejected = false;
    for (const auto &message : messages)
        rejected = rejected || message[0].toString().contains("Source link is stale");
    QVERIFY(rejected);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART),
             source.indexOf("int caller") + QByteArray("// changed\n").size());
    QCOMPARE(readSourceFile(implementation).bytes, source);
}

void InteractionTests::editorSelectionTracksOutline() {
    QTemporaryDir dir;
    const auto path = dir.filePath("follow.cpp");
    const QByteArray source = "int alpha() {\n"
                              "    int local = 1;\n"
                              "    // inside alpha\n"
                              "    return local;\n"
                              "}\n\n"
                              "struct Holder {\n"
                              "    int field;\n"
                              "    int beta() {\n"
                              "        return field;\n"
                              "    }\n"
                              "};\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    window.activateWindow();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QTRY_VERIFY(symbolItem(outline, "beta"));
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      sourcePoint(editor, source.indexOf("return local") + 3));
    QTRY_VERIFY(outline->currentItem() && outline->currentItem()->text(0) == "alpha");
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      sourcePoint(editor, source.indexOf("inside alpha") + 2));
    QTRY_VERIFY(outline->currentItem() && outline->currentItem()->text(0) == "alpha");
    auto *holder = symbolItem(outline, "Holder");
    QVERIFY(holder);
    holder->setExpanded(false);
    const auto betaPoint = sourcePoint(editor, source.indexOf("return field") + 3);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, betaPoint);
    const auto position = editor->caretPosition();
    QTRY_VERIFY(outline->currentItem() && outline->currentItem()->text(0) == "beta");
    QVERIFY(holder->isExpanded());
    QCOMPARE(editor->caretPosition(), position);
    QVERIFY(editor->hasFocus());
    QVERIFY(!window.findChild<QAction *>("historyBack")->isEnabled());
    outline->setCurrentItem(symbolItem(outline, "alpha"));
    QCOMPARE(editor->caretPosition(), position);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, betaPoint);
    QTRY_VERIFY(outline->currentItem() && outline->currentItem()->text(0) == "beta");
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      sourcePoint(editor, source.indexOf("\n\n") + 1));
    QTRY_VERIFY(!outline->currentItem());
    editor->selectRange(source.indexOf("alpha"), source.indexOf("alpha") + 5);
    QTRY_VERIFY(outline->currentItem() && outline->currentItem()->text(0) == "alpha");
    QCOMPARE(editor->bytes(), source);
    QVERIFY(!editor->isModified());
    QSignalSpy analyzed(&window, &MainWindow::analysisReady);
    const QByteArray prefix = "// prefix\n";
    editor->sends(SCI_INSERTTEXT, 0, prefix.constData());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      sourcePoint(editor, prefix.size() + source.indexOf("\n\n") + 1));
    QTRY_VERIFY_WITH_TIMEOUT(!analyzed.isEmpty(), 10000);
    QTRY_VERIFY(!outline->currentItem());
    QCOMPARE(editor->bytes(), prefix + source);
    QCOMPARE(readSourceFile(path).bytes, source);
}

void InteractionTests::panelFiltersAreTemporary() {
    QTemporaryDir dir;
    const auto path = dir.filePath("alpha.cpp");
    const QByteArray source = "int alpha() { return 1; }\nint beta() { return 2; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    QVERIFY(
        writeSourceFile(dir.filePath("beta.cpp"), "int gamma() { return 3; }\n", false).isEmpty());
    MainWindow window(false);
    window.show();
    window.activateWindow();
    QVERIFY(window.openFile(path));
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QTRY_COMPARE(outline->topLevelItemCount(), 2);
    auto *files = window.findChild<QTreeWidget *>("fileList");
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 2, 10000);
    struct Panel {
        const char *filter;
        const char *tree;
        int tab;
    };
    const Panel panels[]{{"outlineFilter", "outline", -1},
                         {"fileFilter", "fileList", 0},
                         {"symbolFilter", "symbolList", 1}};
    for (const auto &panel : panels) {
        if (panel.tab >= 0)
            window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(panel.tab);
        auto *filter = window.findChild<QLineEdit *>(panel.filter);
        auto *tree = window.findChild<QTreeWidget *>(panel.tree);
        QVERIFY(filter && tree);
        filter->setFocus();
        QTRY_VERIFY(filter->hasFocus());
        QTest::keyClicks(filter, "alp");
        QCOMPARE(filter->text(), "alp");
        tree->setFocus();
        QTRY_VERIFY(tree->hasFocus());
        QCOMPARE(filter->text(), "alp");
        QTest::keyClicks(tree, "ha");
        QCOMPARE(filter->text(), "alpha");
        QVERIFY(filter->hasFocus());
        QTest::keyClick(filter, Qt::Key_Escape);
        QCOMPARE(filter->text(), QString{});
        QTest::keyClicks(filter, "alpha");
        tree->setFocus();
        QCOMPARE(filter->text(), "alpha");
        QTest::keyClick(tree, Qt::Key_Escape);
        QCOMPARE(filter->text(), QString{});
        filter->setFocus();
        QTest::keyClicks(filter, "alpha");
        window.activeEditor()->setFocus();
        QTRY_COMPARE(filter->text(), QString{});
        if (panel.tab == 1)
            QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 3, 10000);
    }
    QCOMPARE(readSourceFile(path).bytes, source);
}

void InteractionTests::confirmedPanelSelectionClearsFilter_data() {
    QTest::addColumn<QString>("filterName");
    QTest::addColumn<QString>("treeName");
    QTest::addColumn<QString>("target");
    QTest::addColumn<int>("tab");
    QTest::addColumn<bool>("doubleClick");
    for (const bool mouse : {false, true}) {
        const auto suffix = mouse ? "-double-click" : "-enter";
        QTest::newRow(qPrintable("outline" + QString(suffix)))
            << QString("outlineFilter") << QString("outline") << QString("beta") << -1 << mouse;
        QTest::newRow(qPrintable("files" + QString(suffix)))
            << QString("fileFilter") << QString("fileList") << QString("beta.cpp") << 0 << mouse;
        QTest::newRow(qPrintable("symbols" + QString(suffix)))
            << QString("symbolFilter") << QString("symbolList") << QString("beta") << 1 << mouse;
    }
}

void InteractionTests::confirmedPanelSelectionClearsFilter() {
    QFETCH(QString, filterName);
    QFETCH(QString, treeName);
    QFETCH(QString, target);
    QFETCH(int, tab);
    QFETCH(bool, doubleClick);
    QTemporaryDir dir;
    const auto path = dir.filePath("alpha.cpp");
    const QByteArray first = "int alpha() { return 1; }\nint beta() { return 2; }\n";
    const QByteArray second = "int gamma() { return 3; }\n";
    QVERIFY(writeSourceFile(path, first, false).isEmpty());
    QVERIFY(writeSourceFile(dir.filePath("beta.cpp"), second, false).isEmpty());
    MainWindow window(false);
    window.show();
    window.activateWindow();
    QVERIFY(window.openFile(path));
    QTRY_COMPARE(window.findChild<QTreeWidget *>("outline")->topLevelItemCount(), 2);
    if (tab >= 0)
        window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(tab);
    auto *filter = window.findChild<QLineEdit *>(filterName);
    auto *tree = window.findChild<QTreeWidget *>(treeName);
    QVERIFY(filter && tree);
    if (tab == 0)
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 2, 10000);
    if (tab == 1)
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 3, 10000);
    filter->setFocus();
    QTRY_VERIFY(filter->hasFocus());
    QTest::keyClicks(filter, "beta");
    QTRY_VERIFY_WITH_TIMEOUT(symbolItem(tree, target) && !symbolItem(tree, target)->isHidden(),
                             10000);
    if (doubleClick) {
        clickRow(tree, target);
        QCOMPARE(filter->text(), "beta");
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                           tree->visualItemRect(symbolItem(tree, target)).center());
    } else
        QTest::keyClick(filter, Qt::Key_Return);
    QTRY_COMPARE(filter->text(), QString{});
    QTRY_VERIFY(window.activeEditor() && window.activeEditor()->hasFocus());
    QCOMPARE(window.activeEditor()->bytes(), tab == 0 ? second : first);
    if (tab != 0)
        QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), first.indexOf("beta"));
    if (tab == 1) {
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 3, 10000);
        QVERIFY(tree->currentItem());
        QCOMPARE(tree->currentItem()->text(0), "beta");
    }
}

void InteractionTests::escapeCancelsPendingSymbolActivation() {
    QTemporaryDir dir;
    const auto path = dir.filePath("symbols.cpp");
    const QByteArray source = "int alpha() { return 1; }\nint beta() { return 2; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    window.activateWindow();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 9);
    window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 2, 10000);
    auto *filter = window.findChild<QLineEdit *>("symbolFilter");
    filter->setFocus();
    QTRY_VERIFY(filter->hasFocus());
    auto *index = window.findChild<WorkspaceIndex *>();
    QSignalSpy updated(index, &WorkspaceIndex::symbolsReady);
    filter->setText("beta");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QCoreApplication::sendEvent(filter, &enter);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(filter, &escape);
    QCOMPARE(filter->text(), QString{});
    QTRY_VERIFY_WITH_TIMEOUT(!updated.isEmpty(), 10000);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), 4);
    QCOMPARE(editor->bytes(), source);
    QVERIFY(filter->hasFocus());
}

void InteractionTests::navigationBackClearsPanelFilters() {
    QTemporaryDir dir;
    const auto path = dir.filePath("file.cpp");
    const QByteArray source = "int alpha() { return 1; }\nint beta() { return 2; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    window.activateWindow();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QTRY_COMPARE(outline->topLevelItemCount(), 2);
    editor->selectRange(0, 0);
    clickRow(outline, "beta");
    QVERIFY(window.findChild<QAction *>("historyBack")->isEnabled());
    struct Panel {
        const char *name;
        const char *text;
        int tab;
    };
    const Panel panels[]{
        {"outlineFilter", "beta", -1}, {"fileFilter", "file", 0}, {"symbolFilter", "beta", 1}};
    for (const auto &panel : panels) {
        if (panel.tab >= 0)
            window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(panel.tab);
        auto *filter = window.findChild<QLineEdit *>(panel.name);
        filter->setFocus();
        QTRY_VERIFY(filter->hasFocus());
        QTest::keyClicks(filter, panel.text);
        QTest::keyClick(filter, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(filter->text(), QString{});
        QTRY_VERIFY(editor->hasFocus());
        QCOMPARE(editor->send(SCI_GETSELECTIONSTART), 0);
        window.findChild<QAction *>("historyForward")->trigger();
        QCOMPARE(editor->send(SCI_GETSELECTIONSTART), source.indexOf("beta"));
    }
    QCOMPARE(editor->bytes(), source);
}

QTEST_MAIN(InteractionTests)
#include "InteractionTests.moc"
