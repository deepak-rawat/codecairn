#include "MainWindow.h"
#include "ReferenceResultsView.h"
#include "Diagnostics.h"
#include "UiAppearance.h"
#include "RuntimeLimits.h"
#include "NavigationTree.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QUrl>

namespace {
QVBoxLayout *compactLayout(QWidget *widget) {
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(0);
    return layout;
}

QToolBar *smallToolbar(const QString &name, QWidget *parent, const AppSettings &settings) {
    auto *bar = new QToolBar(parent);
    bar->setObjectName(name);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(settings.ui.iconSize, settings.ui.iconSize));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setFixedHeight(UiAppearance::bandHeight(settings));
    return bar;
}

QLineEdit *filterEntry(const QString &name, const QString &hint, const AppSettings &settings) {
    auto *entry = new QLineEdit;
    entry->setObjectName(name);
    entry->setPlaceholderText(hint);
    entry->setAccessibleName(hint);
    entry->setClearButtonEnabled(true);
    entry->setFixedHeight(UiAppearance::bandHeight(settings));
    return entry;
}
}

void MainWindow::buildUi() {
    setFont(UiAppearance::uiFont(configuration_));
    setStyleSheet(UiAppearance::styleSheet(configuration_));
    setWindowTitle("CodeCairn");
    setWindowIcon(UiAppearance::applicationIcon());
    resize(configuration_.layout.width, configuration_.layout.height);
    setMinimumSize(900, 500);

    documentsTabs_ = new QTabWidget;
    documentsTabs_->setObjectName("sourceTabs");
    documentsTabs_->setDocumentMode(true);
    documentsTabs_->tabBar()->hide();
    documentBar_ = new QTabBar;
    documentBar_->setObjectName("documentBar");
    documentBar_->setExpanding(false);
    documentBar_->setDrawBase(false);
    documentBar_->setTabsClosable(true);
    documentBar_->setUsesScrollButtons(true);
    documentBar_->setFixedHeight(UiAppearance::bandHeight(configuration_));
    connect(documentBar_, &QTabBar::currentChanged, documentsTabs_, &QTabWidget::setCurrentIndex);
    connect(documentBar_, &QTabBar::tabCloseRequested, this,
            [this](int index) { closeTab(index); });
    connect(documentsTabs_, &QTabWidget::currentChanged, documentBar_, &QTabBar::setCurrentIndex);

    appearanceDialog_ = new QDialog(this);
    appearanceDialog_->setObjectName("sourceAppearance");
    appearanceDialog_->setWindowTitle("Source Appearance");
    auto *form = new QFormLayout(appearanceDialog_);
    sourceFontComboBox_ = new QFontComboBox;
    sourceFontComboBox_->setObjectName("fontFamily");
    sourceFontComboBox_->setCurrentFont(QFont(configuration_.editor.typography.family.isEmpty()
                                                  ? UiAppearance::sourceFont()
                                                  : configuration_.editor.typography.family));
    sourceFontSizeSpinBox_ = new QDoubleSpinBox;
    sourceFontSizeSpinBox_->setObjectName("fontSize");
    sourceFontSizeSpinBox_->setRange(6, 32);
    sourceFontSizeSpinBox_->setDecimals(2);
    sourceFontSizeSpinBox_->setSingleStep(0.25);
    sourceFontSizeSpinBox_->setValue(configuration_.editor.typography.pointSize);
    sourceFontSizeSpinBox_->setSuffix(" pt");
    declarationScaleSpinBox_ = new QDoubleSpinBox;
    declarationScaleSpinBox_->setObjectName("declarationScale");
    declarationScaleSpinBox_->setRange(1.0, 1.8);
    declarationScaleSpinBox_->setDecimals(2);
    declarationScaleSpinBox_->setSingleStep(0.05);
    declarationScaleSpinBox_->setValue(configuration_.editor.typography.declarationScale);
    uniformSizesCheckBox_ = new QCheckBox("Use the same point size for all syntax styles");
    uniformSizesCheckBox_->setObjectName("uniformTypography");
    whitespaceCheckBox_ = new QCheckBox("Show spaces and tabs");
    currentLine_ = new QCheckBox("Highlight current line");
    currentLine_->setObjectName("highlightCurrentLine");
    uiFontComboBox_ = new QFontComboBox;
    uiFontComboBox_->setObjectName("uiFontFamily");
    uiFontComboBox_->setCurrentFont(UiAppearance::uiFont(configuration_));
    uiFontSizeSpinBox_ = new QSpinBox;
    uiFontSizeSpinBox_->setObjectName("uiFontSize");
    uiFontSizeSpinBox_->setRange(8, 32);
    uiFontSizeSpinBox_->setValue(configuration_.ui.fontSizePx);
    uiFontSizeSpinBox_->setSuffix(" px");
    connect(uiFontComboBox_, &QFontComboBox::currentFontChanged, this,
            &MainWindow::changeUiTypography);
    connect(uiFontSizeSpinBox_, &QSpinBox::valueChanged, this, &MainWindow::changeUiTypography);
    form->addRow("UI font", uiFontComboBox_);
    form->addRow("UI font size (logical)", uiFontSizeSpinBox_);
    form->addRow("Source font", sourceFontComboBox_);
    form->addRow("Base size", sourceFontSizeSpinBox_);
    form->addRow("Declaration scale", declarationScaleSpinBox_);
    form->addRow(uniformSizesCheckBox_);
    form->addRow(whitespaceCheckBox_);
    form->addRow(currentLine_);
    colorRole_ = new QComboBox;
    colorRole_->setObjectName("colorRole");
    for (auto it = configuration_.colors.begin(); it != configuration_.colors.end(); ++it)
        colorRole_->addItem(it.key(), it.key());
    for (const auto name : syntaxNames())
        colorRole_->addItem("syntax." + QString::fromLatin1(name),
                            "syntax." + QString::fromLatin1(name));
    colorButton_ = new QPushButton;
    colorButton_->setObjectName("colorValue");
    connect(colorRole_, &QComboBox::currentIndexChanged, this, &MainWindow::updateColorButton);
    connect(colorButton_, &QPushButton::clicked, this, &MainWindow::editConfigurationColor);
    form->addRow("Color role", colorRole_);
    form->addRow("Color", colorButton_);
    auto *note =
        new QLabel("Changes apply to both source views. Scintilla uses a uniform line height.\n"
                   "Font fallback and rasterization vary by platform.");
    note->setWordWrap(true);
    form->addRow(note);
    configurationPathLabel_ =
        new QLabel(configurationFile_.path().isEmpty() ? "In-memory settings (no file)"
                                                       : configurationFile_.path());
    configurationPathLabel_->setObjectName("configurationPath");
    configurationPathLabel_->setWordWrap(true);
    configurationPathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("JSON file", configurationPathLabel_);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Close | QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Save);
    connect(buttons, &QDialogButtonBox::rejected, appearanceDialog_, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QAbstractButton::clicked, this,
            [this] {
                configuration_ = defaultAppSettings();
                configurationDirty_ = true;
                applyConfiguration();
            });
    buttons->button(QDialogButtonBox::Save)->setText("Save Configuration");
    connect(buttons->button(QDialogButtonBox::Save), &QAbstractButton::clicked, this, [this] {
        if (!saveConfiguration())
            QMessageBox::warning(this, "UI configuration", configurationError_);
    });
    form->addRow(buttons);

    outline_ = new NavigationTree;
    outline_->setObjectName("outline");
    UiAppearance::configureTree(outline_, {"Symbol"}, configuration_);
    outline_->setHeaderHidden(true);
    outline_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    outlineTitle_ = new QLabel("Source");
    outlineTitle_->setObjectName("outlineTitle");
    outlineTitle_->setFixedHeight(UiAppearance::bandHeight(configuration_));
    outlineFilter_ = filterEntry("outlineFilter", "Symbol Name (Alt+L)", configuration_);
    auto *left = new QWidget;
    auto *leftLayout = compactLayout(left);
    leftLayout->addWidget(outlineTitle_);
    leftLayout->addWidget(outlineFilter_);
    leftLayout->addWidget(outline_, 1);
    auto *outlineBar = smallToolbar("outlineToolbar", left, configuration_);
    leftLayout->addWidget(outlineBar);
    outlineState_ = new QLabel(this);
    outlineState_->hide();

    contextPreviewEditor_ = new CodeEditor;
    contextPreviewEditor_->setObjectName("contextPreview");
    contextPreviewEditor_->setReadOnly(true);
    contextPreviewEditor_->send(SCI_SETCARETLINEVISIBLE, false);
    contextTitle_ = new QLabel("Context");
    contextTitle_->setObjectName("contextTitle");
    contextTitle_->setFixedHeight(UiAppearance::bandHeight(configuration_));
    contextTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *contextPanel = new QWidget;
    auto *contextLayout = compactLayout(contextPanel);
    contextLayout->addWidget(contextTitle_);
    definitionChoices_ = new QComboBox;
    definitionChoices_->setObjectName("definitionChoices");
    definitionChoices_->setFixedHeight(UiAppearance::bandHeight(configuration_));
    definitionChoices_->hide();
    contextLayout->addWidget(definitionChoices_);
    contextLayout->addWidget(contextPreviewEditor_, 1);
    auto *contextBar = smallToolbar("contextToolbar", contextPanel, configuration_);
    contextLayout->addWidget(contextBar);

    browserTabs_ = new QTabWidget;
    browserTabs_->setObjectName("browserTabs");
    browserTabs_->setDocumentMode(true);
    browserTabs_->setElideMode(Qt::ElideNone);
    fileList_ = new NavigationTree;
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileList_->setObjectName("fileList");
    UiAppearance::configureTree(fileList_, {"File Name", "Directory", "Size"}, configuration_);
    fileList_->setRootIsDecorated(false);
    fileList_->setColumnWidth(0, 138);
    fileList_->setColumnWidth(1, 300);
    fileList_->header()->setStretchLastSection(false);
    fileList_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    fileList_->setColumnWidth(2, 44);
    fileList_->headerItem()->setToolTip(2, "File size in bytes");
    fileFilter_ = filterEntry("fileFilter", "File Name (Ctrl+O)", configuration_);
    auto *files = new QWidget;
    auto *filesLayout = compactLayout(files);
    filesLayout->addWidget(fileFilter_);
    filesLayout->addWidget(fileList_, 1);
    auto *filesBar = smallToolbar("filesToolbar", files, configuration_);
    filesLayout->addWidget(filesBar);
    browserTabs_->addTab(files, "Project Files");
    projectState_ = new QLabel(this);
    projectState_->hide();

    symbolList_ = new NavigationTree;
    symbolList_->setObjectName("symbolList");
    UiAppearance::configureTree(symbolList_, {"Symbol Name", "File"}, configuration_);
    symbolList_->setColumnWidth(0, 215);
    symbolList_->setRootIsDecorated(false);
    symbolFilter_ = filterEntry("symbolFilter", "Symbol Name (Ctrl+Shift+S)", configuration_);
    symbolFilter_->setToolTip("Workspace symbols: name fragments; append ( for functions; prefix ? "
                              "for a regular expression.");
    auto *symbols = new QWidget;
    auto *symbolLayout = compactLayout(symbols);
    symbolLayout->addWidget(symbolFilter_);
    symbolLayout->addWidget(symbolList_, 1);
    auto *symbolsBar = smallToolbar("symbolsToolbar", symbols, configuration_);
    symbolLayout->addWidget(symbolsBar);
    symbolScope_ = new QLabel("No workspace");
    symbolScope_->setObjectName("symbolScope");
    symbolsBar->addWidget(symbolScope_);
    browserTabs_->addTab(symbols, "Project Symbols");
    folderTree_ = new NavigationTree;
    folderTree_->setObjectName("folderTree");
    UiAppearance::configureTree(folderTree_, {"Folders"}, configuration_);
    folderTree_->setHeaderHidden(true);
    browserTabs_->addTab(folderTree_, "Folders");
    categoryTree_ = new NavigationTree;
    categoryTree_->setObjectName("categoryTree");
    UiAppearance::configureTree(categoryTree_, {"Symbol Type", "File Name"}, configuration_);
    categoryTree_->setColumnWidth(0, 300);
    categoryTree_->setExpandsOnDoubleClick(false);
    browserTabs_->addTab(categoryTree_, "Symbol Categories");
    connect(categoryTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item->parent())
            activateCategorySymbol(true);
        else
            selectCategory(item->data(0, Qt::UserRole).toString());
    });
    connect(categoryTree_, &QTreeWidget::itemClicked, this,
            [this] { activateCategorySymbol(false); });
    connect(qobject_cast<NavigationTree *>(categoryTree_),
            &NavigationTree::keyboardSelectionChanged, this,
            [this] { activateCategorySymbol(false); });
    connect(folderTree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, Qt::UserRole + 1).toString().isEmpty())
            previewFile(item->data(0, Qt::UserRole + 1).toString());
    });
    connect(folderTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const auto path = item->data(0, Qt::UserRole + 1).toString();
        if (!path.isEmpty())
            navigateTo({path}, true, true);
    });
    const auto browseFolderFile = [this] {
        const auto *item = folderTree_->currentItem();
        if (item && !item->data(0, Qt::UserRole + 1).toString().isEmpty())
            navigateTo({item->data(0, Qt::UserRole + 1).toString()});
    };
    connect(folderTree_, &QTreeWidget::itemClicked, this, browseFolderFile);
    connect(qobject_cast<NavigationTree *>(folderTree_), &NavigationTree::keyboardSelectionChanged,
            this, browseFolderFile);

    rightSplitter_ = new QSplitter(Qt::Vertical);
    rightSplitter_->setObjectName("rightSplitter");
    rightSplitter_->setChildrenCollapsible(false);
    rightSplitter_->setHandleWidth(configuration_.ui.splitterWidth);
    rightSplitter_->addWidget(contextPanel);
    rightSplitter_->addWidget(browserTabs_);
    mainSplitter_ = new QSplitter(Qt::Horizontal);
    mainSplitter_->setObjectName("mainSplitter");
    mainSplitter_->setChildrenCollapsible(false);
    mainSplitter_->setHandleWidth(configuration_.ui.splitterWidth);
    mainSplitter_->addWidget(left);
    mainSplitter_->addWidget(documentsTabs_);
    mainSplitter_->addWidget(rightSplitter_);
    left->setMinimumWidth(150);
    documentsTabs_->setMinimumWidth(280);
    rightSplitter_->setMinimumWidth(360);
    contextPanel->setMinimumHeight(100);
    browserTabs_->setMinimumHeight(140);
    resetConfiguredLayout();
    auto *workspace = new QWidget;
    auto *workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    workspaceLayout->addWidget(documentBar_, 0, Qt::AlignLeft);
    workspaceLayout->addWidget(mainSplitter_, 1);
    setCentralWidget(workspace);
    metrics_ = new QLabel;
    metrics_->setObjectName("positionStatus");
    statusBar()->addWidget(metrics_, 1);
    formatStatus_ = new QLabel;
    statusBar()->addPermanentWidget(formatStatus_);
    statusBar()->setFixedHeight(UiAppearance::bandHeight(configuration_) + 1);
    statusBar()->setSizeGripEnabled(false);

    auto action = [this](QMenu *menu, const QString &name, const QString &label,
                         const QString &glyph, const QKeySequence &shortcut, auto callback) {
        auto *value = new QAction(UiAppearance::icon(glyph), label, this);
        value->setObjectName(name);
        actions_.insert(name, value);
        value->setShortcut(shortcut);
        value->setToolTip(label + (shortcut.isEmpty()
                                       ? ""
                                       : " (" + shortcut.toString(QKeySequence::NativeText) + ")"));
        connect(value, &QAction::triggered, this, callback);
        if (menu)
            menu->addAction(value);
        return value;
    };
    auto *fileMenu = menuBar()->addMenu("&File");
    auto *editMenu = menuBar()->addMenu("&Edit");
    auto *searchMenu = menuBar()->addMenu("&Search");
    auto *projectMenu = menuBar()->addMenu("&Project");
    auto *viewMenu = menuBar()->addMenu("&View");
    auto *optionsMenu = menuBar()->addMenu("&Options");
    auto *windowMenu = menuBar()->addMenu("&Window");
    auto *helpMenu = menuBar()->addMenu("&Help");
    for (auto *menu :
         {fileMenu, editMenu, searchMenu, projectMenu, viewMenu, optionsMenu, windowMenu})
        connect(menu, &QMenu::aboutToShow, this, &MainWindow::updateActions);

    auto *open =
        action(fileMenu, "openFile", "Open File...", "open", QKeySequence("Ctrl+Alt+O"), [this] {
            const auto path = QFileDialog::getOpenFileName(this, "Open UTF-8 source file");
            if (!path.isEmpty())
                openFile(path);
        });
    auto *openFolder =
        action(projectMenu, "openFolder", "Open Folder...", "openFolder",
               QKeySequence("Ctrl+Shift+O"), [this] {
                   const auto path = QFileDialog::getExistingDirectory(this, "Open project folder");
                   if (!path.isEmpty())
                       openProject(path);
               });
    action(projectMenu, "closeProject", "Close Project", "file", {}, [this] { closeProject(); });
    auto *save =
        action(fileMenu, "saveFile", "Save", "save", QKeySequence::Save, [this] { saveCurrent(); });
    action(fileMenu, "saveAs", "Save As...", "save", QKeySequence("Ctrl+Alt+S"),
           [this] { saveCurrent(true); });
    action(fileMenu, "closeFile", "Close File", "file", QKeySequence::Close,
           [this] { closeTab(documentsTabs_->currentIndex()); });
    fileMenu->addSeparator();
    action(fileMenu, "quit", "Quit", "file", QKeySequence::Quit, [this] { close(); });
    auto editAction = [&](const QString &label, const QString &glyph, const QKeySequence &key,
                          unsigned message) {
        return action(editMenu, glyph, label, glyph, key, [this, message] {
            auto *focused = QApplication::focusWidget();
            auto *editor = focused && (focused == contextPreviewEditor_ ||
                                       contextPreviewEditor_->isAncestorOf(focused))
                               ? contextPreviewEditor_
                               : activeEditor();
            if (auto *entry = qobject_cast<QLineEdit *>(focused)) {
                if (message == SCI_COPY)
                    entry->copy();
                else if (message == SCI_CUT)
                    entry->cut();
                else if (message == SCI_PASTE)
                    entry->paste();
                else if (message == SCI_UNDO)
                    entry->undo();
                else if (message == SCI_REDO)
                    entry->redo();
                else if (message == SCI_SELECTALL)
                    entry->selectAll();
            } else if (auto *view = qobject_cast<ReferenceResultsView *>(referencesList_);
                       view && focused && (view == focused || view->isAncestorOf(focused))) {
                if (message == SCI_COPY)
                    view->copySelection();
                else if (message == SCI_SELECTALL)
                    view->selectAllText();
            } else if (editor)
                editor->send(message);
        });
    };
    auto *undo = editAction("Undo", "undo", QKeySequence::Undo, SCI_UNDO);
    auto *redo = editAction("Redo", "redo", QKeySequence::Redo, SCI_REDO);
    auto *cut = editAction("Cut", "cut", QKeySequence::Cut, SCI_CUT);
    auto *copy = editAction("Copy", "copy", QKeySequence::Copy, SCI_COPY);
    auto *paste = editAction("Paste", "paste", QKeySequence::Paste, SCI_PASTE);
    editAction("Select All", "selectAll", QKeySequence::SelectAll, SCI_SELECTALL);
    auto *find = action(searchMenu, "findText", "Find...", "find", QKeySequence::Find, [this] {
        bool ok = false;
        const auto text = QInputDialog::getText(this, "Find", "Text (case-sensitive)",
                                                QLineEdit::Normal, searchText_, &ok);
        if (ok && !text.isEmpty()) {
            searchText_ = text;
            findText();
        }
    });
    action(searchMenu, "lookupReferences", "Lookup References...", "find", QKeySequence("Ctrl+/"),
           [this] { lookupSymbolReferences(); });
    action(searchMenu, "searchProject", "Search Project...", "find", QKeySequence("Ctrl+Shift+F"),
           [this] { lookupReferences(nullptr, true); });
    action(searchMenu, "showSearchResults", "Search Results", "referenceLink", {},
           [this] { showReferenceResults(); });
    auto *deleteSearch = action(nullptr, "deleteSearchSymbol", "Delete Searched Symbol", "cut", {},
                                [this] { removeSearchQuery(); });
    deleteSearch->setShortcut(QKeySequence::Delete);
    deleteSearch->setShortcutContext(Qt::WidgetShortcut);
    outline_->addAction(deleteSearch);
    action(searchMenu, "jumpReference", "Jump to Reference Link", "referenceLink", QKeySequence("Ctrl+L"),
           [this] {
               jumpReferenceLink();
           });
    action(searchMenu, "firstReference", "First Reference", "referencePrevious", QKeySequence("Ctrl+Shift+L"),
           [this] { stepReferenceResult(1, true); });
    action(searchMenu, "nextReference", "Next Reference", "referenceNext", QKeySequence("Shift+F9"),
           [this] { stepReferenceResult(1); });
    action(searchMenu, "previousReference", "Previous Reference", "referencePrevious", QKeySequence("Shift+F8"),
           [this] { stepReferenceResult(-1); });
    action(searchMenu, "jumpDefinition", "Jump to Definition", "open", QKeySequence("Ctrl+="),
           [this] {
               auto *editor =
                   contextPreviewEditor_->hasFocus() ? contextPreviewEditor_ : activeEditor();
               if (editor)
                   jumpToDefinition(editor, editor->lookupPosition());
           });
    action(searchMenu, "findNext", "Find Next", "forward", QKeySequence::FindNext,
           [this] { findText(); });
    action(searchMenu, "findPrevious", "Find Previous", "back", QKeySequence::FindPrevious,
           [this] { findText(true); });
    refreshFiles_ = action(projectMenu, "refreshFiles", "Refresh File List", "refresh",
                           QKeySequence("Ctrl+Shift+R"), [this] { refreshWorkspace(false, true); });
    refreshFiles_->setEnabled(false);
    rebuildSymbols_ = action(projectMenu, "rebuildSymbols", "Rebuild Symbol Index", "refresh", {},
                             [this] { refreshWorkspace(true, false); });
    rebuildAll_ = action(projectMenu, "rebuildAll", "Rebuild Files and Symbols", "refresh", {},
                         [this] { refreshWorkspace(true, true); });
    cancelIndex_ = action(projectMenu, "cancelIndex", "Cancel Background Indexing", "collapse", {},
                          [this] { workspaceIndex_->cancel(); });
    rebuildSymbols_->setEnabled(false);
    rebuildAll_->setEnabled(false);
    cancelIndex_->setEnabled(false);
    action(projectMenu, "restartLanguageService", "Restart Language Service", "refresh", {},
           [this] { restartLanguageService(); });
    action(projectMenu, "compileCommandsDirectory", "Compilation Database Directory...", "Folder",
           {}, [this] {
               if (projectPath_.isEmpty()) {
                   statusBar()->showMessage("Open a folder or file first.", 5000);
                   return;
               }
               const auto directory = QFileDialog::getExistingDirectory(
                   this, "Directory containing compile_commands.json", projectPath_);
               if (!directory.isEmpty()) {
                   compilationDirectory_ = directory;
                   writeWorkspaceOptions();
                   restartLanguageService();
               }
           });
    action(projectMenu, "workspaceExclusions", "Excluded Directory Names...", "Folder", {}, [this] {
        if (projectPath_.isEmpty()) {
            statusBar()->showMessage("Open a workspace first.", 5000);
            return;
        }
        bool accepted = false;
        const auto names = QInputDialog::getText(
            this, "Workspace exclusions",
            "Comma-separated directory names to skip at any depth.\n"
            "Remove build from this list if it contains source you want indexed.",
            QLineEdit::Normal, workspaceIndex_->excludes().join(", "), &accepted);
        if (accepted) {
            QStringList excludes;
            for (const auto &name : names.split(',', Qt::SkipEmptyParts))
                excludes << name.trimmed();
            workspaceIndex_->setExcludes(excludes);
            writeWorkspaceOptions();
        }
    });
    auto *filesFocus =
        action(viewMenu, "activateFiles", "Project File List", "file", QKeySequence::Open, [this] {
            browserTabs_->setCurrentIndex(0);
            fileFilter_->setFocus();
            fileFilter_->selectAll();
        });
    auto *symbolsFocus = action(viewMenu, "activateSymbols", "Project Symbol List", "Function",
                                QKeySequence("Ctrl+Shift+S"), [this] {
                                    browserTabs_->setCurrentIndex(1);
                                    symbolFilter_->setFocus();
                                    symbolFilter_->selectAll();
                                });
    action(viewMenu, "activateOutline", "File Symbol Window", "Type", QKeySequence("Alt+L"),
           [this] {
               outlineFilter_->setFocus();
               outlineFilter_->selectAll();
           });
    action(viewMenu, "activateContext", "Context Window", "file", {},
           [this] { contextPreviewEditor_->setFocus(); });
    lineNumbers_ = action(viewMenu, "lineNumbers", "Line Numbers", "sortLine", {},
                          [this] { applyTypography(); });
    lineNumbers_->setCheckable(true);
    auto *appearance = action(optionsMenu, "sourceAppearanceAction", "Source Appearance...",
                              "settings", {}, [this] { showAppearance(); });
    auto *outlineOptions = action(optionsMenu, "outlineOptions", "Symbol Window Options...",
                                  "settings", {}, [this] { showPanelOptions("outline"); });
    auto *filesOptions = action(optionsMenu, "filesOptions", "Project File List Options...",
                                "settings", {}, [this] { showPanelOptions("files"); });
    auto *symbolsOptions = action(optionsMenu, "symbolsOptions", "Project Symbol List Options...",
                                  "settings", {}, [this] { showPanelOptions("symbols"); });
    auto *contextOptions = action(optionsMenu, "contextOptions", "Context Window Options...",
                                  "settings", {}, [this] { showPanelOptions("context"); });
    action(optionsMenu, "openUiConfig", "Open UI Configuration File", "file", {}, [this] {
        const auto path = configurationFile_.path();
        if (path.isEmpty() || !QFileInfo::exists(path) ||
            !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            QMessageBox::warning(this, "UI configuration",
                                 "Cannot open the configuration file: " + path);
    });
    action(optionsMenu, "reloadUiConfig", "Reload UI Configuration", "refresh", {}, [this] {
        if (configurationDirty_ &&
            QMessageBox::question(this, "Reload UI configuration",
                                  "Discard unsaved appearance changes and reload the JSON file?",
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
        if (!reloadConfiguration())
            QMessageBox::warning(this, "UI configuration", configurationError_);
    });
    action(optionsMenu, "saveUiConfig", "Save UI Configuration", "save", {}, [this] {
        if (!saveConfiguration())
            QMessageBox::warning(this, "UI configuration", configurationError_);
    });
    action(optionsMenu, "saveUiConfigAs", "Save UI Configuration As...", "save", {}, [this] {
        const auto path =
            QFileDialog::getSaveFileName(this, "Save UI configuration", configurationFile_.path(),
                                         "JSON configuration (*.json)");
        if (path.isEmpty())
            return;
        SettingsFile destination(path);
        const auto loaded = destination.load();
        const auto error = loaded.settings ? destination.save(configuration_) : loaded.error;
        if (!error.isEmpty()) {
            QMessageBox::warning(this, "UI configuration", error);
            return;
        }
        configurationFile_ = std::move(destination);
        configurationDirty_ = false;
        configurationPathLabel_->setText(configurationFile_.path());
        statusBar()->showMessage("Saved UI configuration: " + configurationFile_.path(), 6000);
    });
    action(optionsMenu, "resetLayout", "Reset Reference Layout", "settings", {},
           [this] { resetConfiguredLayout(); });
    action(windowMenu, "nextDocument", "Next Document", "forward", QKeySequence("Ctrl+Tab"),
           [this] {
               if (documentsTabs_->count())
                   documentsTabs_->setCurrentIndex((documentsTabs_->currentIndex() + 1) %
                                                   documentsTabs_->count());
           });
    action(windowMenu, "previousDocument", "Previous Document", "back",
           QKeySequence("Ctrl+Shift+Tab"), [this] {
               if (documentsTabs_->count())
                   documentsTabs_->setCurrentIndex(
                       (documentsTabs_->currentIndex() + documentsTabs_->count() - 1) %
                       documentsTabs_->count());
           });
    action(helpMenu, "openLogsFolder", "Open Logs Folder", "openFolder", {}, [this] {
        const auto directory = Diagnostics::directory();
        if (directory.isEmpty() || !QFileInfo(directory).isDir() ||
            !QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
            QMessageBox::warning(this, "Diagnostics", "Cannot open the logs folder: " + directory);
    });
    action(helpMenu, "about", "About CodeCairn", "settings", {}, [this] {
        QMessageBox::information(this, "CodeCairn",
                                 "CodeCairn - Find your way through code.\n\n"
                                 "Qt + Scintilla + Tree-sitter + SQLite.\n"
                                 "Workspace data is stored in your per-user application state "
                                 "directory.\n"
                                 "clangd supplies C/C++ definitions; configure "
                                 "compile_commands.json for accurate results.");
    });

    auto *sortMenu = viewMenu->addMenu("Sort File Symbols");
    auto *sortGroup = new QActionGroup(this);
    for (int mode = 0; mode < 3; ++mode) {
        const QString label = mode == 0 ? "Source Order" : mode == 1 ? "Name Order" : "Kind Order";
        const QString glyph = mode == 0 ? "sortLine" : mode == 1 ? "sortName" : "sortKind";
        auto *sort = action(sortMenu, glyph, label, glyph, {}, [this, mode] { sortOutline(mode); });
        sort->setCheckable(true);
        sortGroup->addAction(sort);
        sort->setChecked(mode == 0);
        outlineBar->addAction(sort);
    }
    outlineBar->addSeparator();
    outlineBar->addAction(
        action(viewMenu, "expandOutline", "Expand All Symbols", "expand", {}, [this] {
            outline_->expandAll();
            rememberOutline();
        }));
    outlineBar->addAction(
        action(viewMenu, "collapseOutline", "Collapse All Symbols", "collapse", {}, [this] {
            outline_->collapseAll();
            rememberOutline();
        }));
    outlineBar->addAction(outlineOptions);
    contextBack_ = action(nullptr, "contextBack", "Previous Context", "back", {},
                          [this] { navigateContext(-1); });
    contextForward_ = action(nullptr, "contextForward", "Next Context", "forward", {},
                             [this] { navigateContext(1); });
    contextBack_->setEnabled(false);
    contextForward_->setEnabled(false);
    contextOpen_ = action(nullptr, "openContext", "Open Context in Editor", "open", {},
                          [this] { openContext(); });
    contextOpen_->setEnabled(false);
    auto *refreshContext =
        action(nullptr, "refreshContext", "Refresh from Current Editor", "refresh", {}, [this] {
            if (!pinContextAction_->isChecked()) {
                contextFollowsEditor_ = true;
                updateContext();
            }
        });
    pinContextAction_ = action(viewMenu, "pinContext", "Lock Context Window", "lock", {}, [] {});
    pinContextAction_->setCheckable(true);
    contextBar->addActions({contextBack_, contextForward_, contextOpen_, refreshContext});
    contextBar->addSeparator();
    contextBar->addActions({pinContextAction_, contextOptions});
    auto *openSelected = action(nullptr, "openSelectedFile", "Open Selected File", "open", {},
                                [this] { navigateProjectFile(true); });
    filesBar->addActions({openSelected, refreshFiles_, openFolder});
    filesBar->addSeparator();
    filesBar->addActions({symbolsFocus, filesOptions});
    symbolsBar->addSeparator();
    symbolsBar->addAction(action(nullptr, "clearCategory", "Show All Symbol Categories", "sortKind",
                                 {}, [this] { selectCategory({}); }));
    symbolsBar->addAction(symbolsOptions);
    auto *toolbar = smallToolbar("mainToolbar", this, configuration_);
    addToolBar(toolbar);
    historyBack_ = action(viewMenu, "historyBack", "Go Back", "back", QKeySequence("Alt+Left"),
                          [this] { navigateHistory(-1); });
    historyForward_ = action(viewMenu, "historyForward", "Go Forward", "forward",
                             QKeySequence("Alt+Right"), [this] { navigateHistory(1); });
    historyBack_->setEnabled(false);
    historyForward_->setEnabled(false);
    toolbar->addActions({historyBack_, historyForward_});
    toolbar->addSeparator();
    toolbar->addActions({open, openFolder, save});
    toolbar->addSeparator();
    toolbar->addActions({undo, redo, cut, copy, paste});
    toolbar->addSeparator();
    toolbar->addActions({find, filesFocus, symbolsFocus, refreshFiles_});
    toolbar->addSeparator();
    toolbar->addActions({actions_.value("previousReference"), actions_.value("nextReference")});
    toolbar->addSeparator();
    const auto zoomEditor = [this] {
        return contextPreviewEditor_->hasFocus() ? contextPreviewEditor_ : activeEditor();
    };
    action(viewMenu, "zoomIn", "Zoom In", "zoomIn", QKeySequence::ZoomIn, [zoomEditor] {
        if (auto *editor = zoomEditor())
            editor->changeZoom(1);
    });
    action(viewMenu, "zoomOut", "Zoom Out", "zoomOut", QKeySequence::ZoomOut, [zoomEditor] {
        if (auto *editor = zoomEditor())
            editor->changeZoom(-1);
    });
    action(viewMenu, "resetZoom", "Reset Source Zoom", "file", QKeySequence("Ctrl+0"),
           [zoomEditor] {
               if (auto *editor = zoomEditor())
                   editor->resetZoom();
           });
    toolbar->addActions({pinContextAction_, appearance});
    fileFilter_->setPlaceholderText(
        "File Name (" + filesFocus->shortcut().toString(QKeySequence::NativeText) + ')');
    symbolFilter_->setPlaceholderText(
        "Symbol Name (" + symbolsFocus->shortcut().toString(QKeySequence::NativeText) + ')');
    connect(qApp, &QApplication::focusChanged, this, [this] { updateActions(); });
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::updateActions);
    connect(contextPreviewEditor_, &CodeEditor::caretMoved, this, &MainWindow::updateActions);
    for (auto *entry : {outlineFilter_, fileFilter_, symbolFilter_})
        connect(entry, &QLineEdit::selectionChanged, this, &MainWindow::updateActions);
}

void MainWindow::updateActions() {
    auto enable = [this](const QString &name, bool value) {
        if (auto *action = actions_.value(name))
            action->setEnabled(value);
    };
    auto *documentEditor = activeEditor();
    auto *focused = QApplication::focusWidget();
    auto *entry = qobject_cast<QLineEdit *>(focused);
    auto *results = qobject_cast<ReferenceResultsView *>(referencesList_);
    const bool resultsFocus = results && focused && (results == focused || results->isAncestorOf(focused));
    auto *editor = focused && (focused == contextPreviewEditor_ ||
                               contextPreviewEditor_->isAncestorOf(focused))
                       ? contextPreviewEditor_
                       : documentEditor;
    const bool selected =
        resultsFocus ? results->textCursor().hasSelection() : entry ? entry->hasSelectedText()
              : editor && editor->send(SCI_GETSELECTIONSTART) != editor->send(SCI_GETSELECTIONEND);
    const bool writable = !resultsFocus && (entry ? !entry->isReadOnly() : editor && !editor->send(SCI_GETREADONLY));
    enable("copy", selected);
    enable("cut", selected && writable);
    const auto *mime = QApplication::clipboard()->mimeData();
    enable("paste", writable && mime && mime->hasText());
    enable("undo", entry ? entry->isUndoAvailable() : writable && editor->send(SCI_CANUNDO));
    enable("redo", entry ? entry->isRedoAvailable() : writable && editor->send(SCI_CANREDO));
    enable("selectAll", resultsFocus || entry || editor);
    for (const auto &name : {"saveFile", "saveAs"})
        enable(name, documentEditor);
    enable("closeFile", documentsTabs_->currentWidget());
    for (const auto &name :
         {"firstReference", "nextReference", "previousReference"})
        enable(name, !referenceHits_.isEmpty());
    enable("jumpReference", editor || !referenceHits_.isEmpty());
    enable("showSearchResults", !referenceGroups_.isEmpty());
    enable("deleteSearchSymbol", showingReferenceOutline_ && outline_->currentItem());
    for (const auto &name :
         {"findText", "zoomIn", "zoomOut", "resetZoom", "jumpDefinition"})
        enable(name, editor);
    enable("lookupReferences", editor || outline_->hasFocus() || symbolList_->hasFocus() ||
                                  categoryTree_->hasFocus() || !projectPath_.isEmpty());
    enable("searchProject", !projectPath_.isEmpty() || editor);
    enable("findNext", editor && !searchText_.isEmpty());
    enable("findPrevious", editor && !searchText_.isEmpty());
    for (const auto &name :
         {"restartLanguageService", "compileCommandsDirectory", "workspaceExclusions",
          "closeProject", "refreshFiles", "rebuildSymbols", "rebuildAll"})
        enable(name, !projectPath_.isEmpty());
}

void MainWindow::showAppearance() {
    appearanceDialog_->show();
    appearanceDialog_->raise();
    appearanceDialog_->activateWindow();
}

void MainWindow::findText(bool backwards) {
    auto *editor = contextPreviewEditor_->hasFocus() ? contextPreviewEditor_ : activeEditor();
    if (!editor || searchText_.isEmpty()) {
        statusBar()->showMessage("Use Search > Find to enter search text.", 5000);
        return;
    }
    const auto needle = searchText_.toUtf8();
    const auto source = editor->bytes();
    const int start =
        static_cast<int>(editor->send(backwards ? SCI_GETSELECTIONSTART : SCI_GETSELECTIONEND));
    qsizetype match = backwards ? (start > 0 ? source.lastIndexOf(needle, start - 1) : -1)
                                : source.indexOf(needle, start);
    if (match < 0)
        match = backwards ? source.lastIndexOf(needle) : source.indexOf(needle);
    if (match < 0) {
        statusBar()->showMessage("Text not found: " + searchText_, 5000);
        return;
    }
    editor->selectRange(static_cast<int>(match), static_cast<int>(match + needle.size()));
    editor->setFocus();
}
