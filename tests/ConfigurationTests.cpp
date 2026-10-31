#include "AppSettings.h"
#include "CodeEditor.h"
#include "MainWindow.h"
#include "UiAppearance.h"
#include "TestFixtures.h"
#include "TestStorage.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFontComboBox>
#include <QFontMetricsF>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLockFile>
#include <QScreen>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

namespace {
void writeJson(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
}

class ConfigurationTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void referenceDefaultsAndRoundTrip();
    void sourceFontUsesWindowsScale_data();
    void sourceFontUsesWindowsScale();
    void nativeSourcePointSize_data();
    void nativeSourcePointSize();
    void screenChangePreservesEditorState_data();
    void screenChangePreservesEditorState();
    void documentTabsStartAtLeft();
    void partialOverrides();
    void workspaceRefreshConfiguration();
    void invalidConfiguration_data();
    void invalidConfiguration();
    void missingFileAndAtomicSave();
    void invalidFilesAreNotOverwritten();
    void externalChangesAreNotOverwritten();
    void concurrentWriterIsReported();
    void reloadUpdatesBothViewsWithoutEditing();
    void failedReloadPreservesCurrentAppearance();
    void uiSettingsPersistInJson();
    void uniformSizesOverrideSyntaxScaling();
};

void ConfigurationTests::referenceDefaultsAndRoundTrip() {
    const auto &defaults = defaultAppSettings();
    QCOMPARE(defaults.ui.fontSizePx, 11);
    QCOMPARE(defaults.behavior.workspaceRefreshMinutes, 360);
    QCOMPARE(defaults.editor.typography.pointSize, 8.25);
    QCOMPARE(defaults.color("source.background"), QColor("#202020"));
    QVERIFY(!defaults.editor.lineNumbers);
    QVERIFY(!UiAppearance::styleSheet(defaults).contains('@'));
    QVERIFY(defaults.syntax[static_cast<size_t>(TextStyle::Function)].bold);
    const auto &parameter = defaults.syntax[static_cast<size_t>(TextStyle::Parameter)];
    QCOMPARE(parameter.foreground, QColor("#d09030"));
    QVERIFY(parameter.bold);
    const auto &variable = defaults.syntax[static_cast<size_t>(TextStyle::Variable)];
    QCOMPARE(variable.foreground, QColor("#00a6df"));
    QVERIFY(!variable.bold);
    QVERIFY(defaults.syntax[static_cast<size_t>(TextStyle::VariableDeclaration)].bold);
    QCOMPARE(defaults.syntax[static_cast<size_t>(TextStyle::Constant)].foreground,
             QColor("#e04040"));
    QCOMPARE(defaults.syntax[static_cast<size_t>(TextStyle::Number)].foreground, QColor("#e04040"));
    QVERIFY(defaults.syntax[static_cast<size_t>(TextStyle::Field)].italic);
    QCOMPARE(defaults.syntax[static_cast<size_t>(TextStyle::Punctuation)].foreground,
             QColor("#d4cc72"));
    const auto encoded = settingsToJson(defaults);
    const auto decoded = parseSettings(QJsonDocument(encoded).toJson());
    QVERIFY2(decoded.settings.has_value(), qPrintable(decoded.error));
    QCOMPARE(settingsToJson(*decoded.settings), encoded);
}

void ConfigurationTests::sourceFontUsesWindowsScale_data() {
    QTest::addColumn<QByteArray>("profile");
    QTest::addColumn<double>("sourcePixels");
    QTest::addColumn<double>("contextScale");
    QTest::newRow("fresh-default") << QByteArray("{}") << 11.0 << 1.0;
    QTest::newRow("saved-default")
        << QByteArray(R"({"editor":{"fontSizePt":8.25}})") << 11.0 << 1.0;
    QTest::newRow("custom-size") << QByteArray(R"({"editor":{"fontSizePt":12}})") << 16.0 << 1.0;
    QTest::newRow("context-scale")
        << QByteArray(R"({"editor":{"fontSizePt":8.25},"context":{"scale":1.5}})") << 11.0 << 1.5;
}

void ConfigurationTests::sourceFontUsesWindowsScale() {
    QFETCH(QByteArray, profile);
    QFETCH(double, sourcePixels);
    QFETCH(double, contextScale);
    QTemporaryDir dir;
    const auto path = dir.filePath("ui.json");
    writeJson(path, profile);
    MainWindow window(false, nullptr, path);
    window.show();
    QVERIFY(openTypographyFixture(window));
    for (auto *view : {window.activeEditor(), window.contextEditor()}) {
        const auto pixels = sourcePixels * (view == window.contextEditor() ? contextScale : 1.0);
        for (const int style : {STYLE_DEFAULT, STYLE_LINENUMBER, static_cast<int>(TextStyle::Plain),
                                static_cast<int>(TextStyle::Function)}) {
            auto font = QFont(UiAppearance::sourceFont());
            const auto scale = style == static_cast<int>(TextStyle::Function) ? 1.2 : 1.0;
            font.setPointSizeF(pixels * scale * 72.0 / view->logicalDpiY());
            font.setBold(style == static_cast<int>(TextStyle::Function));
            const auto expectedWidth = QFontMetricsF(font, view).horizontalAdvance("MMMMMMMMMM");
            const auto width = view->sends(SCI_TEXTWIDTH, style, "MMMMMMMMMM");
            QVERIFY2(qAbs(width - expectedWidth) <= 1.0,
                     qPrintable(QString("Style %1 width %2, expected %3 at %4 logical px, DPI %5")
                                    .arg(style)
                                    .arg(width)
                                    .arg(expectedWidth)
                                    .arg(pixels * scale)
                                    .arg(view->logicalDpiY())));
        }
    }
    QCOMPARE(read(path), profile);
}

void ConfigurationTests::documentTabsStartAtLeft() {
    QTemporaryDir dir;
    const auto path = dir.filePath("ui.json");
    writeJson(path, "{}");
    MainWindow window(false, nullptr, path);
    window.show();
    QVERIFY(openTypographyFixture(window));
    auto *bar = window.findChild<QTabBar *>("documentBar");
    QVERIFY(bar);
    QVERIFY(bar->count() > 0);
    const auto firstTabLeft = [bar] {
        return bar->mapToParent(bar->tabRect(0).topLeft()).x();
    };
    QTRY_COMPARE(firstTabLeft(), 0);
    window.resize(window.width() + 200, window.height());
    QTRY_COMPARE(firstTabLeft(), 0);
    QVERIFY(window.reloadConfiguration());
    QTRY_COMPARE(firstTabLeft(), 0);
}

void ConfigurationTests::nativeSourcePointSize_data() {
    QTest::addColumn<int>("logicalDpi");
    QTest::addColumn<double>("referencePoints");
    QTest::addColumn<double>("nativePoints");
    QTest::newRow("72-dpi") << 72 << 8.25 << 11.0;
    QTest::newRow("96-dpi") << 96 << 8.25 << 8.25;
    QTest::newRow("120-dpi") << 120 << 8.25 << 6.6;
    QTest::newRow("144-dpi") << 144 << 8.25 << 5.5;
    QTest::newRow("192-dpi") << 192 << 8.25 << 4.125;
    QTest::newRow("custom-size") << 72 << 12.0 << 16.0;
}

void ConfigurationTests::nativeSourcePointSize() {
    QFETCH(int, logicalDpi);
    QFETCH(double, referencePoints);
    QFETCH(double, nativePoints);
    QImage device(1, 1, QImage::Format_RGB32);
    device.setDotsPerMeterY(qRound(logicalDpi / 0.0254));
    QCOMPARE(device.logicalDpiY(), logicalDpi);
    QCOMPARE(UiAppearance::nativeSourcePointSize(referencePoints, device), nativePoints);
}

void ConfigurationTests::screenChangePreservesEditorState_data() {
    QTest::addColumn<bool>("dpiSignal");
    QTest::newRow("screen-change") << false;
    QTest::newRow("current-screen-dpi-change") << true;
}

void ConfigurationTests::screenChangePreservesEditorState() {
    QFETCH(bool, dpiSignal);
    CodeEditor editor;
    editor.show();
    editor.loadBytes("int value;\n");
    editor.sends(SCI_APPENDTEXT, 6, "// new");
    editor.selectRange(4, 9);
    editor.showLineNumbers(true);
    editor.changeZoom(2);
    const auto contents = editor.bytes();
    const auto width = editor.sends(SCI_TEXTWIDTH, STYLE_DEFAULT, "MMMMMMMMMM");
    const auto pointSize = editor.send(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT);
    CodeEditor context;
    context.loadBytes("context preview;\n");
    context.setReadOnly(true);
    const auto contextPointSize = context.send(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT);
    for (auto *view : {&editor, &context})
        view->send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, 2000);
    QSignalSpy changed(&editor, &CodeEditor::contentEdited);
    if (dpiSignal) {
        QVERIFY(editor.screen());
        QCOMPARE(context.screen(), editor.screen());
        QVERIFY(QMetaObject::invokeMethod(editor.screen(), "logicalDotsPerInchChanged",
                                          Qt::DirectConnection,
                                          Q_ARG(qreal, editor.screen()->logicalDotsPerInch())));
    } else {
        for (auto *view : {&editor, &context}) {
            QEvent event(QEvent::ScreenChangeInternal);
            QApplication::sendEvent(view, &event);
        }
    }
    QCOMPARE(editor.send(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT), pointSize);
    QCOMPARE(context.send(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT), contextPointSize);
    QCOMPARE(context.bytes(), QByteArray("context preview;\n"));
    QVERIFY(context.send(SCI_GETREADONLY));
    QCOMPARE(editor.bytes(), contents);
    QCOMPARE(editor.send(SCI_GETSELECTIONSTART), 4);
    QCOMPARE(editor.send(SCI_GETSELECTIONEND), 9);
    QCOMPARE(editor.send(SCI_GETZOOM), 2);
    QCOMPARE(editor.send(SCI_GETMARGINWIDTHN, 0),
             editor.sends(SCI_TEXTWIDTH, STYLE_LINENUMBER, "99999 ") + 8);
    QCOMPARE(editor.sends(SCI_TEXTWIDTH, STYLE_DEFAULT, "MMMMMMMMMM"), width);
    QVERIFY(editor.isModified());
    QCOMPARE(changed.count(), 0);
    editor.send(SCI_UNDO);
    QCOMPARE(editor.bytes(), QByteArray("int value;\n"));
}

void ConfigurationTests::partialOverrides() {
    const auto loaded = parseSettings(R"({
        "schemaVersion": 1,
        "ui": {"fontSizePx": 16},
        "colors": {"source.background": "#102030"},
        "syntax": {"comment": {"italic": true, "foreground": "#abcdef"}},
        "behavior": {"contextDebounceMs": 250}
    })");
    QVERIFY2(loaded.settings.has_value(), qPrintable(loaded.error));
    const auto &settings = *loaded.settings;
    QCOMPARE(settings.ui.fontSizePx, 16);
    QCOMPARE(settings.editor.typography.pointSize,
             defaultAppSettings().editor.typography.pointSize);
    QCOMPARE(settings.color("source.background"), QColor("#102030"));
    QVERIFY(settings.syntax[static_cast<size_t>(TextStyle::Comment)].italic);
    QCOMPARE(settings.behavior.contextDebounceMs, 250);
    QCOMPARE(settings.behavior.workspaceRefreshMinutes, 360);
    const auto explicitSize = parseSettings(R"({"editor":{"fontSizePt":8.25}})");
    QVERIFY2(explicitSize.settings.has_value(), qPrintable(explicitSize.error));
    QCOMPARE(explicitSize.settings->editor.typography.pointSize, 8.25);
}

void ConfigurationTests::workspaceRefreshConfiguration() {
    QTemporaryDir dir;
    const auto path = dir.filePath("settings.json");
    writeJson(path, R"({"behavior":{"workspaceRefreshMinutes":720}})");
    MainWindow window(false, nullptr, path);
    auto *index = window.findChild<WorkspaceIndex *>();
    QCOMPARE(index->refreshIntervalMinutes(), 720);
    writeJson(path, R"({"behavior":{"workspaceRefreshMinutes":60}})");
    QVERIFY(window.reloadConfiguration());
    QCOMPARE(index->refreshIntervalMinutes(), 60);
    QVERIFY(window.saveConfiguration());
    const auto saved = parseSettings(read(path));
    QVERIFY2(saved.settings.has_value(), qPrintable(saved.error));
    QCOMPARE(saved.settings->behavior.workspaceRefreshMinutes, 60);
    writeJson(path, R"({"behavior":{"workspaceRefreshMinutes":0}})");
    QVERIFY(!window.reloadConfiguration());
    QCOMPARE(index->refreshIntervalMinutes(), 60);
    writeJson(path, "{}");
    QVERIFY(window.reloadConfiguration());
    QCOMPARE(index->refreshIntervalMinutes(), 360);
    QSignalSpy failed(index, &WorkspaceIndex::failed);
    index->setRefreshIntervalMinutes(0);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(index->refreshIntervalMinutes(), 360);
    index->setRefreshIntervalMinutes(RuntimeLimits::MaximumWorkspaceRefreshMinutes);
    QCOMPARE(index->refreshIntervalMinutes(), RuntimeLimits::MaximumWorkspaceRefreshMinutes);
}

void ConfigurationTests::invalidConfiguration_data() {
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<QString>("message");
    QTest::newRow("malformed") << QByteArray("{") << QString("JSON error");
    QTest::newRow("nonobject") << QByteArray("[]") << QString("object");
    QTest::newRow("version") << QByteArray(R"({"schemaVersion":2})") << QString("schemaVersion");
    QTest::newRow("wrong-type") << QByteArray(R"({"editor":{"fontSizePt":"12"}})")
                                << QString("editor.fontSizePt");
    QTest::newRow("unknown") << QByteArray(R"({"colors":{"source.backgrond":"#ffffff"}})")
                             << QString("unknown");
    QTest::newRow("null") << QByteArray(R"({"ui":null})") << QString("ui");
    QTest::newRow("color") << QByteArray(
                                  R"({"colors":{"source.background":"red; font-size:80px"}})")
                           << QString("#RRGGBB");
    QTest::newRow("small-font") << QByteArray(R"({"editor":{"fontSizePt":0}})")
                                << QString("fontSizePt");
    QTest::newRow("large-font") << QByteArray(R"({"ui":{"fontSizePx":500}})")
                                << QString("fontSizePx");
    QTest::newRow("fractional-pixels")
        << QByteArray(R"({"ui":{"rowHeight":14.5}})") << QString("integer");
    QTest::newRow("layout-length")
        << QByteArray(R"({"layout":{"columns":[250,650]}})") << QString("expected 3");
    QTest::newRow("layout-type") << QByteArray(R"({"layout":{"columns":[250,"650",500]}})")
                                 << QString("integers");
    QTest::newRow("bad-style") << QByteArray(R"({"syntax":{"comment":{"scale":9}}})")
                               << QString("syntax.comment.scale");
    QTest::newRow("font-control") << QByteArray(R"({"ui":{"fontFamily":"bad\nfont"}})")
                                  << QString("control");
    QTest::newRow("timer") << QByteArray(R"({"behavior":{"parseDebounceMs":0}})")
                           << QString("parseDebounceMs");
    QTest::newRow("refresh-zero") << QByteArray(R"({"behavior":{"workspaceRefreshMinutes":0}})")
                                  << QString("workspaceRefreshMinutes");
    QTest::newRow("refresh-negative")
        << QByteArray(R"({"behavior":{"workspaceRefreshMinutes":-1}})")
        << QString("workspaceRefreshMinutes");
    QTest::newRow("refresh-fraction")
        << QByteArray(R"({"behavior":{"workspaceRefreshMinutes":1.5}})") << QString("integer");
    QTest::newRow("refresh-overflow")
        << QByteArray(R"({"behavior":{"workspaceRefreshMinutes":35792}})")
        << QString("workspaceRefreshMinutes");
    QTest::newRow("refresh-type") << QByteArray(R"({"behavior":{"workspaceRefreshMinutes":"360"}})")
                                  << QString("workspaceRefreshMinutes");
    QTest::newRow("oversized") << QByteArray(256 * 1024 + 1, ' ') << QString("256 KiB");
}

void ConfigurationTests::invalidConfiguration() {
    QFETCH(QByteArray, json);
    QFETCH(QString, message);
    const auto result = parseSettings(json);
    QVERIFY(!result.settings);
    QVERIFY2(result.error.contains(message), qPrintable(result.error));
}

void ConfigurationTests::missingFileAndAtomicSave() {
    QTemporaryDir dir;
    SettingsFile file(dir.filePath("config/settings.json"));
    const auto result = file.load();
    QVERIFY(result.settings.has_value());
    QVERIFY(!file.existed());
    auto settings = *result.settings;
    settings.colors["source.foreground"] = QColor("#998877");
    QVERIFY(file.save(settings).isEmpty());
    QVERIFY(file.existed());
    SettingsFile reopened(file.path());
    const auto loaded = reopened.load();
    QVERIFY(loaded.settings);
    QCOMPARE(loaded.settings->color("source.foreground"), QColor("#998877"));
    QVERIFY(!QFileInfo::exists(file.path() + ".lock"));
}

void ConfigurationTests::invalidFilesAreNotOverwritten() {
    QTemporaryDir dir;
    const auto path = dir.filePath("broken.json");
    writeJson(path, "{invalid");
    SettingsFile file(path);
    QVERIFY(!file.load().settings);
    QVERIFY(!file.save(defaultAppSettings()).isEmpty());
    QCOMPARE(read(path), QByteArray("{invalid"));
}

void ConfigurationTests::externalChangesAreNotOverwritten() {
    QTemporaryDir dir;
    const auto path = dir.filePath("settings.json");
    SettingsFile file(path);
    QVERIFY(file.load().settings);
    QVERIFY(file.save(defaultAppSettings()).isEmpty());
    const QByteArray edited = R"({"ui":{"fontSizePx":18}})";
    writeJson(path, edited);
    QVERIFY(file.save(defaultAppSettings()).contains("externally"));
    QCOMPARE(read(path), edited);
    QVERIFY(file.load().settings);
    QVERIFY(QFile::remove(path));
    QVERIFY(file.save(defaultAppSettings()).contains("externally"));
    QVERIFY(!QFileInfo::exists(path));
}

void ConfigurationTests::concurrentWriterIsReported() {
    QTemporaryDir dir;
    SettingsFile file(dir.filePath("settings.json"));
    QVERIFY(file.load().settings);
    QLockFile lock(file.path() + ".lock");
    QVERIFY(lock.tryLock(0));
    QVERIFY(file.save(defaultAppSettings()).contains("Another process"));
    QVERIFY(!QFileInfo::exists(file.path()));
}

void ConfigurationTests::reloadUpdatesBothViewsWithoutEditing() {
    QTemporaryDir dir;
    const auto path = dir.filePath("ui.json");
    writeJson(path, "{}");
    MainWindow window(false, nullptr, path);
    window.show();
    QSignalSpy parsed(&window, &MainWindow::analysisReady);
    QVERIFY(openTypographyFixture(window));
    QTRY_VERIFY_WITH_TIMEOUT(parsed.count() > 0, 10000);
    auto *editor = window.activeEditor();
    editor->sends(SCI_APPENDTEXT, 6, "// new");
    QTRY_VERIFY_WITH_TIMEOUT(parsed.count() > 1, 10000);
    editor->selectRange(4, 12);
    const auto contents = editor->bytes();
    const auto before = parsed.count();
    QSignalSpy changed(editor, &CodeEditor::contentEdited);
    writeJson(path, R"({
        "ui":{"fontSizePx":20,"rowHeight":25,"bandHeight":30},
        "editor":{"fontSizePt":12,"showLineNumbers":true,"highlightCurrentLine":true},
        "colors":{"source.background":"#102030","source.selectionBackground":"#123456","ui.background":"#304050"},
        "syntax":{
            "function":{"foreground":"#abcdef","italic":true,"bold":false},
            "parameter":{"foreground":"#bb8844","bold":false},
            "variable":{"foreground":"#2288dd"},
            "constant":{"foreground":"#dd3322"},
            "punctuation":{"foreground":"#eeee66"}
        }
    })");
    QVERIFY(window.reloadConfiguration());
    for (auto *view : {editor, window.contextEditor()}) {
        QCOMPARE(view->send(SCI_STYLEGETBACK, STYLE_DEFAULT), 0x302010);
        QCOMPARE(view->send(SCI_STYLEGETFORE, static_cast<int>(TextStyle::Function)), 0xefcdab);
        QCOMPARE(view->send(SCI_STYLEGETITALIC, static_cast<int>(TextStyle::Function)), 1);
        QCOMPARE(view->send(SCI_STYLEGETBOLD, static_cast<int>(TextStyle::Function)), 0);
        for (size_t i = 0; i < window.configuration().syntax.size(); ++i) {
            const auto &style = window.configuration().syntax[i];
            const auto foreground = style.foreground;
            QCOMPARE(view->send(SCI_STYLEGETFORE, i),
                     foreground.red() | (foreground.green() << 8) | (foreground.blue() << 16));
            QCOMPARE(view->send(SCI_STYLEGETBOLD, i), style.bold);
            QCOMPARE(view->send(SCI_STYLEGETITALIC, i), style.italic);
        }
        QCOMPARE(view->send(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT),
                 qRound(1200.0 * 96.0 / view->logicalDpiY()));
        QVERIFY(view->send(SCI_GETMARGINWIDTHN, 0) > 0);
        QCOMPARE(view->send(SCI_GETCARETLINEVISIBLE), 1);
    }
    QCOMPARE(editor->bytes(), contents);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), 4);
    QCOMPARE(editor->send(SCI_GETSELECTIONEND), 12);
    QVERIFY(editor->isModified());
    QCOMPARE(changed.count(), 0);
    QTest::qWait(200);
    QCOMPARE(parsed.count(), before);
    auto *tree = window.findChild<QTreeWidget *>("outline");
    QVERIFY(tree->visualItemRect(tree->topLevelItem(0)).height() >= 25);
    QCOMPARE(window.findChild<QTabBar *>("documentBar")->height(),
             UiAppearance::bandHeight(window.configuration()));
    editor->send(SCI_UNDO);
    QVERIFY(editor->bytes() != contents);
}

void ConfigurationTests::failedReloadPreservesCurrentAppearance() {
    QTemporaryDir dir;
    const auto path = dir.filePath("ui.json");
    writeJson(path, R"({"colors":{"source.background":"#223344"}})");
    MainWindow window(false, nullptr, path);
    const auto before = settingsToJson(window.configuration());
    writeJson(path, R"({"editor":{"fontSizePt":-5}})");
    QVERIFY(!window.reloadConfiguration());
    QCOMPARE(settingsToJson(window.configuration()), before);
    QVERIFY(!window.saveConfiguration());
    QCOMPARE(read(path), QByteArray(R"({"editor":{"fontSizePt":-5}})"));
}

void ConfigurationTests::uiSettingsPersistInJson() {
    QTemporaryDir dir;
    const auto path = dir.filePath("ui.json");
    writeJson(path, R"({"colors":{"source.background":"#112233"}})");
    {
        MainWindow window(false, nullptr, path);
        auto *size = window.findChild<QSpinBox *>("uiFontSize");
        size->setValue(16);
        window.setTypography({UiAppearance::sourceFont(), 11.5, 1.4, false, true});
        QVERIFY(window.saveConfiguration());
    }
    SettingsFile saved(path);
    const auto loaded = saved.load();
    QVERIFY(loaded.settings);
    QCOMPARE(loaded.settings->ui.fontSizePx, 16);
    QCOMPARE(loaded.settings->editor.typography.pointSize, 11.5);
    QCOMPARE(loaded.settings->color("source.background"), QColor("#112233"));
}

void ConfigurationTests::uniformSizesOverrideSyntaxScaling() {
    auto settings = defaultAppSettings();
    settings.syntax[static_cast<size_t>(TextStyle::Comment)].scale = 1.5;
    settings.editor.typography.uniform = true;
    CodeEditor editor;
    editor.applyConfiguration(settings);
    const auto baseSize = settings.editor.typography.pointSize * SC_FONT_SIZE_MULTIPLIER * 96.0 /
                          editor.logicalDpiY();
    QCOMPARE(editor.send(SCI_STYLEGETSIZEFRACTIONAL, static_cast<int>(TextStyle::Comment)),
             qRound(baseSize));
    QCOMPARE(editor.send(SCI_STYLEGETSIZEFRACTIONAL, static_cast<int>(TextStyle::Function)),
             qRound(baseSize));
    settings.editor.typography.uniform = false;
    editor.applyConfiguration(settings);
    QCOMPARE(editor.send(SCI_STYLEGETSIZEFRACTIONAL, static_cast<int>(TextStyle::Comment)),
             qRound(baseSize * 1.5));
}

QTEST_MAIN(ConfigurationTests)
#include "ConfigurationTests.moc"
