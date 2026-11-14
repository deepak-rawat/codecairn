#pragma once

#include <codecairn/Project.h>
#include <QHash>

namespace codecairn::detail {

struct ContextChange {
    bool identity = false;
    bool analysis = false;
};

class ProjectContext {
public:
    const ProjectInfo &info() const { return info_; }
    quint64 generation() const { return generation_; }
    const QString &analysisDirectory() const { return analysisDirectory_; }
    Result<ContextChange> open(const PreparedProject &prepared);
    Result<ContextChange> reload();
    bool includes(const QString &identity, bool cache) const;
    void clear();
    void invalidateAnalysis() { ++generation_; }
    void clearMembership() { membership_.clear(); }
    void forgetDocument(const QString &path) { membership_.remove(path); }
    Result<void> setExcludedDirectories(const QStringList &names);

private:
    ProjectInfo info_;
    QString analysisDirectory_;
    quint64 generation_ = 0;
    mutable QHash<QString, bool> membership_;
};

}
