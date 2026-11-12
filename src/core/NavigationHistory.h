#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

struct NavigationLocation {
    QString path;
    int start = 0;
    int end = 0;
    int firstVisibleLine = -1;
    QByteArray expectedHash{};
    int line = -1;
    int character = 0;
    int endLine = -1;
    int endCharacter = 0;
    bool wholeLine = false;
    bool operator==(const NavigationLocation &) const = default;
};

class NavigationHistory {
public:
    void record(const NavigationLocation &origin, const NavigationLocation &destination);
    bool canBack() const { return position_ > 0; }
    bool canForward() const { return position_ + 1 < entries_.size(); }
    NavigationLocation move(int direction);

private:
    QVector<NavigationLocation> entries_;
    int position_ = -1;
};
