/*
 * Chip RTC/LP watchdog for a SimpleOTA trial.
 *
 * simpleota_init arms it while the running image is still pending
 * verification, for the confirm timeout plus a margin (the larger of
 * 60 seconds and 10 percent). The deadline uses the calibrated slow
 * clock. Confirm and the component's own rollback both disable it.
 * Other boots do not touch it.
 */

#include "sdkconfig.h"

#include "sota_wdt.h"

#include "esp_log.h"

static const char *TAG = "simpleota";

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && !CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG
#define SOTA_WDT_WANTED 1
#else
#define SOTA_WDT_WANTED 0
#endif

#if SOTA_WDT_WANTED && __has_include("hal/wdt_hal.h")
#define SOTA_WDT_HW 1
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define SOTA_HAVE_SLOWCLK_CAL 1
#else
#define SOTA_HAVE_SLOWCLK_CAL 0
#endif
#else
#define SOTA_WDT_HW 0
#endif

static bool s_armed;

#if SOTA_WDT_HW
static uint32_t sota_wdt_ticks(uint32_t confirm_sec)
{
    uint32_t margin = sota_wdt_margin_sec(confirm_sec, SOTA_TRIAL_WDT_MARGIN_S);
    uint64_t total_sec = (uint64_t)confirm_sec + (uint64_t)margin;
    uint64_t us = total_sec * 1000000ull;
#if SOTA_HAVE_SLOWCLK_CAL
    uint32_t cal = esp_clk_slowclk_cal_get();
    uint64_t calibrated = sota_slowclk_ticks(us, cal);
    if (calibrated != 0) {
        if (calibrated > 0xFFFFFFFFu) return 0xFFFFFFFFu;
        if (calibrated < 2) return 2;
        return (uint32_t)calibrated;
    }
#else
    (void)us;
#endif
    return sota_nominal_slow_ticks(total_sec, rtc_clk_slow_freq_get_hz());
}

static void sota_wdt_hw_off(void)
{
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_disable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = false;
}
#endif

bool sota_wdt_arm(uint32_t confirm_sec)
{
#if SOTA_WDT_HW
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_config_stage(&hal, WDT_STAGE0, sota_wdt_ticks(confirm_sec),
                         WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_enable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = true;
    return true;
#else
    (void)confirm_sec;
#if SOTA_WDT_WANTED
    ESP_LOGW(TAG,
             "chip watchdog is not running; a hang during this trial "
             "is not reset by the component");
#endif
    s_armed = false;
    return false;
#endif
}

void sota_wdt_disarm(void)
{
#if SOTA_WDT_HW
    if (!s_armed) return;
    sota_wdt_hw_off();
#else
    s_armed = false;
#endif
}
