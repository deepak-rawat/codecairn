#pragma once

#include <QFont>
#include <QIcon>
#include <QString>
#include "AppSettings.h"

class QTreeWidget;
class QTreeView;
class QImage;
class QPaintDevice;

namespace UiAppearance {
QFont uiFont(const AppSettings &settings = defaultAppSettings());
int bandHeight(const AppSettings &settings);
QString sourceFont();
double nativeSourcePointSize(double referencePoints, const QPaintDevice &device);
QString styleSheet(const AppSettings &settings = defaultAppSettings());
QIcon icon(const QString &kind);
QIcon applicationIcon();
QImage referenceLinkImage(int size, bool hovered = false);
void configureTree(QTreeWidget *tree, const QStringList &headers,
                   const AppSettings &settings = defaultAppSettings());
void applyTreeAppearance(QTreeWidget *tree, const AppSettings &settings);
void configureTree(QTreeView *tree, const AppSettings &settings = defaultAppSettings());
void applyTreeAppearance(QTreeView *tree, const AppSettings &settings);
}
