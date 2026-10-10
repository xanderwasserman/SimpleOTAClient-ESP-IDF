# SimpleOTA client for ESP-IDF

First-party ESP-IDF component for the [SimpleOTA](https://simpleota.com)
firmware update platform. Polls for updates, streams and flashes offered
firmware with on-the-fly SHA-256 and Ed25519 signature verification, reports
the full update lifecycle to the dashboard, and integrates with the ESP-IDF
bootloader's app-rollback support so a bad build backs itself out.

The sibling of [SimpleOTAClient-Arduino](https://github.com/xanderwasserman/SimpleOTAClient-Arduino):
both clients implement the same device API contract and the same signed
firmware semantics, so mixed fleets behave identically. A device keeps its
identity (Wi-Fi MAC device id) across a migration between the two.

Local OTA (ArduinoOTA, espota, ElegantOTA) is for a board on your desk. SimpleOTA is managed fleet OTA for ESP32 when those devices are in the field, without pulling you into a full IoT platform.

You upload a .bin from PlatformIO, ESP-IDF, Arduino, or CI, and devices check in on their own. Eligible ones get a short-lived pre-signed URL and pull firmware straight from object storage. Rollouts can start at 5% with deterministic bucketing, so the same unit stays in the same cohort. Builds can be Ed25519-signed and verified on the device during download, before the new partition is marked bootable. After the reboot, the new firmware runs on trial until the first successful check-in, or until your code calls `simpleota_confirm()` when manual confirm is on. A crash or a power loss before that returns the board to the previous firmware, and if the confirm time runs out the component switches back itself. If the new firmware hangs during that trial, a watchdog on the chip's slow clock resets the board so the bootloader can still switch back.

Signing here protects the delivery path. It is not Secure Boot. They stack if you already use Secure Boot.

[Quickstart (project, client, first update)](https://wiki.simpleota.com/getting-started/?utm_source=idf_registry&utm_medium=readme&utm_campaign=signup_v1)
[Site](https://simpleota.com/?utm_source=idf_registry&utm_medium=readme&utm_campaign=signup_v1)

## Install

```sh
idf.py add-dependency "xanderwasserman/simpleota"
```

## Requirements

- ESP-IDF **v5.1 or newer** (tested through IDF 6.x). Targets: esp32, esp32s2,
  esp32s3, esp32c3, esp32c6, esp32h2.
- A partition table with **two OTA app slots**, and (strongly recommended)
  bootloader rollback. In your project's `sdkconfig.defaults`:

  ```
  CONFIG_PARTITION_TABLE_TWO_OTA=y
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
  CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
  ```

  Both settings live in the bootloader / partition table and must ship with
  the **first serial flash**; they cannot be enabled later over OTA. Without
  `BOOTLOADER_APP_ROLLBACK_ENABLE` the component still updates firmware, but
  without automatic rollback (images are confirmed on the first successful
  check-in instead).

## Quick start

```c
#include "simpleota.h"

void app_main(void) {
    // nvs_flash_init() first. Call simpleota_init() before other bring-up
    // so a trial arms the chip watchdog immediately. Wi-Fi can follow;
    // the task waits on is_connected.

    simpleota_config_t cfg = {
        .token = "YOUR_PROJECT_TOKEN",   // Project -> API tokens (device scope)
    };
    ESP_ERROR_CHECK(simpleota_init(&cfg));
    ESP_ERROR_CHECK(simpleota_start());
}
```

That is a complete integration: the component starts a background task that
checks hourly, flashes offered builds, reboots into them as a supervised
trial, and confirms them once the new image completes a successful check-in.
See [examples/basic](../examples/basic) for a full project including Wi-Fi
bring-up, and [examples/signed](../examples/signed) for signed firmware.

## Configuration

All fields of `simpleota_config_t` (strings are copied at init):

| Field | Default | Purpose |
|---|---|---|
| `token` | required | Project API token (device scope) or per-device token |
| `base_url` | `https://simpleota.com` | Platform base URL |
| `device_id` | Wi-Fi STA MAC (`aa:bb:cc:dd:ee:ff`) | Stable per-unit identifier |
| `chip_family` | `CONFIG_IDF_TARGET` | e.g. `esp32`, `esp32s3`, `esp32c3` |
| `board_id`, `hardware_revision`, `partition_profile`, `channel` | omitted | Compatibility metadata; report what your artifacts constrain |
| `labels_json` | omitted | Reserved: sent verbatim on check-ins, not yet consumed by the platform |
| `security_mode` | omitted | `SIMPLEOTA_SECURITY_SIGNED` to enforce signed firmware |
| `version_label` | last persisted | Display-only version string |
| `nvs_schema_version` | 1 | On-device data schema the firmware expects |
| `check_interval_s` | 3600 | Steady-state poll interval |
| `initial_delay_ms` | 0 | Delay before the first check after `simpleota_start()` |
| `confirm_timeout_s` | 300 | Trial window before automatic rollback |
| `manual_confirm` | false | true: you call `simpleota_confirm()` after health checks |
| `disable_auto_reboot` | false | true: you call `simpleota_reboot_for_update()` |
| `signing_keys`, `num_signing_keys` | none | Up to 2 pinned Ed25519 public keys (PEM) |
| `is_connected` | always try | Return false to postpone network activity |
| `event_cb`, `user_ctx` | none | Lifecycle events (see `simpleota_event_id_t`) |
| `cert_pem` | none | Pin a specific CA instead of the certificate bundle |
| `crt_bundle_attach` | `esp_crt_bundle_attach` | Override the bundle function |

Build-time knobs (menuconfig, "SimpleOTA"): task stack size (12288),
priority (5), HTTP timeout (15 s), HTTP TX buffer (2048 -- must fit the long
pre-signed download URLs), download chunk size (4096). The same menu has
`SIMPLEOTA_DISABLE_TRIAL_WATCHDOG`, which is off unless you turn it on.
Turning it on leaves the chip watchdog alone during a trial. The confirm
timer still runs.

## Update lifecycle

Status events reported to the dashboard, in order:

```
download_started -> downloaded -> flashed -> validated -> reboot
    -> (next successful check-in) confirmed | rolled_back
```

Failures report `failed` with a reason token (`checksum_mismatch`,
`signature_invalid`, `download_stalled`, `update_begin_failed`, ...) that the
dashboard shows inline on the device row. `confirmed` and `rolled_back` are
retried across reboots until the server accepts them.

## Rollback

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a freshly flashed image boots
as PENDING_VERIFY: the **bootloader itself** guarantees one attempt. If the
image crashes or loses power before confirmation, the next boot returns to
the previous app. On top of that, the component runs a confirm timeout
(default 300 s, `confirm_timeout_s`). If the new image runs but never
completes a successful check-in, the component marks it invalid and the
device reboots into the previous build, reporting `rolled_back` with
reason `confirm_timeout`.

During that pending trial, `simpleota_init()` also starts the chip's RTC
or LP watchdog, for the confirm timeout plus a margin. The margin is the
larger of 60 seconds and 10 percent of the confirm timeout, and the stage
value comes from the calibrated slow clock (`esp_clk_slowclk_cal_get()`,
falling back to `rtc_clk_slow_freq_get_hz()` when the calibration is 0).
The watchdog runs on its own clock, so a hang that stops both cores, and
therefore stops the confirm `esp_timer`, still resets the chip. The
bootloader then switches back, and the next boot reports `rolled_back`
with reason `watchdog`. `simpleota_confirm()`, and the automatic confirm
on the first successful check-in, both disable that watchdog. The
component's own timeout rollback disables it before the reboot, so that
reboot stays `confirm_timeout`. A boot that is not a pending SimpleOTA
trial does not touch the watchdog, and neither does a plain image.

Call `simpleota_init()` as soon as NVS is ready. That is when the watchdog
starts. The examples do this before Wi-Fi, then call `simpleota_start()`
so the confirm timer has a task to run on while the network comes up.
`CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` skips the watchdog and leaves
the confirm timer in place. If a target's build has no `hal/wdt_hal.h`,
the component skips the arm and logs that a hang during the trial is not
reset by it. A crash still switches back through the bootloader when
rollback is enabled.

At the usual internal slow clock the stage can wait at most about 8 hours
on the original ESP32, about 9 hours on the S3, C3, C6, and H2, and about
13 hours on the S2. A longer `confirm_timeout_s` still sets the confirm
timer to the time you asked for, and the chip watchdog fires at its
maximum. Light sleep pauses this watchdog, because `wdt_hal_init` enables
pause-in-sleep. Deep sleep does not pause it, so a trial that enters deep
sleep can still be reset when the deadline passes.

| Chip | Peripheral | What this component calls |
|---|---|---|
| ESP32, S2, S3, C3 | RTC watchdog (`RTCCNTL`) | `wdt_hal` `WDT_RWDT`, stage 0 action `WDT_STAGE_ACTION_RESET_RTC` |
| ESP32-C6, H2 | LP watchdog, HAL name RWDT | the same `wdt_hal` `WDT_RWDT` calls |

C2 uses that same RTC watchdog, and C5 and P4 use the LP watchdog under the
same `WDT_RWDT` name. Those three are not in this component's published
target list. The source does not special-case the chip: any target whose
IDF provides `hal/wdt_hal.h` gets this sequence.

After any rollback the component restores the previous build number in NVS,
so the dashboard sees the truth and the server can re-offer fixed builds. The
`rolled_back` report carries the detected cause: `confirm_timeout` when the
component's own supervision marked the image invalid, `watchdog` when the
reset reason is the chip watchdog (`ESP_RST_WDT`), and `boot_failed` when
the bootloader rejected or abandoned the image for another reason (crash
loop, power loss during the trial, image-verification or Secure Boot
rejection). Even without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a corrupt
image can be refused by the bootloader's basic image verification and will
then surface as `rolled_back` / `boot_failed`.

Confirmation is automatic on the first successful check-in. If your
application has its own health checks, set `.manual_confirm = true` and call
`simpleota_confirm()` once they pass. During a trial this component is using
the chip watchdog, so start your own use of that watchdog after confirm.

## Signed firmware

Uploads with `security_mode: "signed"` carry an Ed25519 signature over the
exact binary bytes. Pin your project's public key and the device verifies
every image during download, **before** the partition is marked bootable:

```c
static const char *PUBKEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "...from the dashboard's Signing keys section...\n"
    "-----END PUBLIC KEY-----\n";

const simpleota_signing_key_t keys[] = {{.key_id = "prod-2026", .pem = PUBKEY_PEM}};
simpleota_config_t cfg = {
    .token = "...",
    .security_mode = SIMPLEOTA_SECURITY_SIGNED,
    .signing_keys = keys,
    .num_signing_keys = 1,
};
```

Behavior matrix (identical to the Arduino client):

| Device state | Offer | Result |
|---|---|---|
| signed mode + key pinned | valid signature | verify during download; flash only on pass |
| signed mode + key pinned | missing/invalid/stripped signature | **rejected before flashing**; `failed` / `signature_invalid`; device stays on current build |
| key pinned (any mode) | signed offer without usable signature | rejected (fail closed) |
| no key pinned | signed offer | applied with a prominent "unverified" warning (keeps basic -> signed migration flowing) |
| any | unsigned offer, device not enforcing | normal checksum-verified update |

An enforcing device (signed mode + pinned key) cannot be silently downgraded:
stripping the signature fields from a response fails closed.

**Key rotation:** pin the old and new key together (`num_signing_keys = 2`),
start signing with the new key in CI, revoke the old key on the server once
the fleet has moved.

**Migration (basic -> signed) for a deployed fleet:** ship one last `basic`
upload whose firmware pins the key and sets
`.security_mode = SIMPLEOTA_SECURITY_SIGNED`; devices then report signed mode
and all subsequent uploads are signed and verified. The server never offers a
signed artifact to a device still reporting basic.

**Scope note:** this verifies the *delivery path* (compromised build server,
leaked CI token, tampered storage, MITM). It is not Secure Boot: someone with
physical access can still reflash over serial. The two compose; if you enable
Secure Boot v2, sign the final (Secure-Boot-signed) binary for SimpleOTA and
note that a Secure Boot rejection surfaces as `update_end_failed`, distinct
from `signature_invalid`.

## TLS

Server and download connections use the ESP-IDF certificate bundle by
default (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`, enabled by default), so there
is no CA to paste or rotate. Pin a specific CA with `.cert_pem` if you
prefer. Plain HTTP download URLs are refused (`insecure_url`).

### Task watchdog warnings during the handshake

Verifying a certificate chain is a synchronous, CPU-bound operation inside
mbedTLS. The OTA task cannot yield while it runs, so the priority-0 IDLE
task may be starved for longer than the default 5 second task-watchdog
window, and you see a warning like:

```
E (17533) task_wdt: Task watchdog got triggered. ... IDLE ...
```

This is a warning, not a crash: the check completes normally. It shows up
most often on the classic ESP32 at the default 160 MHz, on the first
handshake after boot. Any TLS client on this hardware can hit it; it is not
specific to this component, and the component cannot yield from inside the
mbedTLS call. Pick whichever fix suits your project:

| Setting | Effect |
|---|---|
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` | Widen the window, keep the watchdog. What the examples do, and the recommended default. |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` | Roughly halves handshake time on ESP32/ESP32-S3 (not available on targets capped at 160 MHz, such as ESP32-C3). |
| `.cert_pem = <your CA>` | Pinning one CA verifies less than searching the full bundle. |
| `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` | Stops watching that core's idle task. Blunt; prefer the timeout. |

## Examples

- [examples/basic](../examples/basic): managed updates with rollback.
- [examples/signed](../examples/signed): on-device Ed25519 verification.

Both are complete projects (`idf.py menuconfig` for Wi-Fi + token, then
`idf.py flash monitor`). They build against the in-repo component through a
local override that works from any clone directory name (the component lives
in the `simpleota/` subdirectory precisely so the override resolves).

New to ESP-IDF, or testing on real hardware for the first time? See
[docs/hardware-testing.md](../docs/hardware-testing.md) for a full
install-to-first-OTA walkthrough.

## Testing

`make -C test/host test` runs the security-critical modules (Ed25519
verification, SHA-256, the signed-firmware gate, response parsing) on the
host with plain cc: RFC 8032 vectors, NIST SHA-256 vectors, a vector
generated by the actual SimpleOTA server stack, tamper/wrong-key/truncation
negatives, key rotation, and the full gate truth table. The same run checks
the trial watchdog margin, the calibrated slow-clock tick conversion, and
the `watchdog` rollback reason. CI builds the examples on IDF v5.1, v5.2,
v5.3, v5.4, v5.5, and latest, covering each published target, and checks
that the watchdog object references `wdt_hal_init` unless
`SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` is set. `examples/compile_check` is the
build used for the ESP32-H2, which has no Wi-Fi radio.

## License

MIT (see [LICENSE](LICENSE)). Ed25519 verification uses a vendored,
unmodified copy of [Monocypher](https://monocypher.org/) 3.1.3, dual-licensed
CC0-1.0 / BSD-2-Clause (see [src/vendor/LICENSE.md](src/vendor/LICENSE.md)).
