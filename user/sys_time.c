#include "c_types.h"
#include "osapi.h"

typedef union _timer {
uint32_t time_s[2];
uint64_t time_l;
} long_time_t;

static long_time_t time;
static uint32_t old;

uint64_t ICACHE_FLASH_ATTR get_long_systime() {
	uint32_t now = system_get_time();
	if (now < old) {
		time.time_s[1]++;
	}
	old = now;
	time.time_s[0] = now;
	return time.time_l;
}

uint64_t ICACHE_FLASH_ATTR get_low_systime() {
	get_long_systime();
	return time.time_s[0];
}

void init_long_systime() {
	old = system_get_time();
	time.time_l = (uint64_t)old;
}


