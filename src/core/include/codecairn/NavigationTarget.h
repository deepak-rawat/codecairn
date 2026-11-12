#pragma once

#include <codecairn/FileIO.h>
#include <codecairn/IndexTypes.h>
#include <codecairn/NavigationHistory.h>

struct DefinitionCandidates {
    QVector<IndexedSymbol> targets;
    int total = 0;
    bool complete = false;
    QString error;
};

struct ResolvedNavigation {
    int start = 0;
    int end = 0;
    QString error;
    bool stale = false;
};

ResolvedNavigation resolveNavigationTarget(const NavigationLocation &location,
                                           const QByteArray &source,
                                           const std::optional<IndexedSymbol> &indexedTarget = {});
QString sourceIdentifier(const QByteArray &source, int position);

struct IdentifierSelection {
    QString name;
    int start = 0;
    int end = 0;
};

IdentifierSelection contextIdentifier(const QByteArray &source, int position);
