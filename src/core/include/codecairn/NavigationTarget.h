#pragma once

#include <codecairn/FileIO.h>
#include <codecairn/IndexTypes.h>
#include <codecairn/SourceLocation.h>

struct ResolvedNavigation {
    int start = 0;
    int end = 0;
    QString error;
    bool stale = false;
};

ResolvedNavigation
resolveNavigationTarget(const codecairn::SourceLocation &location, const QByteArray &source,
                        const std::optional<codecairn::IndexedSymbol> &indexedTarget = {});
QString sourceIdentifier(const QByteArray &source, int position);

struct IdentifierSelection {
    QString name;
    int start = 0;
    int end = 0;
};

IdentifierSelection contextIdentifier(const QByteArray &source, int position);
