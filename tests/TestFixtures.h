#pragma once
#include "MainWindow.h"
#include <QFile>

inline bool openTypographyFixture(MainWindow &window) {
    const QString path = ":/fixtures/typography.cpp";
    if (!QFile::exists(path)) {
        qWarning("The test-only typography resource is missing.");
        return false;
    }
    return window.openFile(path);
}
