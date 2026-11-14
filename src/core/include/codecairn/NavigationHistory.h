#pragma once
#include <codecairn/SourceLocation.h>
#include <QVector>

class NavigationHistory {
public:
    void record(const codecairn::SourceLocation &origin,
                const codecairn::SourceLocation &destination);
    bool canBack() const { return position_ > 0; }
    bool canForward() const { return position_ + 1 < entries_.size(); }
    codecairn::SourceLocation move(int direction);

private:
    QVector<codecairn::SourceLocation> entries_;
    int position_ = -1;
};
