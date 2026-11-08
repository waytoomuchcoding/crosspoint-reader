# CrossPoint Reader (waytoomuchcoding fork)

This is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader),
open-source e-reader firmware for Xteink and other FreeInk-supported devices.

It is not affiliated with the upstream CrossPoint project, Xteink, or any
device manufacturer. Report problems with this fork here, not upstream.

## How this fork differs

This fork follows its own feature direction and is not bound by upstream's
`SCOPE.md` or contribution process. Changes from upstream so far:

- Removed upstream's AI-agent scope gate and AI contribution policy from
  `AGENTS.md` and `.agents/`.
- Chinese reading mode: zhuyin ruby on words rarer than a chosen frequency,
  CC-CEDICT word lookup, and a pixel-exact Unifont setup. See
  [docs/chinese-reading.md](docs/chinese-reading.md).
- Desktop simulator build targets (`simulator`, `simulator_x3`).

Known upstream defaults still in place:

- **Firmware updates.** The on-device update check (Settings) pulls releases
  from the upstream repository, so installing an update replaces this fork's
  build with upstream's. Avoid on-device updates and flash builds from this
  repository instead.
- **Plugins** install from upstream's plugin hub.
- **Fonts** download from upstream's fonts repository.
- **KOReader sync** defaults to upstream's sync server; it can be changed in
  settings.

## Build and flash

Prerequisites:

- [pioarduino PlatformIO Core](https://github.com/pioarduino/platformio-core) or
  [VS Code + pioarduino IDE](https://github.com/pioarduino/pioarduino-vscode-ide)
- Python 3.8+
- `clang-format` 21
- USB-C cable that supports data transfer

```bash
git clone --recursive https://github.com/waytoomuchcoding/crosspoint-reader
cd crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive

pio run --target upload
```

Nix/NixOS users can enter the development shell with `nix develop -f nix` or
`nix-shell nix`.

Before committing:

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

## Documentation

Upstream's documentation is kept in this repository:

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md) and [endpoints](./docs/webserver-endpoints.md)
- [Contributing and architecture docs](./docs/contributing/README.md)

## Syncing with upstream

```bash
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git  # once
git fetch upstream
git merge upstream/develop
```

Merges from upstream may bring back files this fork removed or changed,
such as `README.md`, `AGENTS.md`, and `.agents/`. Check them after each merge.

## License

MIT, as upstream. See [LICENSE](./LICENSE) and [licenses/](./licenses/).
