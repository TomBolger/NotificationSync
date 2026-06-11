# Notification Sync

Notification Sync is a Pebble companion app and watch app for syncing Android notifications to Pebble watches.

This fork keeps the notification delivery and action engine from Pebble Notification Center 2, while reshaping the watch experience around a PebbleOS-style notification list, notification detail view, and action menu. The current watch UI work is focused on Emery/Pebble Time 2.

## 2.0 Release

Notification Sync 2.0 is the first release of this fork's PebbleOS-style notification experience.

- Adds a stock-inspired watch notification list, detail screen, action menu, empty state, and notification launch flow.
- Keeps the live Android notification shade as the source of truth, including dismiss sync, replies, voice replies, snooze, mute, silence, open on phone, and image viewing actions where supported.
- Adds Android companion settings for interruption behavior, notification sorting/filtering, app and notification-channel control, unsupported emoji naming, and watchface-aware launch behavior.
- Uses platform-specific watch payload behavior so Emery-class watches can receive fuller detail/action payloads without forcing Basalt through the same memory budget.
- Preserves Basalt/Diorite support with a lighter inline action overlay and smaller detail cache.
- Tightens detail loading so Emery notification details wait for complete body/action data instead of showing truncated preview text.

## Credits

Notification Sync is based on [Pebble Notification Center 2](https://github.com/matejdro/PebbleNotificationCenter2) by Matej Drobnič. The original project did the hard work of building a full Android-to-Pebble notification sync system with rich actions, rules, images, replies, snoozing, and offline notification history.

The PebbleOS-style notification UI direction and watch-side visual assets in this fork are derived from the open source [Core Devices PebbleOS](https://github.com/coredevices/pebbleos) project.

This fork is not an official Pebble, Core Devices, Rebble, or Pebble Notification Center release.

## What This Fork Changes

- Renames the app to Notification Sync.
- Reworks the watch app around a PebbleOS-style notification list, notification detail view, empty state, dismiss flow, and action menu.
- Keeps Notification Center actions such as dismiss, replies, voice replies, snooze, image display, mute, and open on phone.
- Modernizes the Android companion app UI around Material You-style settings and notification rules screens.
- Adds per-app and per-notification-channel controls for mute, snooze, silence, and filtering.
- Adds configurable notification interruption behavior so new notifications can either open immediately or wait while the user is interacting with the watch.
- Parses unsupported Pebble emoji into readable `:emoji_name:` labels.
- Adds a watchface-aware launch option for deferring Notification Sync launches until the watch is back on a watchface.
- Targets Android 16 for the companion app.
- Supports Pebble SDK 3 watches on Basalt, Diorite, and Emery.
- Vendors the shared PebbleCommons code so the fork contains the watch-side sync fixes used by this branch.

## Project Layout

- `mobile/` - Android companion app.
- `watch/` - Pebble watch app.
- `PebbleCommons/` - Shared Pebble support code, vendored for this fork.
- `protocol.md` - Phone/watch protocol notes.
- `plan.md` - Current project plan and implementation notes.

## Building

### Android App

Open the `mobile/` directory in Android Studio or build from the command line with Gradle:

```sh
cd mobile
./gradlew assembleDebug
```

### Watch App

Build the Pebble watch app from the `watch/` directory with the Pebble SDK:

```sh
cd watch
pebble build
```

The watch package is configured as `Notification Sync` and the generated PBW is named `Notifications.pbw`.

## Release Artifacts

The 2.0 release ships two installable artifacts:

- Android companion APK: `notification-sync-mobile.apk`
- Pebble watch app: `Notifications.pbw`

Install both artifacts for the matching phone/watch protocol.

## License

This project follows the license of the original Pebble Notification Center 2 project. See [LICENSE](LICENSE).
