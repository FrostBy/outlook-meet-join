# Research notes

Findings from reverse engineering new Outlook for Windows (`olk.exe` 1.2026.922.300, WebView2, Windows 11) with a Google Workspace account connected. They explain why the code looks the way it does.

## Loader

- The Application Verifier technique still works: `VerifierDlls` + `GlobalFlag=0x100` in IFEO loads the DLL before Outlook code runs.
- `CreateCoreWebView2EnvironmentWithOptions` is imported by `nh.dll`; the export of `WebView2Loader.dll` is patched as well, so the hook does not depend on which module imports it.
- Every window (main window, pop-out event window) gets its own WebView2 controller in the same process. Each one is injected; there is no controller limit.
- `inject.js` is read on every injection, so new windows pick up an updated payload without restarting Outlook.
- Outlook updates from the Microsoft Store have been reported (valinet/NewOutlookPatcher#19) to remove the IFEO key. Not observed here. `install.ps1 -Action Status` reports it; run `Install` again.

## DOM

- Pop-out event window: the body is not in an iframe. The toolbar is `div[role="toolbar"][data-app-section="Toolbar"]` (Fluent UI), the event body is inside `[data-app-section="Form_Content"]`.
- Calendar peek card: `[data-app-section="CalendarItemPeek"]` inside a Fluent layer portal. It shows subject, time and organizer. **It never contains the event body**, so the Meet link is not in its DOM.
- Calendar grid tiles carry `data-calitemid` (Exchange item id) and an `aria-label` that starts with the subject.
- Hashed Griffel class names are never used as selectors; they are build artifacts and likely to change.

## Where the event body comes from

The body (and the Meet link) does not travel through any page-level network API:

| Channel | Carries the event body |
|---|---|
| `fetch` / `XMLHttpRequest` | no |
| `WebSocket` | no (none opened) |
| `chrome.webview` host messages | no |
| `MessagePort` messages | **yes** |

A `ClientCalendarEvent` object reaches the page through a `MessagePort` message. Outlook runs several dedicated workers; the sender is most likely its data worker, but that was not confirmed. It contains `Body.Value` (HTML with the Meet link), `Subject`, `UID`, the item id and `mailboxInfo.mailboxSmtpAddress`. The payload listens to those messages passively and caches the link. It sends no requests of its own.

The item id in that object equals `data-calitemid` of the calendar tile.

## Known limitation

The body is only sent to the page when Outlook loads it, which happens when an event is opened. The peek card does not load it. So:

- an event opened at least once shows Join in its peek card;
- a recurring meeting shows Join on every occurrence after any one occurrence was opened (the cache also keys by subject);
- a one-off meeting that was never opened has no Join in its peek card.

Requesting the body ourselves was investigated and dropped: `translateExchangeIds` returned 400 and `/api/v2.0/me/events/{id}` returned 404 for this account, and no replayable request carrying the event id was found in page-level traffic, `MessagePort` or `Worker.postMessage`.

## Google account selection

`?authuser=<email>` on a Meet URL opens it with that Google account when several are signed in. The email is taken from `mailboxInfo` of the same event.
