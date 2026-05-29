# Notification Sync

Notification Sync is a Pebble companion app and watch app for syncing Android notifications to Pebble watches.

This fork keeps the notification delivery and action engine from Pebble Notification Center 2, while reshaping the watch experience around a PebbleOS-style notification list, notification detail view, and action menu. The current watch UI work is focused on Emery/Pebble Time 2.

## Credits

Notification Sync is based on [Pebble Notification Center 2](https://github.com/matejdro/PebbleNotificationCenter2) by Matej Drobnič. The original project did the hard work of building a full Android-to-Pebble notification sync system with rich actions, rules, images, replies, snoozing, and offline notification history.

The PebbleOS-style notification UI direction and watch-side visual assets in this fork are derived from the open source [Core Devices PebbleOS](https://github.com/coredevices/pebbleos) project.

This fork is not an official Pebble, Core Devices, Rebble, or Pebble Notification Center release.

## What This Fork Changes

- Renames the app to Notification Sync.
- Reworks the watch app around a PebbleOS-style notification list, notification detail view, empty state, dismiss flow, and action menu.
- Keeps Notification Center actions such as dismiss, replies, voice replies, snooze, image display, mute, and open on phone.
- Modernizes the Android companion app UI around Material You-style settings and notification rules screens.
- Targets Android 16 for the companion app.
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

## License

This project follows the license of the original Pebble Notification Center 2 project. See [LICENSE](LICENSE).
