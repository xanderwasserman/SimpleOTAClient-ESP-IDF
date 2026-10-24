# SimpleOTAClient-ESP-IDF

First-party ESP-IDF component for the [SimpleOTA](https://simpleota.com)
firmware update platform, published to the Espressif Component Registry as
[`wassie-dev/simpleota`](https://components.espressif.com/components/wassie-dev/simpleota).

[Managed fleet OTA for ESP32 devices in the field](https://simpleota.com/?utm_source=idf_registry&utm_medium=readme&utm_campaign=signup_v1)

Starting a new project? [simpleOTA-starter-esp-idf](https://github.com/wassie-dev/simpleOTA-starter-esp-idf) is a template repo with a working ESP-IDF app and a GitHub Actions release workflow already set up.

```sh
idf.py add-dependency "wassie-dev/simpleota"
```

Full documentation (features, configuration, signed firmware, rollback):
**[simpleota/README.md](simpleota/README.md)** (the component itself, also
rendered on the registry page).

Complete example projects: [examples/basic](examples/basic) (managed mode),
[examples/polling](examples/polling) (the app runs the checks) and
[examples/signed](examples/signed). The examples build against the in-repo
component via a local override, so they always exercise the checked-out code.

New to ESP-IDF, or want to test on real hardware? See
[docs/hardware-testing.md](docs/hardware-testing.md) for a full
install-to-first-OTA walkthrough.

The client itself, the `simpleota.h` API and everything behind it, lives in
[SimpleOTAClient-core](https://github.com/wassie-dev/simpleOTAClient-core),
which the Arduino library ships too. It is copied into `simpleota/src/core`,
with the header in `simpleota/include`, as plain files; its host tests live
in the core repository. This repository is the packaging: the component's
`CMakeLists.txt`, `Kconfig`, manifest, examples and docs. To move to another core tag or commit, run
`scripts/update-core.sh <ref>` and commit the result. CI fails when the copy
differs from the commit in `simpleota/src/core/CORE_VERSION`.

**Releasing** (mirrors SimpleOTAClient-Arduino): bump `version:` in
[simpleota/idf_component.yml](simpleota/idf_component.yml) and merge to
`main`. Once CI is green, the Release workflow tags `vX.Y.Z`, creates the
GitHub release, and uploads to the Espressif Component Registry
automatically. Pushes that do not bump the version release nothing.
Registry versions are immutable, so a version publishes exactly once.

MIT licensed; Ed25519 verification uses vendored Monocypher 3.1.3
(CC0-1.0 / BSD-2-Clause). See [simpleota/README.md](simpleota/README.md#license).
