#include "counter.hpp"

int main() {
    sample::Counter counter;
    return counter.increment() == 1 && counter.increment(2) == 3 ? 0 : 1;
}
