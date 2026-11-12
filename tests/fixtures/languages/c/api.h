#pragma once
typedef struct CounterTag { int value; } Counter;
int increment(Counter *counter);
void reset_counter(Counter *counter);
