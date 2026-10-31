#include "MainWindow.h"
#include "UiAppearance.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>

void MainWindow::applyPanelConfiguration() {
    const QMap<QString, QTreeWidget *> trees{
        {"outline", outline_}, {"files", fileList_}, {"symbols", symbolList_}};
    for (auto it = trees.cbegin(); it != trees.cend(); ++it) {
        const auto &settings = configuration_.panels[it.key()];
        auto font = UiAppearance::uiFont(configuration_);
        if (!settings.fontFamily.isEmpty())
            font.setFamily(settings.fontFamily);
        if (settings.fontSizePx)
            font.setPixelSize(settings.fontSizePx);
        it.value()->setFont(font);
        const auto family = font.family().replace('\\', "\\\\").replace('"', "\\\"");
        QString style =
            QString("font-family:\"%1\";font-size:%2px;").arg(family).arg(font.pixelSize());
        if (settings.foreground.isValid())
            style += "color:" + settings.foreground.name() + ';';
        if (settings.background.isValid())
            style += "background-color:" + settings.background.name() + ';';
        it.value()->setStyleSheet(style.isEmpty() ? QString{} : "QTreeWidget {" + style + '}');
        it.value()->setHeaderHidden(!settings.headers);
        if (it.value()->columnCount() > 1)
            it.value()->setColumnHidden(1, !settings.paths);
        if (it.value()->columnCount() > 2)
            it.value()->setColumnHidden(2, !settings.sizes);
    }
    auto context = configuration_;
    context.editor.typography.pointSize *= configuration_.context.scale;
    contextPreviewEditor_->applyConfiguration(context);
    updateOutline();
    if (configuration_.context.tracking == "off") {
        contextPath_.clear();
        showContext("Context tracking is off", {}, {}, 0, 0);
    } else
        contextTimer_->start();
}

void MainWindow::showPanelOptions(const QString &panel) {
    auto *dialog = new QDialog(this);
    dialog->setObjectName(panel + "OptionsDialog");
    dialog->setWindowTitle(panel == "outline"   ? "Symbol Window Options"
                           : panel == "files"   ? "Project File List Options"
                           : panel == "symbols" ? "Project Symbol List Options"
                                                : "Context Window Options");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *form = new QFormLayout(dialog);
    auto draft = std::make_shared<AppSettings>(configuration_);
    auto check = [form, dialog](const QString &name, bool &value) {
        auto *box = new QCheckBox(name, dialog);
        box->setChecked(value);
        QObject::connect(box, &QCheckBox::toggled, dialog,
                         [&value](bool checked) { value = checked; });
        form->addRow(box);
    };
    if (panel == "context") {
        auto *tracking = new QComboBox(dialog);
        tracking->addItems({"selected", "enclosing", "off"});
        tracking->setCurrentText(draft->context.tracking);
        connect(tracking, &QComboBox::currentTextChanged, dialog,
                [draft](const QString &text) { draft->context.tracking = text; });
        form->addRow("Track", tracking);
        check("Track identifiers in comments", draft->context.comments);
        check("Track identifiers in strings", draft->context.strings);
        auto *scale = new QDoubleSpinBox(dialog);
        scale->setRange(0.5, 2);
        scale->setSingleStep(0.05);
        scale->setValue(draft->context.scale);
        connect(scale, &QDoubleSpinBox::valueChanged, dialog,
                [draft](double value) { draft->context.scale = value; });
        form->addRow("Source font scale", scale);
    } else {
        auto &settings = draft->panels[panel];
        auto *family = new QFontComboBox(dialog);
        family->setCurrentFont(settings.fontFamily.isEmpty() ? UiAppearance::uiFont(configuration_)
                                                             : QFont(settings.fontFamily));
        connect(
            family, &QFontComboBox::currentFontChanged, dialog,
            [draft, panel](const QFont &font) { draft->panels[panel].fontFamily = font.family(); });
        form->addRow("List font", family);
        auto *size = new QSpinBox(dialog);
        size->setRange(0, 32);
        size->setSpecialValueText("Inherit UI size");
        size->setValue(settings.fontSizePx);
        connect(size, &QSpinBox::valueChanged, dialog,
                [draft, panel](int value) { draft->panels[panel].fontSizePx = value; });
        form->addRow("Font size (px)", size);
        for (bool background : {false, true}) {
            auto *button = new QPushButton(background ? "Background..." : "Text color...", dialog);
            connect(button, &QPushButton::clicked, dialog, [draft, panel, background, dialog] {
                auto &color =
                    background ? draft->panels[panel].background : draft->panels[panel].foreground;
                const auto chosen = QColorDialog::getColor(color, dialog);
                if (chosen.isValid())
                    color = chosen;
            });
            form->addRow(button);
        }
        check("Show column headings", settings.headers);
        if (panel != "outline")
            check("Show file/path column", settings.paths);
        if (panel == "files")
            check("Show file sizes", settings.sizes);
        if (panel == "outline") {
            check("Show class/struct members", draft->outline.members);
            check("Show enum constants", draft->outline.enumerators);
            check("Show local declarations", draft->outline.locals);
            auto *length = new QSpinBox(dialog);
            length->setRange(0, 100000);
            length->setSpecialValueText("Do not emphasize long functions");
            length->setValue(draft->outline.longFunctionLines);
            connect(length, &QSpinBox::valueChanged, dialog,
                    [draft](int value) { draft->outline.longFunctionLines = value; });
            form->addRow("Bold functions at least (lines)", length);
        }
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                             QDialogButtonBox::RestoreDefaults,
                                         dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, dialog,
            [this, panel, dialog] {
                if (panel == "context")
                    configuration_.context = defaultAppSettings().context;
                else {
                    configuration_.panels[panel] = defaultAppSettings().panels[panel];
                    if (panel == "outline")
                        configuration_.outline = defaultAppSettings().outline;
                }
                configurationDirty_ = true;
                applyPanelConfiguration();
                if (persistSettings_)
                    saveConfiguration();
                dialog->accept();
            });
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, draft, panel, dialog] {
        if (panel == "context")
            configuration_.context = draft->context;
        else {
            configuration_.panels[panel] = draft->panels[panel];
            if (panel == "outline")
                configuration_.outline = draft->outline;
        }
        configurationDirty_ = true;
        applyPanelConfiguration();
        if (persistSettings_)
            saveConfiguration();
        dialog->accept();
    });
    dialog->open();
}
