# Outlook Meet Join

Adds a **Join** button for Google Meet meetings to the new Outlook for Windows.

For Google Meet invitations, new Outlook shows no Join button, only a link buried in the event body. This tool puts a Join button:

- next to the event toolbar when an event is opened in its own window;
- on the calendar peek card that appears when you click an event in the grid.

When several Google accounts are signed in to your browser, Join opens the meeting with the account the invitation was sent to (`?authuser=<email>`).

## Requirements

- Windows 11 x64 (Windows 10 is untested)
- New Outlook for Windows (`olk.exe`), not classic Outlook
- Administrator rights for install and uninstall

## Install

1. Download `OutlookMeetJoin.zip` from [Releases](../../releases) and extract it.
2. Run `install.cmd` and confirm the elevation prompt.

The installer closes Outlook, copies the DLL to `C:\Windows\System32`, registers it for `olk.exe` and starts Outlook again.

Other actions:

```
install.cmd Status
install.cmd UpdatePayload
install.cmd Uninstall
```

`Uninstall` restores the previous registry values, removes the DLL and `%LOCALAPPDATA%\OutlookMeetJoin`.

> The DLL is not code-signed, so SmartScreen or an antivirus may warn about it. The source is in `src/loader`, and every release is built from scratch by GitHub Actions.

## How it works

Two parts:

- **Loader** (`src/loader`, C++). Registered as an Application Verifier DLL for `olk.exe`, so Windows loads it before Outlook starts. It hooks WebView2 environment creation and injects `inject.js` into every Outlook window. It contains no Join logic.
- **Payload** (`src/payload/inject.js`). Listens to the event data Outlook passes to the page through `MessagePort` messages, caches the Meet link of each event and renders the Join button.

The tool makes **no network requests** and uses no Google or Microsoft APIs. It only reads data Outlook already loads. See [docs/research.md](docs/research.md) for the details.

## Limitations

- The peek card shows Join only for events whose body Outlook has already loaded, which happens when you open the event once. For recurring meetings, opening any one occurrence is enough. A one-off meeting that was never opened has no Join in its peek card; open the event and Join is there.
- Outlook updates from the Microsoft Store have been reported to remove the registration. If Join disappears, `install.cmd Status` tells you; run `install.cmd` again.
- The Join button next to the toolbar is tested in the separate event window only. The reading pane is untested.
- Install as the same Windows user who runs Outlook. The payload is stored in that user's `%LOCALAPPDATA%`.
- Uninstall does not clear the small link cache stored in Outlook's own web storage (`omjv1:` keys). It is harmless and is not used without the loader.

## Troubleshooting

- `install.cmd Status` shows the registration, files and the last lines of `%LOCALAPPDATA%\OutlookMeetJoin\loader.log`.
- If Outlook does not start after install, run `install.cmd Uninstall`.

## Build from source

Requires Visual Studio 2022 Build Tools with the C++ workload. The WebView2 SDK is downloaded automatically.

```
build\build.cmd
```

The result is in `dist\`. Payload tests run with Node.js:

```
node tests\payload.test.js
```

## Credits

The loader is a modified version of [alex94we/outlook-ads-remover](https://github.com/alex94we/outlook-ads-remover), which follows the technique of [valinet/NewOutlookPatcher](https://github.com/valinet/NewOutlookPatcher). See [NOTICE](NOTICE).

## License

[GPL-3.0](LICENSE)
