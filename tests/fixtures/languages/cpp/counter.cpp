#include "counter.hpp"

namespace sample {
int Counter::increment(int amount) {
    value_ += amount;
    return value_;
}

int Counter::increment() {
    return increment(1);
}
}
