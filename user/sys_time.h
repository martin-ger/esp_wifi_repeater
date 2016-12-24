#include "c_types.h"

// returns time until boot in us
uint64_t get_long_systime();

// returns lower half of time until boot in us
uint64_t get_low_systime();

// initializes the timer
void init_long_systime();


