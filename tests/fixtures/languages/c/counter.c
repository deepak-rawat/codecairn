#include "api.h"

static _Thread_local Counter *current_counter;

int increment(Counter *counter) {
    current_counter = counter;
    return ++counter->value;
}

#define DEFINE_RESET(name) void reset_##name(Counter *counter) { counter->value = 0; }
DEFINE_RESET(counter)
