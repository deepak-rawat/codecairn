#pragma once

#include <codecairn/Engine.h>
#include <QHash>

namespace codecairn::detail {

class ProjectAnalysisCache {
public:
    void store(const AnalysisResult &result) {
        const auto previous = entries_.constFind(result.source.path);
        if (previous != entries_.cend() && previous->source.revision == result.source.revision &&
            previous->source.hash == result.source.hash && !previous->analysis.styles.isEmpty() &&
            result.analysis.styles.isEmpty())
            return;
        entries_.insert(result.source.path, result);
    }
    codecairn::SourceAnalysis find(const SourceSnapshot &source) const {
        const auto entry = entries_.constFind(source.path);
        if (entry != entries_.cend() && entry->source.revision == source.revision &&
            entry->source.hash == source.hash)
            return entry->analysis;
        return {};
    }
    void forget(const QString &path) { entries_.remove(path); }
    void clear() { entries_.clear(); }

private:
    QHash<QString, AnalysisResult> entries_;
};

}
