#pragma once
#include "SearchOptions.h"

#include "SourceAnalysis.h"
#include <QColor>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <array>
#include <optional>

struct Typography {
    QString family;
    double pointSize = 0;
    double declarationScale = 0;
    bool uniform = false;
    bool whitespace = false;
    bool operator==(const Typography &) const = default;
};

struct UiSettings {
    QString fontFamily;
    int fontSizePx = 0;
    int rowHeight = 0;
    int bandHeight = 0;
    int iconSize = 0;
    int symbolIconSize = 0;
    int splitterWidth = 0;
    int scrollbarWidth = 0;
    int treeIndent = 0;
};

struct EditorSettings {
    Typography typography;
    bool lineNumbers = false;
    bool currentLine = false;
    int tabWidth = 0;
    int indentWidth = 0;
    bool useTabs = false;
    int extraAscent = 0;
    int extraDescent = 0;
    bool operator==(const EditorSettings &) const = default;
};

struct SyntaxSettings {
    QColor foreground;
    QColor background;
    QString fontFamily;
    double scale = 0;
    bool bold = false;
    bool italic = false;
};

struct LayoutSettings {
    int width = 0;
    int height = 0;
    std::array<int, 3> columns{};
    std::array<int, 2> rightPanes{};
};

struct BehaviorSettings {
    int parseDebounceMs = 0;
    int contextDebounceMs = 0;
    int searchDebounceMs = 0;
    int workspaceRefreshMinutes = 0;
};

struct PanelSettings {
    QString fontFamily;
    int fontSizePx = 0;
    QColor foreground;
    QColor background;
    bool headers = true;
    bool paths = true;
    bool sizes = true;
};

struct OutlineSettings {
    bool members = true;
    bool enumerators = true;
    bool locals = false;
    int longFunctionLines = 50;
};

struct ContextSettings {
    QString tracking = "selected";
    bool comments = false;
    bool strings = false;
    double scale = 1;
};

struct ReferenceSettings {
    bool semantic = true;
    int scope = 0;
    bool declarations = true;
    bool comments = false;
    bool strings = false;
    SearchOptions search;
};

struct AppSettings {
    UiSettings ui;
    EditorSettings editor;
    LayoutSettings layout;
    BehaviorSettings behavior;
    QMap<QString, PanelSettings> panels;
    OutlineSettings outline;
    ContextSettings context;
    ReferenceSettings references;
    ReferenceSettings searchProject;
    QMap<QString, QColor> colors;
    std::array<SyntaxSettings, static_cast<size_t>(TextStyle::Count)> syntax{};

    QColor color(const QString &role) const;
};

struct SettingsResult {
    std::optional<AppSettings> settings;
    QString error;
};

const AppSettings &defaultAppSettings();
const std::array<const char *, static_cast<size_t>(TextStyle::Count)> &syntaxNames();
SettingsResult parseSettings(const QByteArray &json);
QJsonObject settingsToJson(const AppSettings &settings);
QString defaultSettingsPath();

class SettingsFile {
public:
    explicit SettingsFile(QString path = {});
    QString path() const { return path_; }
    SettingsResult load();
    QString save(const AppSettings &settings);
    bool existed() const { return existed_; }

private:
    QString path_;
    QByteArray fingerprint_;
    bool existed_ = false;
    bool loaded_ = false;
};
