# SimpleOTA client for ESP-IDF

First-party ESP-IDF component for the [SimpleOTA](https://simpleota.com)
firmware update platform. Polls for updates, streams and flashes offered
firmware with on-the-fly SHA-256 and Ed25519 signature verification, reports
the full update lifecycle to the dashboard, and integrates with the ESP-IDF
bootloader's app-rollback support so a bad build backs itself out.

The sibling of [SimpleOTAClient-Arduino](https://github.com/wassie-dev/simpleOTAClient-Arduino),
which is the same client with the same `simpleota.h` API, so mixed fleets
behave identically. A device keeps its
identity (Wi-Fi MAC device id) across a migration between the two.

Local OTA (ArduinoOTA, espota, ElegantOTA) is for a board on your desk. SimpleOTA is managed fleet OTA for ESP32 when those devices are in the field, without pulling you into a full IoT platform.

You upload a .bin from PlatformIO, ESP-IDF, Arduino, or CI, and devices check in on their own. Eligible ones get a short-lived pre-signed URL and pull firmware straight from object storage. Rollouts can start at 5% with deterministic bucketing, so the same unit stays in the same cohort. Builds can be Ed25519-signed and verified on the device during download, before the new partition is marked bootable. After reboot there's a trial window: crash or no successful check-in, and the device rolls back.

Signing here protects the delivery path. It is not Secure Boot. They stack if you already use Secure Boot.

[Quickstart (project, client, first update)](https://wiki.simpleota.com/getting-started/?utm_source=idf_registry&utm_medium=readme&utm_campaign=signup_v1)
[Site](https://simpleota.com/?utm_source=idf_registry&utm_medium=readme&utm_campaign=signup_v1)

## Install

Starting a new project? [simpleOTA-starter-esp-idf](https://github.com/wassie-dev/simpleOTA-starter-esp-idf) is a template repo with a working ESP-IDF app and a GitHub Actions release workflow already set up.

```sh
idf.py add-dependency "wassie-dev/simpleota"
```

## Requirements

- ESP-IDF **v5.1 or newer** (tested through IDF 6.x). Targets: esp32, esp32s2,
  esp32s3, esp32c3, esp32c6, esp32h2.
- A partition table with **two OTA app slots**, and (strongly recommended)
  bootloader rollback. With the `partitions.csv` from
  [Partition table](#partition-table) in your project directory, put this in
  your project's `sdkconfig.defaults`:

  ```
  CONFIG_PARTITION_TABLE_CUSTOM=y
  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
  CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
  ```

  ESP-IDF reads `sdkconfig.defaults` only when it creates `sdkconfig`: in a
  project that already has one, delete it or set the options with
  `idf.py menuconfig`.

  The partition table and the bootloader settings ship with the **first
  serial flash**; they cannot be changed later over OTA. Without
  `BOOTLOADER_APP_ROLLBACK_ENABLE` the component rolls back on its own
  (see [Rollback](#rollback)), which catches less.

## Quick start

```c
#include "simpleota.h"

void app_main(void) {
    // nvs_flash_init(), then bring up Wi-Fi (or Ethernet) and wait for an
    // IP before simpleota_start(). A check that runs without one fails, and
    // the next one is check_interval_s (an hour by default) later.

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
`esp_wifi_connect()` returns before the IP arrives, so if your bring-up does
not wait for one, set the optional `.is_connected` callback to return false
until it does: each check then waits for the link, as
[examples/basic](../examples/basic/main/main.c) does.
See [examples/basic](../examples/basic) for a full project including Wi-Fi
bring-up, [examples/polling](../examples/polling) for polling mode, and
[examples/signed](../examples/signed) for signed firmware.

### Polling mode

To run the checks from your own task instead, skip `simpleota_start()`:

```c
bool update = false;
if (simpleota_check(&update) == ESP_OK && update) {
    simpleota_result_t r = simpleota_apply();   // reboots on success
    if (r != SIMPLEOTA_OK) ESP_LOGW("app", "update failed: %d", r);
}
```

Each call runs on a short-lived task with `CONFIG_SIMPLEOTA_TASK_STACK_SIZE`
while your task waits, so calling from `app_main()` with its default stack
is fine.

In polling mode you confirm a trial yourself with `simpleota_confirm()` once
the new image is healthy, whatever `manual_confirm` says. The modes don't mix:
while the managed task runs, `simpleota_check()` returns
`ESP_ERR_INVALID_STATE` and `simpleota_apply()` returns `SIMPLEOTA_NO_OFFER`, with a warning.
[examples/polling](../examples/polling) is a complete project.

`simpleota_set_server_url()` moves check-ins to another `https://` server
while the managed task is stopped, and `simpleota_last_offered_version()`
returns the last offer's version label.

## API reference

The **[API reference](https://wassie-dev.github.io/simpleOTAClient-core/latest/)** documents every
function, result, event and configuration field. It is generated from
[`include/simpleota.h`](include/simpleota.h), which has the same comments.
In short:

- Fill a [`simpleota_config_t`](https://wassie-dev.github.io/simpleOTAClient-core/latest/structsimpleota__config__t.html)
  from all zeros and call `simpleota_init()` early. Strings are copied.
- Managed mode: `simpleota_start()`, `simpleota_stop()`,
  `simpleota_check_now()`.
- Polling mode: `simpleota_check()`, then `simpleota_apply()`, from one
  task of yours.
- Trials: `simpleota_confirm()`, `simpleota_is_trial_boot()`.
- With `disable_auto_reboot`: `simpleota_reboot_for_update()`.
- Also `simpleota_set_server_url()`, `simpleota_current_build()`,
  `simpleota_last_offered_version()` and `simpleota_deinit()`.

`event_cb` runs on the task doing the work: keep it short, and call only
`simpleota_reboot_for_update()` from it. The `reason` tokens that events
and the dashboard show are listed under [Update lifecycle](#update-lifecycle)
and [Rollback](#rollback).

## Build options

Build-time knobs (menuconfig, "SimpleOTA"): task stack size (12288),
priority (5), HTTP timeout (15 s), HTTP TX buffer (2048 -- must fit the long
pre-signed download URLs), download chunk size (4096),
`SIMPLEOTA_SOFTWARE_ROLLBACK` (on; only without bootloader rollback, see
[Rollback](#rollback)), and `SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` (off), which
leaves the trial watchdog out.

## Update lifecycle

Status events reported to the dashboard, in order:

```
download_started -> downloaded -> flashed -> validated -> reboot
    -> (next successful check-in) confirmed | rolled_back
```

Failures report `failed` with a reason token that the dashboard shows
inline on the device row, and that `SIMPLEOTA_EVENT_UPDATE_FAILED` carries:

| Reason | Stage |
|---|---|
| `downgrade_refused` | The offered build is not above the running one (reported once per boot) |
| `insecure_url` | The download URL is not `https://` |
| `signature_invalid` | A signed offer failed Ed25519 verification, or a required signature is missing |
| `https_begin_failed` | The download connection could not be opened |
| `http_status_<N>` | The download returned HTTP N, or a fourth redirect |
| `redirect_not_https` | A download redirect led to a plain HTTP URL |
| `redirect_malformed` | A download redirect had no usable `Location` |
| `update_begin_failed` | The update slot could not be opened: no second OTA slot, or the image is larger than the slot |
| `update_write_failed` | A flash write failed during the download |
| `stream_error` | The connection broke during the download |
| `download_stalled` | No bytes arrived for `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` |
| `short_read` | The download ended before its size |
| `checksum_mismatch` | The SHA-256 of the download did not match the offer |
| `update_end_failed` | The image failed ESP-IDF's image check, or the boot slot could not be switched |
| `state_save_failed` | The trial record could not be saved to NVS before the slot switch |

Once a boot has worked out what happened to a trial, its `confirmed` or
`rolled_back` report goes to an outbox in NVS (two slots, oldest dropped
first) and never holds up an update. Reports are kept until the server
answers, across reboots: any HTTP response counts as delivered, and the
device does not act on what it says. Without a response the report is sent
again, first after 10 s (three times), then at the normal check interval.

Check-ins and status posts retry once after 2 s when no response arrives
(not during a trial), and never retry a response that arrived. The download
does not retry; the next check can try again.

## Rollback

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a freshly flashed image boots
as PENDING_VERIFY: the **bootloader itself** guarantees one attempt. If the
image crashes or loses power before confirmation, the next boot returns to
the previous app. On top of that, the component runs a confirm timeout
(default 300 s): if the new image runs but never completes a successful
check-in (or, with `.manual_confirm`, is never confirmed), it is marked
invalid and the device reboots into the previous build, reporting
`rolled_back` / `confirm_timeout`.

A hang that also stops the confirm timer is caught by the chip's RTC or LP
watchdog, which runs on the slow clock. `simpleota_init()` starts it on a trial
boot for the confirm timeout plus a margin, and confirming stops it. The margin
is the larger of 10 percent and the time one request can take at
`CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` (70 s at the default), and never less than
60 s. During a trial each request is sent once and queued reports wait, so
when the confirm timeout runs out the component rolls back first, reporting
`confirm_timeout`. When the watchdog fires, the chip resets, the bootloader
returns to the previous build, and the report says `watchdog`. Call `simpleota_init()`
as soon as NVS is ready, since that is when the watchdog starts. At the usual
internal slow clock it can wait at most about 8 hours (about 13 on the S2); a
longer confirm timeout logs a warning and the watchdog fires at its limit. It
stops before deep sleep. Deep sleep before confirming ends the trial: the
device wakes on the previous build and the report says `deep_sleep`, also with
`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`, where the component rolls
back on the wake. So confirm before sleeping.
With the bootloader watchdog on (the default), other
boots leave the RTC watchdog alone. `CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG`
leaves it out; crashes still roll back.

After any rollback the component restores the previous build number in NVS,
so the dashboard sees the truth and the server can re-offer fixed builds. The
`rolled_back` report carries the detected cause: `confirm_timeout` when the
component's own supervision marked the image invalid. Otherwise the reset that
ended the trial: `panic`, `int_wdt`, `task_wdt`, `watchdog`, `brownout`,
`power_on`, `deep_sleep`, or `reset` (also used when the bootloader refused the
image, for example on an image-verification or Secure Boot rejection). Even
without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a corrupt image can be refused
by the bootloader's basic image verification and then surfaces as
`rolled_back`.

Without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image still runs as a
trial (`CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK`, on by default). The component
switches the boot slot back itself when the confirm timeout runs out, when the
trial boots after a panic or watchdog reset, or when the device wakes from
deep sleep before confirming. It cannot catch a crash that comes before
`simpleota_init()` runs, so bootloader rollback is still recommended for new
devices. Turn the option off to accept a new image on its first boot.

`simpleota_confirm()` marks the image valid in the bootloader first. If that
fails, it returns `ESP_FAIL` and the trial keeps running, so call it again.
While a trial is running, no further update is downloaded: the other slot holds
the previous image the trial may still return to.

Confirmation is automatic on the first successful check-in. If your
application has its own health checks, set `.manual_confirm = true` and call
`simpleota_confirm()` once they pass.

## Anti-rollback

An offer whose build number is not above the running build is refused
(`downgrade_refused`, `SIMPLEOTA_DOWNGRADE`), because an old image's
signature still verifies and old builds can carry fixed bugs. To go back,
release the old code under a new, higher build number.

The running build is the one stored with the image SimpleOTA installed. An
image flashed some other way, such as over serial, reports build 0, so the
server offers it the current release and a device reused for another
project takes that project's builds.

### When to set `image_build`

Most projects never need it. Leave it at 0 while you develop and test, and
whenever devices only get firmware from SimpleOTA.

It is for one situation: you flash a **release** image over USB, such as at
the factory or when re-flashing a unit in the field, and the same file is
also a SimpleOTA upload. Without it the device reports build 0, so on its
first check-in the server sends it the current release again, or an older
one if that is what its deployment targets. With it, the device reports the
build it actually runs and is left alone.

To use it:

1. Look up the project's last build number on the dashboard, say 41.
   SimpleOTA numbers each upload one above the last, so the next upload
   will be 42.
2. Set `.image_build = 42` and build.
3. Upload that exact `.bin` to SimpleOTA, and check that it got build 42.
4. Flash the same `.bin` over USB.

If the number is wrong, the device reports a build it does not run. Set too
high, it is not offered any build up to that number, so set it back to 0 in
the next upload whenever you are not sure.

## Partition table

The examples ship this table as `partitions.csv`:

```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
otadata,  data, ota,     ,        0x2000,
phy_init, data, phy,     ,        0x1000,
ota_0,    app,  ota_0,   ,        1700K,
ota_1,    app,  ota_1,   ,        1700K,
```

It needs 4 MB of flash. Each OTA slot holds an app image of up to 1700 KB,
and 540 KB is free after `ota_1` for your own data partitions. There is no
factory partition: a serial flash boots `ota_0`, and updates alternate between
`ota_0` and `ota_1`. On ESP-IDF 5.4 and later,
`CONFIG_PARTITION_TABLE_TWO_OTA_LARGE=y` selects the same table.

ESP-IDF's `CONFIG_PARTITION_TABLE_TWO_OTA` also works. It adds a 1 MB factory
partition, which a serial flash boots and updates never write: the first
update installs into `ota_0`, and a rollback from it returns to the factory
image. Its two OTA slots are 1 MB each, which leaves little room for
application code on some targets (see [Footprint](#footprint)).

### Changing the layout

A device keeps the partition table it was flashed with; the table cannot
change over OTA. A new layout reaches a device only through a serial flash.

- Erase the flash with `idf.py erase-flash` before flashing a board whose
  current firmware uses a different layout, such as an Arduino sketch or an
  older table. `idf.py flash` writes the bootloader, the partition table, the
  OTA data and the app, and leaves the rest of the flash as it is. Without the
  erase, the new NVS partition starts with whatever the old layout left there,
  including the build number the component stored, so the device goes on
  reporting that build while it runs the firmware you flashed. After the erase
  it reports build 0 until its first update.
- Builds for devices already in the field must fit their app slot. An image
  larger than the slot is refused and the device stays on its build: the
  update fails with `update_begin_failed`, or with `update_write_failed` when
  the image size is not known in advance. Only the inactive slot is ever
  written.
- When a fleet has devices with different layouts, set `partition_profile` on
  each device to name its layout, and give each upload the profile it was
  built for. The server offers an upload only to devices that report the same
  profile. An upload without a profile is offered to every device, and so is
  any upload to a device that reports none.

## Footprint

Measured with ESP-IDF 5.1.4 and size optimization
(`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`), building the `basic` example against
the same project without SimpleOTA:

| Target | Added to an app that already uses HTTPS, NVS and OTA | Added to a Wi-Fi-only app | `basic` example: free in a 1700 KB slot | `basic` example: free in a 1 MB slot |
|---|---|---|---|---|
| ESP32 | 32 KB flash, 1.6 KB RAM | 204 KB flash, 3.5 KB RAM | 47% | 12% |
| ESP32-S3 | 33 KB flash, 1.5 KB RAM | 208 KB flash, 3.8 KB RAM | 48% | 14% |
| ESP32-C6 | 34 KB flash, 1.9 KB RAM | 245 KB flash, 4.1 KB RAM | 43% | 6% |

RAM is static RAM (`.data`, `.bss` and IRAM code).

Most of the flash added to a Wi-Fi-only app is the HTTPS stack (mbedTLS,
`esp_http_client` and the certificate bundle), which an app that already
makes HTTPS requests has anyway; the rest is the component itself, including
its Ed25519 and SHA-256 code.

At run time the component's task uses a stack of
`CONFIG_SIMPLEOTA_TASK_STACK_SIZE` bytes (12288 by default). A check
allocates a 4 KB response buffer and a download a buffer of
`CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK` bytes (4096 by default), and each request
holds the HTTP client's buffers and a TLS session on the heap while it runs.

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

Behavior matrix (the same in the Arduino library):

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
prefer. Everything is HTTPS: a `base_url` that is not `https://` fails
`simpleota_init()`, a plain HTTP download URL is refused (`insecure_url`),
and a download follows up to three redirects, each only to another
`https://` URL (`redirect_not_https` otherwise).

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
- [examples/polling](../examples/polling): the app runs the checks and
  confirms trials itself, and handles each `simpleota_result_t`.
- [examples/signed](../examples/signed): on-device Ed25519 verification.

All are complete projects (`idf.py menuconfig` for Wi-Fi + token, then
`idf.py flash monitor`). They build against the in-repo component through a
local override that works from any clone directory name (the component lives
in the `simpleota/` subdirectory precisely so the override resolves).

New to ESP-IDF, or testing on real hardware for the first time? See
[docs/hardware-testing.md](../docs/hardware-testing.md) for a full
install-to-first-OTA walkthrough, including how to test each kind of
rollback.

## Testing

The update logic is the shared core in `src/core`, copied from
[SimpleOTAClient-core](https://github.com/wassie-dev/simpleOTAClient-core) at
the commit in `src/core/CORE_VERSION`. Its host tests cover signing, SHA-256,
response parsing, the trial record, rollback, and a power cut at every storage
write. CI checks that `src/core` matches that commit, and builds the examples
across IDF v5.1 through latest on Xtensa and RISC-V targets.

## Arduino and ESP-IDF

The [Arduino library](https://github.com/wassie-dev/simpleOTAClient-Arduino)
is this same client: the same `simpleota.h`, built from the same sources, so
code that uses SimpleOTA works unchanged in a sketch. Two things differ,
because Arduino-ESP32 has no Kconfig and no certificate bundle:

- The build-time knobs above are `-D` flags there (`CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS`
  and so on, with the same defaults), set in PlatformIO's `build_flags`.
- Without `cert_pem`, TLS is verified against the ISRG Root X1 and X2
  certificates that SimpleOTA's hosts chain to, instead of the bundle.

## Upgrading from 0.2

The component's registry name changes to `wassie-dev/simpleota`: replace
`xanderwasserman/simpleota` in your `idf_component.yml`. The old name keeps
its 0.2.x releases and gets no new ones.

Devices update from 0.2.x over the air, also in the middle of a trial: the
NVS layout is unchanged, and a trial that fails returns to the old release,
which reads the rollback as it always has. 1.0.0 adds one NVS key,
`sota_image`, which older releases ignore. Existing code compiles unchanged,
unless it uses `SIMPLEOTA_SECURITY_TOKEN` or sets `security_mode` to a
string; what behaves differently:

- `security_mode` is now an enum, `simpleota_security_t`. The
  `SIMPLEOTA_SECURITY_BASIC` and `SIMPLEOTA_SECURITY_SIGNED` names still
  work. `SIMPLEOTA_SECURITY_TOKEN` is gone: the platform never used a
  reported "token", and a device reporting it was offered neither basic nor
  signed builds.
- `SIMPLEOTA_SECURITY_SIGNED` without a signing key fails
  `simpleota_init()` with `ESP_ERR_INVALID_ARG`: such a device would be
  offered signed builds it cannot verify.
- A `base_url` that is not `https://` fails `simpleota_init()` with
  `ESP_ERR_INVALID_ARG`, and download redirects are followed only to
  `https://` URLs.
- An offer whose build is not above the running one is refused
  (`downgrade_refused`). See [Anti-rollback](#anti-rollback).
- An image flashed over USB reports build 0, so the server offers it its
  current release (see [When to set `image_build`](#when-to-set-image_build)
  if you flash release images over USB). Before, it kept reporting
  the build SimpleOTA last installed.
- `manual_confirm` applies to managed mode only. Without
  `simpleota_start()`, call `simpleota_confirm()` yourself.
- Without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, new images now run as
  trials and roll back on any reset before they are confirmed. Confirm
  before restarting on purpose, or turn `CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK`
  off to keep the old behaviour.
- `simpleota_confirm()` returns `ESP_FAIL` when the bootloader cannot record
  the image as good, and `simpleota_deinit()` returns
  `ESP_ERR_INVALID_STATE` from `event_cb`.
- Every check logs its result at info level (`check: no update (up_to_date)`,
  `check: offer build=N`, `check: failed (HTTP N)`).

Going back to 0.2.x over USB works, but 0.2.x does not know the `sota_image`
key, so it keeps reporting the build 1.0.0 stored (the last one SimpleOTA
installed) instead of build 0. The server then offers it nothing until a
higher build ships. Erase NVS when you flash it, or release the build you
want as a new, higher one.

The [CHANGELOG](https://github.com/wassie-dev/simpleOTAClient-ESP-IDF/blob/main/CHANGELOG.md)
lists every change.

## License

MIT (see [LICENSE](LICENSE)). Ed25519 verification uses a vendored,
unmodified copy of [Monocypher](https://monocypher.org/) 3.1.3, dual-licensed
CC0-1.0 / BSD-2-Clause (see [src/core/LICENSE-vendor.md](src/core/LICENSE-vendor.md)).
