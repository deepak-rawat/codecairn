#include "NavigationHistory.h"
#include "RuntimeLimits.h"

void NavigationHistory::record(const NavigationLocation &origin,
                               const NavigationLocation &destination) {
    if (destination.path.isEmpty())
        return;
    const auto samePlace = [](const auto &a, const auto &b) {
        return a.path == b.path && a.start == b.start && a.end == b.end;
    };
    if (!origin.path.isEmpty()) {
        if (position_ >= 0 && samePlace(entries_[position_], origin))
            entries_[position_] = origin;
        else {
            entries_.resize(position_ + 1);
            entries_.append(origin);
            ++position_;
        }
    }
    if (position_ >= 0 && samePlace(entries_[position_], destination))
        return;
    entries_.resize(position_ + 1);
    entries_.append(destination);
    if (entries_.size() > RuntimeLimits::NavigationHistoryEntries)
        entries_.removeFirst();
    position_ = static_cast<int>(entries_.size()) - 1;
}

NavigationLocation NavigationHistory::move(int direction) {
    if ((direction < 0 && !canBack()) || (direction > 0 && !canForward()))
        return {};
    position_ += direction < 0 ? -1 : 1;
    return entries_[position_];
}
