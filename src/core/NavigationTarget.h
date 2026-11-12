#pragma once

#include "FileIO.h"
#include "IndexTypes.h"
#include "NavigationHistory.h"

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
