// Mbed TLS platform hooks: its millisecond clock (MBEDTLS_PLATFORM_MS_TIME_ALT)
// from the Pico's monotonic timer.
#include "mbedtls/build_info.h"
#include "mbedtls/platform_time.h"
#include "pico/time.h"

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time());
}
