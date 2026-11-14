#pragma once

#include <codecairn/Result.h>
#include <QSet>

namespace codecairn::detail {

class PendingRequests {
public:
    void add(RequestId request) {
        if (request)
            active_.insert(request.value);
    }
    bool contains(RequestId request) const { return active_.contains(request.value); }
    bool finish(RequestId request) { return active_.remove(request.value); }
    void cancel(RequestId request) { active_.remove(request.value); }
    void clear() { active_.clear(); }

private:
    QSet<quint64> active_;
};

}
