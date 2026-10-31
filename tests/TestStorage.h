#pragma once

#include "AppSettings.h"
#include "WorkspacePaths.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

class TestStorage {
public:
    TestStorage() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName({});
        QCoreApplication::setApplicationName(
            "CodeCairnTests-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        stateDirectory_ = defaultStateDirectory();
        settingsDirectory_ = QFileInfo(defaultSettingsPath()).absolutePath();
        const auto identity = QCoreApplication::applicationName();
        if (!QDir::isAbsolutePath(stateDirectory_) || !QDir::isAbsolutePath(settingsDirectory_) ||
            !stateDirectory_.contains(identity) || !settingsDirectory_.contains(identity))
            qFatal("Cannot isolate application storage for this test run.");
    }
    ~TestStorage() {
        for (const auto &path : {stateDirectory_, settingsDirectory_})
            if (QDir(path).exists() && !QDir(path).removeRecursively())
                qWarning("Could not remove isolated test storage: %s", qPrintable(path));
    }

private:
    QString stateDirectory_;
    QString settingsDirectory_;
};
