#include "AppSettings.h"
#include "Diagnostics.h"
#include "RuntimeLimits.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#include <stdexcept>

static void initializeSettingsResource() {
    Q_INIT_RESOURCE(resources);
}

namespace {
constexpr qint64 MaximumSettingsBytes = 256 * 1024;

const QJsonObject &defaultJson() {
    static const auto defaults = [] {
        initializeSettingsResource();
        QFile file(":/config/default-settings.json");
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Embedded configuration is unavailable.");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            qFatal("Embedded configuration is invalid JSON.");
        return document.object();
    }();
    return defaults;
}

[[noreturn]] void invalid(const QString &path, const QString &message) {
    throw std::invalid_argument((path + ": " + message).toStdString());
}

QJsonObject merge(const QJsonObject &defaults, const QJsonObject &override, const QString &prefix) {
    auto result = defaults;
    for (auto it = override.begin(); it != override.end(); ++it) {
        const auto path = prefix.isEmpty() ? it.key() : prefix + '.' + it.key();
        if (!defaults.contains(it.key()))
            invalid(path, "unknown setting");
        const auto expected = defaults.value(it.key());
        if (it.value().type() != expected.type())
            invalid(path, "incorrect JSON type");
        result[it.key()] = expected.isObject()
                               ? QJsonValue(merge(expected.toObject(), it.value().toObject(), path))
                               : it.value();
    }
    return result;
}

double number(const QJsonObject &object, const QString &name, double minimum, double maximum,
              const QString &prefix) {
    const double value = object.value(name).toDouble();
    if (!std::isfinite(value) || value < minimum || value > maximum)
        invalid(prefix + '.' + name,
                QString("must be between %1 and %2").arg(minimum).arg(maximum));
    return value;
}

int integer(const QJsonObject &object, const QString &name, int minimum, int maximum,
            const QString &prefix) {
    const auto value = number(object, name, minimum, maximum, prefix);
    if (value != std::floor(value))
        invalid(prefix + '.' + name, "must be an integer");
    return static_cast<int>(value);
}

QColor colorValue(const QJsonValue &value, const QString &path, bool inherit = false) {
    static const QRegularExpression rgb("^#[0-9a-fA-F]{6}$");
    const auto text = value.toString();
    if (inherit && text.isEmpty())
        return {};
    if (!rgb.match(text).hasMatch())
        invalid(path, "expected #RRGGBB");
    return QColor(text);
}

QString family(const QJsonObject &object, const QString &prefix) {
    const auto value = object.value("fontFamily").toString().trimmed();
    if (value.size() > 128)
        invalid(prefix + ".fontFamily", "font name is too long");
    for (const auto character : value)
        if (character.category() == QChar::Other_Control)
            invalid(prefix + ".fontFamily", "control characters are not allowed");
    return value;
}

template <size_t N> std::array<int, N> sizes(const QJsonObject &object, const QString &key) {
    const auto values = object.value(key).toArray();
    if (values.size() != N)
        invalid("layout." + key, QString("expected %1 sizes").arg(N));
    std::array<int, N> result{};
    for (size_t i = 0; i < N; ++i) {
        if (!values[static_cast<qsizetype>(i)].isDouble())
            invalid("layout." + key, "sizes must be integers");
        result[i] = integer({{"size", values[static_cast<qsizetype>(i)]}}, "size", 50, 10000,
                            "layout." + key);
    }
    return result;
}

ReferenceSettings referenceSettings(const QJsonObject &object, const QString &prefix) {
    ReferenceSettings result;
    result.semantic = object.value("semantic").toBool();
    result.scope = integer(object, "scope", 0, 2, prefix);
    result.declarations = object.value("declarations").toBool();
    result.comments = object.value("comments").toBool();
    result.strings = object.value("strings").toBool();
    const auto search = object.value("search").toObject();
    auto &s = result.search;
    s.method = static_cast<SearchMethod>(integer(search, "method", 0, 4, prefix + ".search"));
    s.caseSensitive = search.value("caseSensitive").toBool();
    s.wholeWords = search.value("wholeWords").toBool();
    s.comments = static_cast<CommentSearch>(integer(search, "comments", 0, 2, prefix + ".search"));
    s.strings = search.value("strings").toBool();
    s.skipInactive = search.value("skipInactive").toBool();
    s.smartMatching = search.value("smartMatching").toBool();
    s.contextLines = integer(search, "contextLines", 0, 1000, prefix + ".search");
    s.variations = search.value("variations").toBool();
    s.fragments = search.value("fragments").toBool();
    s.filePatterns = search.value("filePatterns").toString();
    if (s.filePatterns.size() > 4096)
        invalid(prefix + ".search.filePatterns", "must not exceed 4096 characters");
    const auto output = search.value("output").toObject();
    s.output = {output.value("container").toBool(), output.value("file").toBool(),
                output.value("line").toBool(), output.value("text").toBool(),
                output.value("links").toBool(), output.value("relativePaths").toBool()};
    return result;
}

QJsonObject referenceJson(const ReferenceSettings &r) {
    const auto &s = r.search;
    return {{"semantic", r.semantic}, {"scope", r.scope}, {"declarations", r.declarations},
            {"comments", r.comments}, {"strings", r.strings},
            {"search", QJsonObject{
                {"method", static_cast<int>(s.method)}, {"caseSensitive", s.caseSensitive},
                {"wholeWords", s.wholeWords}, {"comments", static_cast<int>(s.comments)},
                {"strings", s.strings}, {"skipInactive", s.skipInactive},
                {"smartMatching", s.smartMatching}, {"contextLines", s.contextLines},
                {"variations", s.variations}, {"fragments", s.fragments},
                {"filePatterns", s.filePatterns},
                {"output", QJsonObject{{"container", s.output.container}, {"file", s.output.file},
                    {"line", s.output.line}, {"text", s.output.text}, {"links", s.output.links},
                    {"relativePaths", s.output.relativePaths}}}}}};
}

AppSettings decode(const QJsonObject &root) {
    if (root.value("schemaVersion").toDouble() != 1)
        invalid("schemaVersion", "only version 1 is supported");
    AppSettings result;
    const auto ui = root.value("ui").toObject();
    result.ui = {family(ui, "ui"),
                 integer(ui, "fontSizePx", 8, 32, "ui"),
                 integer(ui, "rowHeight", 12, 64, "ui"),
                 integer(ui, "bandHeight", 18, 80, "ui"),
                 integer(ui, "iconSize", 12, 48, "ui"),
                 integer(ui, "symbolIconSize", 8, 32, "ui"),
                 integer(ui, "splitterWidth", 2, 16, "ui"),
                 integer(ui, "scrollbarWidth", 10, 40, "ui"),
                 integer(ui, "treeIndent", 8, 40, "ui")};
    const auto editor = root.value("editor").toObject();
    result.editor.typography = {
        family(editor, "editor"), number(editor, "fontSizePt", 6, 32, "editor"),
        number(editor, "declarationScale", 1, 1.8, "editor"), editor.value("uniformSizes").toBool(),
        editor.value("showWhitespace").toBool()};
    result.editor.lineNumbers = editor.value("showLineNumbers").toBool();
    result.editor.currentLine = editor.value("highlightCurrentLine").toBool();
    result.editor.tabWidth = integer(editor, "tabWidth", 1, 16, "editor");
    result.editor.indentWidth = integer(editor, "indentWidth", 1, 16, "editor");
    result.editor.useTabs = editor.value("useTabs").toBool();
    result.editor.extraAscent = integer(editor, "extraAscent", 0, 12, "editor");
    result.editor.extraDescent = integer(editor, "extraDescent", 0, 12, "editor");
    const auto layout = root.value("layout").toObject();
    result.layout = {integer(layout, "windowWidth", 900, 7680, "layout"),
                     integer(layout, "windowHeight", 500, 4320, "layout"),
                     sizes<3>(layout, "columns"), sizes<2>(layout, "rightPanes")};
    const auto behavior = root.value("behavior").toObject();
    result.behavior = {integer(behavior, "parseDebounceMs", 50, 2000, "behavior"),
                       integer(behavior, "contextDebounceMs", 50, 2000, "behavior"),
                       integer(behavior, "searchDebounceMs", 50, 2000, "behavior"),
                       integer(behavior, "workspaceRefreshMinutes", 1,
                               RuntimeLimits::MaximumWorkspaceRefreshMinutes, "behavior")};
    const auto panels = root.value("panels").toObject();
    for (auto it = panels.begin(); it != panels.end(); ++it) {
        const auto object = it.value().toObject();
        const auto prefix = "panels." + it.key();
        result.panels.insert(it.key(),
                             {family(object, prefix), integer(object, "fontSizePx", 0, 32, prefix),
                              colorValue(object.value("foreground"), prefix + ".foreground", true),
                              colorValue(object.value("background"), prefix + ".background", true),
                              object.value("headers").toBool(), object.value("paths").toBool(),
                              object.value("sizes").toBool()});
    }
    const auto outline = root.value("outline").toObject();
    result.outline = {outline.value("members").toBool(), outline.value("enumerators").toBool(),
                      outline.value("locals").toBool(),
                      integer(outline, "longFunctionLines", 0, 100000, "outline")};
    const auto context = root.value("context").toObject();
    result.context = {context.value("tracking").toString(), context.value("comments").toBool(),
                      context.value("strings").toBool(),
                      number(context, "scale", 0.5, 2.0, "context")};
    if (!QStringList{"selected", "enclosing", "off"}.contains(result.context.tracking))
        invalid("context.tracking", "expected selected, enclosing, or off");
    result.references = referenceSettings(root.value("references").toObject(), "references");
    result.searchProject = referenceSettings(root.value("searchProject").toObject(), "searchProject");
    const auto colors = root.value("colors").toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it)
        result.colors.insert(it.key(), colorValue(it.value(), "colors." + it.key()));
    const auto syntax = root.value("syntax").toObject();
    for (size_t i = 0; i < syntaxNames().size(); ++i) {
        const QString name = syntaxNames()[i];
        const auto style = syntax.value(name).toObject();
        const auto prefix = "syntax." + name;
        result.syntax[i] = {colorValue(style.value("foreground"), prefix + ".foreground"),
                            colorValue(style.value("background"), prefix + ".background", true),
                            family(style, prefix),
                            number(style, "scale", 0.5, 2.0, prefix),
                            style.value("bold").toBool(),
                            style.value("italic").toBool()};
    }
    return result;
}

QByteArray fingerprint(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
}

const std::array<const char *, static_cast<size_t>(TextStyle::Count)> &syntaxNames() {
    static const std::array<const char *, static_cast<size_t>(TextStyle::Count)> names{
        "plain",
        "comment",
        "keyword",
        "string",
        "number",
        "type",
        "function",
        "field",
        "preprocessor",
        "operator",
        "typeReference",
        "primitive",
        "parameter",
        "variable",
        "variableDeclaration",
        "constant",
        "functionReference",
        "libraryFunction",
        "punctuation"};
    return names;
}

QColor AppSettings::color(const QString &role) const {
    const auto found = colors.constFind(role);
    if (found == colors.cend())
        qFatal("Unknown application color role: %s", qPrintable(role));
    return *found;
}

SettingsResult parseSettings(const QByteArray &json) {
    if (json.size() > MaximumSettingsBytes)
        return {{}, "Configuration exceeds 256 KiB."};
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return {{},
                QString("JSON error at byte %1: %2")
                    .arg(parseError.offset)
                    .arg(parseError.errorString())};
    if (!document.isObject())
        return {{}, "Configuration must be a JSON object."};
    try {
        auto overrides = document.object();
        for (const auto &key : {QString("references"), QString("searchProject")}) {
            if (!overrides.value(key).isObject())
                continue;
            auto group = overrides.value(key).toObject();
            auto search = group.value("search").toObject();
            if (group.contains("semantic") && !search.contains("method"))
                search["method"] = group.value("semantic").toBool() ? 0 : 1;
            if (group.contains("comments") && !search.contains("comments"))
                search["comments"] = group.value("comments").toBool() ? 1 : 0;
            if (group.contains("strings") && !search.contains("strings"))
                search["strings"] = group.value("strings");
            // Leave malformed objects to the normal schema/type validation.
            if (!group.contains("search") || group.value("search").isObject())
                group["search"] = search;
            overrides[key] = group;
        }
        return {decode(merge(defaultJson(), overrides, {})), {}};
    } catch (const std::invalid_argument &error) {
        return {{}, QString::fromUtf8(error.what())};
    }
}

const AppSettings &defaultAppSettings() {
    static const auto settings = [] {
        const auto result = parseSettings("{}");
        if (!result.settings)
            qFatal("Invalid embedded defaults: %s", qPrintable(result.error));
        return *result.settings;
    }();
    return settings;
}

QJsonObject settingsToJson(const AppSettings &s) {
    QJsonObject colors, syntax, panels;
    for (auto it = s.panels.cbegin(); it != s.panels.cend(); ++it)
        panels[it.key()] = QJsonObject{
            {"fontFamily", it->fontFamily},
            {"fontSizePx", it->fontSizePx},
            {"foreground", it->foreground.isValid() ? it->foreground.name() : QString{}},
            {"background", it->background.isValid() ? it->background.name() : QString{}},
            {"headers", it->headers},
            {"paths", it->paths},
            {"sizes", it->sizes}};
    for (auto it = s.colors.begin(); it != s.colors.end(); ++it)
        colors[it.key()] = it.value().isValid() ? it.value().name() : QString{};
    for (size_t i = 0; i < s.syntax.size(); ++i) {
        const auto &style = s.syntax[i];
        syntax[syntaxNames()[i]] = QJsonObject{
            {"foreground", style.foreground.isValid() ? style.foreground.name() : QString{}},
            {"background", style.background.isValid() ? style.background.name() : QString{}},
            {"fontFamily", style.fontFamily},
            {"scale", style.scale},
            {"bold", style.bold},
            {"italic", style.italic}};
    }
    const auto &t = s.editor.typography;
    return {
        {"schemaVersion", 1},
        {"ui", QJsonObject{{"fontFamily", s.ui.fontFamily},
                           {"fontSizePx", s.ui.fontSizePx},
                           {"rowHeight", s.ui.rowHeight},
                           {"bandHeight", s.ui.bandHeight},
                           {"iconSize", s.ui.iconSize},
                           {"symbolIconSize", s.ui.symbolIconSize},
                           {"splitterWidth", s.ui.splitterWidth},
                           {"scrollbarWidth", s.ui.scrollbarWidth},
                           {"treeIndent", s.ui.treeIndent}}},
        {"editor", QJsonObject{{"fontFamily", t.family},
                               {"fontSizePt", t.pointSize},
                               {"declarationScale", t.declarationScale},
                               {"uniformSizes", t.uniform},
                               {"showWhitespace", t.whitespace},
                               {"showLineNumbers", s.editor.lineNumbers},
                               {"highlightCurrentLine", s.editor.currentLine},
                               {"tabWidth", s.editor.tabWidth},
                               {"indentWidth", s.editor.indentWidth},
                               {"useTabs", s.editor.useTabs},
                               {"extraAscent", s.editor.extraAscent},
                               {"extraDescent", s.editor.extraDescent}}},
        {"layout",
         QJsonObject{
             {"windowWidth", s.layout.width},
             {"windowHeight", s.layout.height},
             {"columns", QJsonArray{s.layout.columns[0], s.layout.columns[1], s.layout.columns[2]}},
             {"rightPanes", QJsonArray{s.layout.rightPanes[0], s.layout.rightPanes[1]}}}},
        {"behavior", QJsonObject{{"parseDebounceMs", s.behavior.parseDebounceMs},
                                 {"contextDebounceMs", s.behavior.contextDebounceMs},
                                 {"searchDebounceMs", s.behavior.searchDebounceMs},
                                 {"workspaceRefreshMinutes", s.behavior.workspaceRefreshMinutes}}},
        {"panels", panels},
        {"outline", QJsonObject{{"members", s.outline.members},
                                {"enumerators", s.outline.enumerators},
                                {"locals", s.outline.locals},
                                {"longFunctionLines", s.outline.longFunctionLines}}},
        {"context", QJsonObject{{"tracking", s.context.tracking},
                                {"comments", s.context.comments},
                                {"strings", s.context.strings},
                                {"scale", s.context.scale}}},
        {"references", referenceJson(s.references)},
        {"searchProject", referenceJson(s.searchProject)},
        {"colors", colors},
        {"syntax", syntax}};
}

QString defaultSettingsPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath("settings.json");
}

SettingsFile::SettingsFile(QString path)
    : path_(path.isEmpty() ? QString{} : QFileInfo(path).absoluteFilePath()) {
}

SettingsResult SettingsFile::load() {
    const auto fail = [&](const QString &error) -> SettingsResult {
        qCWarning(storageLog) << "storage.settings_read_failed" << "path=" << path_
                              << "error=" << error;
        return {{}, error};
    };
    loaded_ = false;
    existed_ = !path_.isEmpty() && QFileInfo::exists(path_);
    if (!existed_) {
        loaded_ = true;
        fingerprint_.clear();
        return {defaultAppSettings(), {}};
    }
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return fail(path_ + ": " + file.errorString());
    if (file.size() > MaximumSettingsBytes)
        return fail(path_ + ": configuration exceeds 256 KiB.");
    const auto bytes = file.read(MaximumSettingsBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(path_ + ": " + file.errorString());
    auto result = parseSettings(bytes);
    if (result.settings) {
        fingerprint_ = fingerprint(bytes);
        loaded_ = true;
    } else
        return fail(path_ + ": " + result.error);
    return result;
}

QString SettingsFile::save(const AppSettings &settings) {
    const auto fail = [&](const QString &error) {
        qCWarning(storageLog) << "storage.settings_write_failed" << "path=" << path_
                              << "error=" << error;
        return error;
    };
    if (path_.isEmpty())
        return fail("No configuration file is selected.");
    if (!loaded_)
        return fail("Reload and fix the configuration before saving; invalid files are never "
                    "overwritten automatically.");
    const auto bytes = QJsonDocument(settingsToJson(settings)).toJson();
    const auto validated = parseSettings(bytes);
    if (!validated.settings)
        return fail(validated.error);
    if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
        return fail("Cannot create the configuration directory.");
    QLockFile lock(path_ + ".lock");
    if (!lock.tryLock(0)) {
        if (lock.error() == QLockFile::LockFailedError)
            return fail("Another process is saving this configuration; retry after it finishes.");
        return fail("Cannot lock the configuration file; check directory permissions and available "
                    "storage.");
    }
    QFile current(path_);
    if (current.exists() != existed_)
        return fail("Configuration changed externally. Reload it before saving.");
    if (existed_) {
        if (!current.open(QIODevice::ReadOnly))
            return fail(current.errorString());
        if (current.size() > MaximumSettingsBytes)
            return fail("Configuration changed externally. Reload it before saving.");
        const auto existing = current.read(MaximumSettingsBytes + 1);
        if (current.error() != QFileDevice::NoError)
            return fail(current.errorString());
        if (existing.size() > MaximumSettingsBytes || fingerprint(existing) != fingerprint_)
            return fail("Configuration changed externally. Reload it before saving.");
        current.close();
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(path_ + ": " + file.errorString());
    existed_ = true;
    fingerprint_ = fingerprint(bytes);
    return {};
}
