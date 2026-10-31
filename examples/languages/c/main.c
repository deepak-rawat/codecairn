#include "api.h"

int main(void) {
    Counter counter = {10};
    reset_counter(&counter);
    return increment(&counter) == 1 ? 0 : 1;
}
