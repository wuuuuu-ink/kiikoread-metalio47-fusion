# kiikoreader WeRead Firmware

Community firmware source for **kiikoreader Metalio S31**, based on firmware `0.3.3-rc113`. It integrates WeRead shelves, account management, book caching, progress synchronization, and reading-duration reporting while reusing the native shelf, reader, and system font settings.

> This is not an official release by Tencent/WeRead or Metalio/kiikoread. WeRead endpoints are unofficial service interfaces and may change without notice.

中文说明：[README.md](README.md)

## Features

- QR-based account login and account management.
- Sync the WeRead shelf and covers in server order, with cached-state indicators.
- Cache book text and chapter images as local EPUB files.
- Read cached books with the firmware's native reader and its existing features.
- Upload local reading progress or download cloud progress.
- Track and report reading time for eligible WeRead books over Wi-Fi; offline users can connect for reporting or continue local reading.
- Reuse the existing shelf UI, pagination, and system font-size settings.

## Build

See [source layout](docs/SOURCE_LAYOUT.md), [WeRead architecture](docs/WEREAD_ARCHITECTURE.md), and [Metalio S31 build guide](docs/BUILD_METALIO_S31.md). ESP-IDF **v6.1** is required. From an exported ESP-IDF environment at the repository root:

```bash
python tools/build_firmware.py --board metalio47 --idf-metalio47 "$IDF_PATH" --python "$(command -v python)" --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

GitHub Actions runs board builds and host-side checks. Verify the board, partition layout, and image type before flashing a device.

## License

Licensing varies by file and component. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md); do not treat the whole repository or historical firmware as covered by one license.
