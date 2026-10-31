#include "MainWindow.h"
#include "UiAppearance.h"

#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>

namespace {
QString sourceFamily(const AppSettings &settings) {
    const auto &name = settings.editor.typography.family;
    return name.isEmpty() ? UiAppearance::sourceFont() : name;
}

int syntaxColorIndex(const QString &role) {
    for (size_t i = 0; i < syntaxNames().size(); ++i)
        if (role == "syntax." + QString::fromLatin1(syntaxNames()[i]))
            return static_cast<int>(i);
    return -1;
}
}

void MainWindow::syncConfigurationControls() {
    const QSignalBlocker b1(sourceFontComboBox_), b2(sourceFontSizeSpinBox_),
        b3(declarationScaleSpinBox_), b4(uniformSizesCheckBox_), b5(whitespaceCheckBox_),
        b6(lineNumbers_), b7(uiFontComboBox_), b8(uiFontSizeSpinBox_), b9(currentLine_);
    const auto &editor = configuration_.editor;
    sourceFontComboBox_->setCurrentFont(QFont(sourceFamily(configuration_)));
    sourceFontSizeSpinBox_->setValue(editor.typography.pointSize);
    declarationScaleSpinBox_->setValue(editor.typography.declarationScale);
    uniformSizesCheckBox_->setChecked(editor.typography.uniform);
    whitespaceCheckBox_->setChecked(editor.typography.whitespace);
    currentLine_->setChecked(editor.currentLine);
    lineNumbers_->setChecked(editor.lineNumbers);
    uiFontComboBox_->setCurrentFont(UiAppearance::uiFont(configuration_));
    uiFontSizeSpinBox_->setValue(configuration_.ui.fontSizePx);
    declarationScaleSpinBox_->setEnabled(!editor.typography.uniform);
    updateColorButton();
}

void MainWindow::applyConfiguration() {
    const QSignalBlocker sourceFontSignals(sourceFontComboBox_), uiFontSignals(uiFontComboBox_),
        uiSizeSignals(uiFontSizeSpinBox_);
    setUpdatesEnabled(false);
    setFont(UiAppearance::uiFont(configuration_));
    setStyleSheet(UiAppearance::styleSheet(configuration_));
    const int band = UiAppearance::bandHeight(configuration_);
    documentBar_->setFixedHeight(band);
    outlineTitle_->setFixedHeight(band);
    contextTitle_->setFixedHeight(band);
    definitionChoices_->setFixedHeight(band);
    for (auto *entry : {outlineFilter_, fileFilter_, symbolFilter_})
        entry->setFixedHeight(band);
    for (auto *bar : findChildren<QToolBar *>()) {
        bar->setFixedHeight(band);
        bar->setIconSize(QSize(configuration_.ui.iconSize, configuration_.ui.iconSize));
    }
    for (auto *tree : {outline_, fileList_, symbolList_, folderTree_, categoryTree_})
        UiAppearance::applyTreeAppearance(tree, configuration_);
    if (referencesList_)
        applyReferenceAppearance();
    mainSplitter_->setHandleWidth(configuration_.ui.splitterWidth);
    rightSplitter_->setHandleWidth(configuration_.ui.splitterWidth);
    statusBar()->setFixedHeight(band + 1);
    for (const auto &doc : documents_) {
        doc->editor->applyConfiguration(configuration_);
        doc->setAnalysisDebounceMs(configuration_.behavior.parseDebounceMs);
    }
    contextPreviewEditor_->applyConfiguration(configuration_);
    contextTimer_->setInterval(configuration_.behavior.contextDebounceMs);
    hoverTimer_->setInterval(configuration_.behavior.contextDebounceMs);
    symbolQueryTimer_->setInterval(configuration_.behavior.searchDebounceMs);
    workspaceIndex_->setRefreshIntervalMinutes(configuration_.behavior.workspaceRefreshMinutes);
    syncConfigurationControls();
    applyPanelConfiguration();
    setUpdatesEnabled(true);
    updateStatus();
}

void MainWindow::resetConfiguredLayout() {
    const auto &layout = configuration_.layout;
    mainSplitter_->setSizes({layout.columns[0], layout.columns[1], layout.columns[2]});
    rightSplitter_->setSizes({layout.rightPanes[0], layout.rightPanes[1]});
}

bool MainWindow::reloadConfiguration() {
    auto result = configurationFile_.load();
    if (!result.settings) {
        configurationError_ = result.error;
        reportError("Configuration reload failed; current settings retained. " + result.error);
        return false;
    }
    configuration_ = *result.settings;
    configurationDirty_ = false;
    configurationError_.clear();
    applyConfiguration();
    statusBar()->showMessage("Loaded UI configuration: " + configurationFile_.path(), 6000);
    return true;
}

bool MainWindow::saveConfiguration() {
    const auto error = configurationFile_.save(configuration_);
    if (!error.isEmpty()) {
        configurationError_ = error;
        reportError("Cannot save UI configuration: " + error);
        return false;
    }
    configurationError_.clear();
    configurationDirty_ = false;
    statusBar()->showMessage("Saved UI configuration: " + configurationFile_.path(), 6000);
    return true;
}

void MainWindow::changeUiTypography() {
    const auto selectedFamily = uiFontComboBox_->currentFont().family();
    auto &ui = configuration_.ui;
    if (!ui.fontFamily.isEmpty() ||
        selectedFamily != UiAppearance::uiFont(defaultAppSettings()).family())
        ui.fontFamily = selectedFamily;
    ui.fontSizePx = uiFontSizeSpinBox_->value();
    configurationDirty_ = true;
    applyConfiguration();
}

void MainWindow::updateColorButton() {
    const auto role = colorRole_->currentData().toString();
    const int index = syntaxColorIndex(role);
    const auto color = index >= 0 ? configuration_.syntax[static_cast<size_t>(index)].foreground
                                  : configuration_.color(role);
    colorButton_->setText(color.name());
    colorButton_->setToolTip("Change " + role);
    colorButton_->setStyleSheet(
        QString("background: %1; color: %2; padding: 4px;")
            .arg(color.name(), color.lightness() < 128 ? "#ffffff" : "#000000"));
}

void MainWindow::editConfigurationColor() {
    const auto role = colorRole_->currentData().toString();
    const int index = syntaxColorIndex(role);
    const auto previous = index >= 0 ? configuration_.syntax[static_cast<size_t>(index)].foreground
                                     : configuration_.color(role);
    const auto selected = QColorDialog::getColor(previous, this, "Color: " + role);
    if (!selected.isValid())
        return;
    if (index >= 0)
        configuration_.syntax[static_cast<size_t>(index)].foreground = selected;
    else
        configuration_.colors[role] = selected;
    configurationDirty_ = true;
    applyConfiguration();
}
