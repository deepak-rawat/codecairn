#include "ReferenceSearch.h"
#include "WorkspacePaths.h"
#include <QDir>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void fields(const QJsonObject &object, const QStringList &allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::invalid_argument("Unknown reference history field: " + it.key().toStdString());
}
QString text(const QJsonObject &object, const QString &key, int maximum) {
    const auto value = object.value(key);
    if (!value.isString() || value.toString().size() > maximum || value.toString().contains(QChar::Null))
        throw std::invalid_argument("Invalid reference history string: " + key.toStdString());
    return value.toString();
}
bool flag(const QJsonObject &object, const QString &key) {
    if (!object.value(key).isBool())
        throw std::invalid_argument("Invalid reference history flag: " + key.toStdString());
    return object.value(key).toBool();
}
int number(const QJsonObject &object, const QString &key) {
    const auto value = object.value(key);
    const auto n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || n < 0 ||
        n > std::numeric_limits<int>::max() || std::floor(n) != n)
        throw std::invalid_argument("Invalid reference history offset: " + key.toStdString());
    return static_cast<int>(n);
}
}

QJsonArray referenceHistoryJson(const QVector<ReferenceGroup> &groups) {
    QJsonArray json;
    for (const auto &group : groups) {
        QJsonArray hits;
        for (const auto &hit : group.hits)
            hits.append(QJsonObject{{"path", hit.location.path}, {"start", hit.location.start},
                {"end", hit.location.end}, {"hash", QString::fromLatin1(hit.location.expectedHash)},
                {"text", hit.text}, {"line", hit.line}, {"container", hit.container},
                {"stale", hit.stale}, {"linked", hit.linked}});
        const auto &o = group.output;
        json.append(QJsonObject{{"title", group.title}, {"hits", hits}, {"query", group.query},
            {"output", QJsonObject{{"container", o.container}, {"file", o.file}, {"line", o.line},
                {"text", o.text}, {"links", o.links}, {"relativePaths", o.relativePaths}}}});
    }
    return json;
}

QVector<ReferenceGroup> parseReferenceHistory(const QJsonArray &json, const QString &root,
                                             QString &error) {
    QVector<ReferenceGroup> groups;
    error.clear();
    try {
        if (json.size() > 1000)
            throw std::invalid_argument("Reference history exceeds 1000 groups.");
        int total = 0;
        static const QRegularExpression hash("^[0-9a-f]{64}$");
        for (const auto &value : json) {
            if (!value.isObject())
                throw std::invalid_argument("Reference history group must be an object.");
            const auto object = value.toObject();
            fields(object, {"title", "hits", "output", "query"});
            ReferenceGroup group;
            group.title = text(object, "title", 8192);
            if (object.contains("query"))
                group.query = text(object, "query", 4096);
            if (!object.value("output").isObject() || !object.value("hits").isArray())
                throw std::invalid_argument("Invalid reference history output or hits.");
            const auto o = object.value("output").toObject();
            fields(o, {"container", "file", "line", "text", "links", "relativePaths"});
            group.output = {flag(o, "container"), flag(o, "file"), flag(o, "line"),
                            flag(o, "text"), flag(o, "links"), flag(o, "relativePaths")};
            for (const auto &entry : object.value("hits").toArray()) {
                if (++total > 100000 || !entry.isObject())
                    throw std::invalid_argument("Invalid or oversized reference history.");
                const auto h = entry.toObject();
                fields(h, {"path", "start", "end", "hash", "text", "line", "container", "stale", "linked"});
                ReferenceHit hit;
                hit.location.path = text(h, "path", 32768);
                if (!QDir::isAbsolutePath(hit.location.path) || !pathInWorkspace(hit.location.path, root))
                    throw std::invalid_argument("Reference history target is outside its workspace.");
                hit.location.start = number(h, "start");
                hit.location.end = number(h, "end");
                if (hit.location.end < hit.location.start)
                    throw std::invalid_argument("Reference history range is reversed.");
                hit.location.expectedHash = text(h, "hash", 64).toLatin1();
                if (!hash.match(QString::fromLatin1(hit.location.expectedHash)).hasMatch())
                    throw std::invalid_argument("Invalid reference history source hash.");
                hit.text = text(h, "text", 4096);
                hit.line = number(h, "line");
                hit.container = text(h, "container", 4096);
                hit.stale = flag(h, "stale");
                hit.linked = flag(h, "linked");
                group.hits.append(hit);
            }
            groups.append(group);
        }
    } catch (const std::invalid_argument &failure) {
        error = QString::fromUtf8(failure.what());
        return {};
    }
    return groups;
}

QString exportReferenceHistory(const QString &path, const QVector<ReferenceGroup> &groups,
                               const QString &root) {
    QByteArray bytes;
    for (const auto &group : groups) {
        bytes += group.title.toUtf8() + '\n';
        const auto &o = group.output;
        for (const auto &hit : group.hits) {
            QStringList columns;
            if (o.container)
                columns << (hit.container.isEmpty() ? "<global>" : hit.container);
            if (o.file)
                columns << (o.relativePaths && !root.isEmpty()
                                ? QDir(root).relativeFilePath(hit.location.path) : hit.location.path);
            if (o.line)
                columns << QString::number(hit.line + 1);
            if (o.text)
                columns << hit.text;
            bytes += columns.join('\t').toUtf8() + '\n';
        }
        bytes += '\n';
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return path + ": " + file.errorString();
    return {};
}
