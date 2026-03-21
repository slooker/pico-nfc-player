#include "mbedtls/platform_time.h"
#include "pico/time.h"

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(time_us_64() / 1000);
}
