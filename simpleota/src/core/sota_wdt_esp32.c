/**
 * sota_wdt_esp32.c - the chip watchdog that resets a hung trial
 * (sota_wdt_calc.h), on wdt_hal. C, not C++: hal/wdt_hal.h does not
 * compile as C++ on the S2 and the C3 in Arduino-ESP32 2.0.17.
 *
 * Its reset is undone by bootloader rollback, or, without it, by the
 * engine's rollback of a trial that boots after a watchdog. The clients'
 * opt-outs are read here, because an Arduino library cannot pass its own
 * defines to this file.
 */

#include "sdkconfig.h"
#include "sota_sys.h"

#include "sota_wdt_calc.h"

#if !defined(CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG) && \
    !defined(SIMPLEOTA_DISABLE_TRIAL_WATCHDOG) && __has_include("hal/wdt_hal.h")
#define SOTA_WDT_HW 1
#include "esp_idf_version.h"
#include "esp_sleep.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define SOTA_SLOWCLK_CAL() esp_clk_slowclk_cal_get()
#else
#define SOTA_SLOWCLK_CAL() 0u
#endif
/* esp_deep_sleep_register_hook() is ESP-IDF 5.1 and later (Arduino 3.x). */
#define SOTA_DEEP_SLEEP_HOOK (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0))
#else
#define SOTA_WDT_HW 0
#endif

#if SOTA_WDT_HW
static bool s_armed;

static void hw_off(void)
{
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_disable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = false;
}

#if SOTA_DEEP_SLEEP_HOOK
/* A long sleep must not end in a watchdog reset. Waking before the trial
 * was confirmed rolls back anyway (SOTA_BOOT_TRIAL_RESET). */
static void before_deep_sleep(void)
{
    sota_sys_wdt_disarm();
}
#endif
#endif /* SOTA_WDT_HW */

bool sota_sys_wdt_arm(uint32_t confirm_s, uint32_t floor_s)
{
    bool clamped = false;
#if SOTA_WDT_HW
    uint32_t ticks = sota_wdt_stage_ticks(confirm_s, floor_s, SOTA_SLOWCLK_CAL(),
                                          rtc_clk_slow_freq_get_hz(), &clamped);
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_config_stage(&hal, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_enable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = true;
#if SOTA_DEEP_SLEEP_HOOK
    /* Registering the same hook again is a no-op. */
    (void)esp_deep_sleep_register_hook(&before_deep_sleep);
#endif
#else
    (void)confirm_s;
    (void)floor_s;
#endif
    return clamped;
}

void sota_sys_wdt_disarm(void)
{
#if SOTA_WDT_HW
    if (s_armed) hw_off();
#endif
}

/* With the bootloader watchdog on (the default), the bootloader reprograms
 * the RTC watchdog on every reset and the application owns it afterwards,
 * so only one this boot armed is stopped. */
void sota_sys_wdt_quiesce(void)
{
#if SOTA_WDT_HW && !defined(CONFIG_BOOTLOADER_WDT_ENABLE)
    hw_off();
#else
    sota_sys_wdt_disarm();
#endif
}
