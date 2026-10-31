#pragma once

#include "CodeEditor.h"
#include "WorkspaceIndex.h"
#include "LanguageServerClient.h"
#include "NavigationHistory.h"
#include "FileIO.h"
#include "ReferenceSearch.h"
#include "SourceDocument.h"
#include "DefinitionResolver.h"
#include "WorkspaceServices.h"
#include "ApplicationStateStore.h"
#include <QMainWindow>
#include <QFutureWatcher>
#include <QSet>
#include <QPointer>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QAction;
class QDialog;
class QComboBox;
class QTabBar;
class QCloseEvent;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool persistSettings = true, QWidget *parent = nullptr,
                        QString configPath = {}, QString stateDirectory = {});
    ~MainWindow() override;
    bool openFile(const QString &path);
    void openStartupPaths(const QStringList &paths);
    void openProject(const QString &path);
    bool closeProject();
    bool saveCurrent(bool saveAs = false);
    CodeEditor *activeEditor() const;
    CodeEditor *contextEditor() const { return contextPreviewEditor_; }
    int documentCount() const;
    void setTypography(const Typography &value);
    void previewSymbol(int index);
    bool reloadConfiguration();
    bool saveConfiguration();
    QString configurationPath() const { return configurationFile_.path(); }
    const AppSettings &configuration() const { return configuration_; }
signals:
    void analysisReady();
    void indexReady();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using Document = SourceDocument;
    struct DiskPreview {
        QByteArray bytes;
        SourceAnalysis analysis;
        QString error;
    };
    void buildUi();
    void initializeWorkspace();
    void populateFiles(const QStringList &paths);
    void filterFiles();
    void filterSymbols();
    void selectCategory(const QString &kind);
    void sortOutline(int mode);
    void rememberOutline();
    void findText(bool backwards = false);
    void showAppearance();
    void applyConfiguration();
    void syncConfigurationControls();
    void editConfigurationColor();
    void updateColorButton();
    void changeUiTypography();
    void resetConfiguredLayout();
    void navigateContext(int direction);
    void openContext();
    void initializeNavigation();
    void navigateTo(const NavigationLocation &location, bool pin = false, bool focus = false,
                    bool record = true);
    void commitNavigation(const NavigationLocation &location, const FileContents &file, bool pin,
                          bool focus, bool record, const NavigationLocation &origin,
                          const QPointer<QWidget> &previousFocus);
    NavigationLocation currentLocation() const;
    void navigateHistory(int direction);
    void pinDocument(Document *document);
    void navigateOutline(bool pin);
    void navigateProjectFile(bool pin);
    void navigateProjectSymbol(bool pin);
    const QByteArray &snapshot(Document *document) const;
    void requestHover(CodeEditor *editor, int position);
    void jumpToDefinition(CodeEditor *editor, int position);
    LanguageServerClient *languageService(const QString &path, const QByteArray &source);
    void connectLanguageService(LanguageServerClient *service);
    void handleLocations(quint64 token, const QString &channel, const QVector<LspLocation> &targets,
                         const QString &error);
    void navigateDefinition(const LspLocation &target);
    void lookupLink(CodeEditor *editor, int position, bool activate);
    WorkspaceServices *workspaceServices_;
    quint64 linkGeneration_ = 0;
    QByteArray hoverHash_;
    QString hoverPath_;
    QString jumpPath_;
    QByteArray jumpHash_;
    QString hoverName_;
    QString jumpName_;
    void fallbackLink(quint64 token, const QString &channel);
    quint64 referenceToken_ = 0;
    QPointer<LanguageServerClient> referenceService_;
    QPointer<QDialog> referencesDialog_;
    QWidget *referenceResultsPage_ = nullptr;
    QTreeWidget *referencesList_ = nullptr;
    QLabel *referencesStatus_ = nullptr;
    QAction *cancelReferenceAction_ = nullptr;
    bool referenceSearchBusy_ = false;
    QString referencesScope_;
    QStringList referencesFiles_;
    void displayReferences(const QVector<LspLocation> &targets, const QString &error);
    void displayReferenceHits(const ReferenceResults &results, bool semantic);
    QVector<ReferenceHit> referenceHits_;
    QVector<QTreeWidgetItem *> referenceRows_;
    void ensureReferenceResults();
    void cancelReferenceSearch();
    void activateReferenceResult(bool open);
    void stepReferenceResult(int direction, bool first = false);
    std::shared_ptr<std::atomic_bool> referenceCancelled_;
    QVector<SymbolOverlay> referenceOverlays_;
    QString referenceIdentifier_;
    QVector<ReferenceGroup> referenceGroups_;
    ReferenceSearchRequest referenceRequest_;
    bool referenceSemantic_ = false;
    QVector<LspLocation> referenceTargets_;
    QString referenceOriginPath_;
    QByteArray referenceOriginHash_;
    int referencePageOffset_ = 0;
    QSet<QString> referenceListedFiles_;
    QAction *moreReferencesAction_ = nullptr;
    void rebuildReferenceResults();
    void removeReferenceResults();
    void exportReferenceResults();
    void continueReferenceSearch();
    void trackReferenceEdit(const QString &path, int position, int removed, int inserted,
                            const QByteArray &beforeHash, const QByteArray &source);
    void lookupSymbolReferences();
    void jumpReferenceLink();
    void showReferenceResults(int index = -1);
    void returnToReferenceResults(const QString &path, int line);
    void updateReferenceMarkers();
    int selectedReferenceIndex_ = -1;
    bool showingReferenceOutline_ = false;
    QString sourceOutlineFilter_;
    QString referenceOutlineFilter_;
    QString selectedSearchQuery_;
    void updateSearchOutline();
    void selectSearchQuery();
    void removeSearchQuery();
    void applyReferenceAppearance();
    void touchReferenceHits(const QVector<ReferenceHit> &hits);
    bool touchReferenceResults_ = false;
    QVector<ReferenceHit> referenceTouchHits_;
    bool replaceReferenceResults_ = false;
    QVector<ReferenceGroup> pendingReferenceGroups_;
    void showPanelOptions(const QString &panel);
    void applyPanelConfiguration();
    void updateActions();
    QHash<QString, QAction *> actions_;
    void lookupReferences(CodeEditor *editor = nullptr, bool projectSearch = false,
                          const QString &symbolPath = {}, int symbolPosition = -1);
    QVector<SymbolOverlay> liveOverlays() const;
    void displaySymbols(const SymbolPage &page);
    void activateProjectSymbol(bool open);
    void activateCategorySymbol(bool open);
    void activateIndexedSymbol(const IndexedSymbol &entry, bool open);
    void previewTarget(const IndexedSymbol &target, const QString &provenance);
    void resolveDefinition(const QString &path, const QByteArray &source, int version,
                           int position);
    void handleDefinitions(quint64 token, const QVector<LspLocation> &targets,
                           const QString &error);
    void chooseDefinition(int index);
    void semanticPreview(const LspLocation &target);
    void restartLanguageService();
    void readWorkspaceOptions();
    void writeWorkspaceOptions();
    void refreshWorkspace(bool force, bool rescan);
    void reconcileOpenDocuments(const IndexResult &result);
    Document *currentDocument() const;
    Document *findDocument(const QString &path) const;
    void addDocument(const QString &path, const QByteArray &bytes, bool bom, bool focus = true,
                     bool preview = false, int index = -1);
    void startAnalysis(Document *document);
    void updateOutline();
    void updateOpenSymbols();
    void updateContext();
    void showContext(const QString &title, const QByteArray &bytes, const SourceAnalysis &analysis,
                     int start, int end, bool referenceLine = false);
    void previewFile(const QString &path);
    void startDiskPreview();
    void applyTypography();
    void updateStatus();
    void updateTab(Document *document);
    bool confirmClose(Document *document);
    void closeTab(int index, bool confirm = true);
    bool saveProjectSession();
    void restoreProjectSession();
    void reportError(const QString &message);
    void restoreApplicationState();
    bool saveApplicationState();
    void filterOutline();
    void syncOutlineToEditor(bool clearWhenOutside = true);
    void clearPanelFilter(QLineEdit *filter);
    bool revealSelectedSymbol_ = false;
    bool symbolFilterPending_ = false;
    std::optional<bool> symbolNavigationAfterFilter_;

    bool persistSettings_;
    ApplicationStateStore stateStore_;
    ApplicationState applicationState_;
    QString stateError_;
    SettingsFile configurationFile_;
    AppSettings configuration_;
    bool configurationDirty_ = false;
    QString configurationError_;
    std::vector<std::unique_ptr<Document>> documents_;
    QTabWidget *documentsTabs_;
    QTabBar *documentBar_;
    QSplitter *mainSplitter_;
    QSplitter *rightSplitter_;
    QTreeWidget *outline_;
    QTreeWidget *fileList_;
    QTreeWidget *symbolList_;
    QTreeWidget *folderTree_;
    QTreeWidget *categoryTree_;
    QLineEdit *outlineFilter_;
    QLineEdit *fileFilter_;
    QLineEdit *symbolFilter_;
    QTabWidget *browserTabs_;
    CodeEditor *contextPreviewEditor_;
    QLabel *contextTitle_;
    QLabel *outlineState_;
    QLabel *outlineTitle_;
    QLabel *projectState_;
    QLabel *metrics_;
    QLabel *formatStatus_;
    QAction *pinContextAction_;
    QAction *contextBack_;
    QAction *contextForward_;
    QAction *contextOpen_;
    QAction *lineNumbers_;
    QAction *refreshFiles_;
    QAction *historyBack_;
    QAction *historyForward_;
    NavigationHistory navigationHistory_;
    QFutureWatcher<FileContents> *navigationJob_;
    quint64 navigationGeneration_ = 0;
    bool navigationBusy_ = false;
    struct OpenRequest {
        NavigationLocation location;
        NavigationLocation origin;
        QPointer<QWidget> focusWidget;
        bool pin = false;
        bool focus = false;
        bool record = true;
        quint64 generation = 0;
    };
    std::optional<OpenRequest> pendingOpen_;
    void startNavigationRead();
    QTimer *hoverTimer_;
    QPointer<CodeEditor> hoverEditor_;
    int hoverPosition_ = -1;
    quint64 hoverToken_ = 0;
    quint64 jumpToken_ = 0;
    QPointer<CodeEditor> jumpEditor_;
    QAction *rebuildSymbols_;
    QAction *rebuildAll_;
    QAction *cancelIndex_;
    QLabel *symbolScope_;
    QComboBox *definitionChoices_;
    WorkspaceIndex *workspaceIndex_;
    LanguageServerClient *cppLanguageService_;
    SymbolPage symbolPage_;
    QStringList indexWarnings_;
    QTimer *symbolQueryTimer_;
    QString compilationDirectory_;
    DefinitionResolver *definitionResolver_;
    QVector<IndexedSymbol> fallbackTargets_;
    QVector<LspLocation> semanticTargets_;
    bool semanticChoice_ = false;
    QString lookupName_;
    QString lookupProvider_;
    bool hasPreviewTarget_ = false;
    IndexedSymbol previewTarget_;
    bool pendingSemantic_ = false;
    LspLocation pendingDefinition_;
    QString pendingProvenance_;
    bool openAfterPreview_ = false;
    bool contextFollowsEditor_ = true;
    QCheckBox *uniformSizesCheckBox_;
    QCheckBox *whitespaceCheckBox_;
    QFontComboBox *sourceFontComboBox_;
    QDoubleSpinBox *sourceFontSizeSpinBox_;
    QDoubleSpinBox *declarationScaleSpinBox_;
    QDialog *appearanceDialog_;
    QFontComboBox *uiFontComboBox_;
    QSpinBox *uiFontSizeSpinBox_;
    QCheckBox *currentLine_;
    QComboBox *colorRole_;
    QPushButton *colorButton_;
    QLabel *configurationPathLabel_;
    QString category_;
    int outlineSort_ = 0;
    QString outlineDocument_;
    bool updatingOutline_ = false;
    QString searchText_;
    struct ContextEntry {
        QString title;
        QString path;
        QByteArray bytes;
        SourceAnalysis analysis;
        int start = 0;
        int end = 0;
        bool referenceLine = false;
    };
    QVector<ContextEntry> contextHistory_;
    int contextHistoryIndex_ = -1;
    bool navigatingContext_ = false;
    QString contextPath_;
    int contextStart_ = 0;
    int contextEnd_ = 0;
    bool contextReferenceLine_ = false;
    QTimer *contextTimer_;
    QString projectPath_;
    QFutureWatcher<DiskPreview> *diskPreview_;
    bool diskPreviewBusy_ = false;
    QString pendingPreviewPath_;
    quint64 contextGeneration_ = 0;
    quint64 diskGeneration_ = 0;
    QString diskPath_;
};
