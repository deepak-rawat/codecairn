#pragma once

namespace sample {
class Counter {
public:
    int increment(int amount);
    int increment();
private:
    int value_ = 0;
};
}
