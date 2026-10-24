# Testing on real hardware

A step-by-step guide to installing ESP-IDF, building the examples, flashing
them to a real ESP32-family board, and testing an OTA update end to end.

Written for first-time ESP-IDF users, including those coming from the
Arduino core. Flashing ESP-IDF firmware overwrites the bootloader, partition
table, and app currently on the chip. That's expected and fully reversible:
reflash Arduino firmware again afterward the normal way, any time. Your
SimpleOTA project, tokens, and dashboard are shared between both toolchains.

## 0. Install ESP-IDF (one-time)

Skip this section if you already have an ESP-IDF install (`idf.py
--version` works in a terminal after sourcing its `export.sh`).

Pick a version this component's CI already validates:
[`v5.1.4`](../.github/workflows/ci.yml) is the floor version and the most
heavily tested, and any `v5.3.x` or newer is also covered.

```sh
mkdir -p ~/esp
cd ~/esp
git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32       # or esp32c3 / esp32s3, matching your board
```

- `--recursive` pulls in submodules. This is required, so don't skip it.
- `install.sh <target>` installs only that chip's toolchain, which is
  faster than installing every target.
- This downloads roughly 1-2 GB and typically takes 5-15 minutes.

## 1. Load the ESP-IDF environment

Before any `idf.py` command works, run this once per terminal window:

```sh
. ~/esp/esp-idf/export.sh
```

(A single dot, a space, then the path. This "activates" ESP-IDF for the
current shell, similar to a Python virtualenv. Run it again in every new
terminal tab/session; it doesn't install anything and is instant.)

Confirm it worked:

```sh
idf.py --version
```

## 2. Is this safe to run against an uncommitted checkout?

Yes. Every file `idf.py` creates is already covered by this repo's
`.gitignore`: `sdkconfig`, `sdkconfig.old`, `build/`, `managed_components/`,
`dependencies.lock`. Building and flashing are strictly read-only on the
component and example sources: nothing you run in this guide modifies or
stages any tracked file. `git status` will look identical before and after.

## 3. Open an example project

```sh
cd examples/basic
```

(`examples/signed` works the same way; see step 7.)

Tell it which chip you have:

```sh
idf.py set-target esp32       # classic ESP32
idf.py set-target esp32c3     # ESP32-C3
idf.py set-target esp32s3     # ESP32-S3
```

Check the silkscreen on your board or the label on the module if you're
not sure which one.

## 4. Configure Wi-Fi and your SimpleOTA token

```sh
idf.py menuconfig
```

This opens a full-screen text menu (arrow keys to move, Enter to select,
Escape to go back).

1. Move down to **"Example Configuration"** and press Enter.
2. Set:
   - **Wi-Fi SSID**: your network name
   - **Wi-Fi password**
   - **SimpleOTA project token**: a *project* token from your SimpleOTA
     dashboard (Project > API tokens). Keep the default device scope. This
     is the same kind of token used by the Arduino library, with nothing
     ESP-IDF-specific about it.
3. Press **Escape** repeatedly until it offers to save, choose **Save**,
   then **Exit**.

## 5. Plug in your board and find its port

```sh
ls /dev/cu.usb*
```

You should see something like `/dev/cu.usbserial-1420` or
`/dev/cu.SLAB_USBtoUART`. Note it for the next step.

## 6. Build, flash, and watch the logs

If the board last ran firmware with a different partition table, such as an
Arduino sketch, erase it first so its NVS partition starts empty:

```sh
idf.py -p /dev/cu.usbserial-1420 erase-flash
```

Then build and flash:

```sh
idf.py -p /dev/cu.usbserial-1420 flash monitor
```

(Replace the port with whatever step 5 showed.) This compiles everything
(first build takes a few minutes; later ones are fast), writes the
bootloader, partition table, OTA data and app to your board, reboots it, and
streams the live serial log to your terminal. **This is the step that
replaces any existing Arduino firmware**, which is expected and reversible
(see the bottom of this guide).

You should see boot messages, then something like:

```
I (1234) example: running build 0; OTA task started
I (2345) wifi: got IP
I (3456) simpleota: check: no update (up_to_date)
```

That last line means it successfully talked to SimpleOTA and there's
nothing new to install yet.

Press **Ctrl + ]** to exit the log viewer whenever you want (this detaches
your terminal; it does not stop the board).

## 7. Check the dashboard

Open your SimpleOTA project in the browser. The device should now appear in
the device list, reporting `framework: esp_idf`, with a recent "last seen"
time.

## 8. Test a real OTA update

1. Edit `examples/basic/main/main.c` and change something you can visually
   confirm later, e.g. add another `ESP_LOGI` line. A small, harmless
   change is fine.
2. Rebuild (no need to reflash yet):
   ```sh
   idf.py build
   ```
3. Upload this file to SimpleOTA as a new artifact:
   ```
   build/simpleota_basic.bin
   ```
   (The equivalent of the Arduino `.ino.bin`: the app image only, not a
   merged/bootloader image.)
4. Deploy it to your project as you normally would.
5. Watch your terminal (or run `idf.py -p /dev/cu.usbserial-1420 monitor`
   again if you closed it). Within one check interval you should see the
   full sequence:
   ```
   check: offer build=1 ...
   status: event=download_started
   status: event=downloaded
   status: event=flashed
   status: event=validated
   status: event=reboot
   ```
   ...then the board reboots, reconnects, and reports `confirmed`.
6. The dashboard's device timeline should show the same sequence.

## 9. Test signed firmware (optional)

Use `examples/signed` instead of `examples/basic`, with the same steps as above
(set-target, menuconfig, flash monitor), with two differences:

- Before building, open `examples/signed/main/main.c` and paste your
  project's real **public** signing key (from the dashboard's Signing Keys
  section) in place of the placeholder. The placeholder is deliberately
  unparseable: if you forget this step, the board fails closed and logs an
  error at startup instead of running unverified.
- Sign the `.bin` before uploading it to SimpleOTA (openssl, or the
  dashboard's in-browser signer), exactly as for a signed Arduino build.

## 10. Test rollback (optional)

The examples set `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Run the table once
with it, and once with it removed from `sdkconfig.defaults` (after
`idf.py fullclean` and a serial flash of the new bootloader), where the
component rolls back in software and every row must end the same way. Deploy a known-good build first, then deploy each of these test builds
from it and watch the log and the dashboard. Adding
`.confirm_timeout_s = 60` to the config in `main.c` keeps the waits short.

| Test build | Expected result |
|---|---|
| Unchanged | Stays on the new build; `confirmed` |
| `.manual_confirm = true` and never calls `simpleota_confirm()` | Back on the known-good build after the confirm timeout; `rolled_back` / `confirm_timeout` |
| Calls `abort()` a few seconds into `app_main` | Back on the known-good build after one reset; `rolled_back` / `panic` |
| Hangs in `app_main` (`for (;;) {}`) | Back on the known-good build; `rolled_back` / `confirm_timeout`, or the watchdog reset that caught the hang |
| Starts a task at priority `configMAX_PRIORITIES - 1` on each core that spins forever, so the confirm timer cannot run either | Back on the known-good build after the confirm timeout plus the watchdog margin; `rolled_back` / `watchdog` |
| Unchanged, but you unplug the board during its first seconds | Back on the known-good build; `rolled_back` / `power_on` |

Unplugging during the download leaves the known-good build running, and it
downloads the update again on a later check. Run the table once on a board
that still has 0.2.1, so the first update into this release is covered too.
If your devices deep-sleep, also put a test build to sleep before it
confirms: it should wake on the known-good build with `rolled_back` /
`deep_sleep`. See the main
[README](../simpleota/README.md#rollback) for what each rollback reason means.

## 11. Test polling mode (optional)

`examples/polling` runs the checks from the app's own loop instead of the
component's task. Build and flash it as in steps 3 to 6, then test an update
as in step 8 (upload `build/simpleota_polling.bin`). It checks once at boot
and then hourly, so reset the board to check sooner. After the update it
confirms the trial itself once Wi-Fi is up; the log shows `running build N
(on trial)` and then the `confirmed` report.

## 12. Test anti-rollback and USB flashing (optional)

| Test | Expected result |
|---|---|
| Deploy a build that is older than, or the same as, the one the board runs | Nothing is downloaded; `failed` / `downgrade_refused` (`SIMPLEOTA_DOWNGRADE` in `examples/polling`) |
| Serial-flash a different build over one SimpleOTA installed, with `image_build` left at 0 | The log warns `running an image SimpleOTA did not install; reporting build 0`, and the server offers the board its current release |
| The same, with `.image_build` in `main.c` set to the build number SimpleOTA gave that exact `.bin` | The board reports that build and is not offered it again |
| Serial-flash the same `.bin` that SimpleOTA installed | No warning; the board keeps its build |

## Getting back to Arduino

Reflash your Arduino sketch the normal way (Arduino IDE or arduino-cli). It
overwrites the ESP-IDF bootloader/partitions/app the same way this guide
overwrote Arduino. For that first upload, set **Tools > Erase All Flash Before
Sketch Upload** to **Enabled**, so the Arduino layout starts with an empty NVS
partition. Nothing about the chip is permanently changed by either
toolchain.

## Quick reference

| Command | What it does |
|---|---|
| `. ~/esp/esp-idf/export.sh` | Load ESP-IDF into this terminal (every new terminal) |
| `idf.py set-target esp32` | Pick your chip (once per project folder) |
| `idf.py menuconfig` | Configure Wi-Fi + token |
| `idf.py build` | Compile only |
| `idf.py -p /dev/cu.usbserial-XXXX erase-flash` | Erase the whole flash (before switching partition layouts) |
| `idf.py -p /dev/cu.usbserial-XXXX flash monitor` | Flash + watch logs |
| `idf.py monitor` | Reattach the log viewer (board already flashed) |
| `Ctrl + ]` | Exit the log viewer |
