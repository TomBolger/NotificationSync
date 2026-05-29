# PebbleOS Notification Center Replacement Plan

## Goal

Replace Notification Center's watch UI completely. The watch app should open to a PebbleOS-style Notifications app, not to Notification Center's original full-screen notification reader.

Notification Center remains only the transport and action engine:

- Android companion supplies notifications from Android.
- Existing mute, filtering, vibration, sync, image, and action execution still run on Android.
- Watch UI, navigation, iconography, colors, fonts, and action presentation should come from PebbleOS conventions.

## Non-Negotiable Requirements

- Do not preserve old Notification Center watch UI patterns.
- Do not keep the original status/dot chrome, old scrolling detail screen, or old button behavior.
- Use PebbleOS notification assets wherever the SDK app can carry them.
- Keep Android launcher icons out of the watch UI; map Android packages/app labels to bundled PebbleOS line assets instead.
- Use PebbleOS-style notification list as the first screen.
- Selecting a list item opens a PebbleOS notification detail clone.
- Middle/select in the detail opens a PebbleOS-consistent action menu populated with Notification Center actions.
- App icons must be PebbleOS line-art timeline notification icons, not streamed Android launcher photos.
- Companion settings that still matter must continue to affect watch behavior.
- Settings made irrelevant by the PebbleOS UI should be ignored or later removed from the companion UI.

## PebbleOS Source To Reuse

Primary references:

- `/tmp/pebbleos/src/fw/apps/system/notifications.c`
- `/tmp/pebbleos/src/fw/popups/notifications/notification_window.c`
- `/tmp/pebbleos/src/fw/popups/notifications/notification_window_private.h`
- `/tmp/pebbleos/src/fw/services/timeline/notification_layout.c`
- `/tmp/pebbleos/include/pbl/services/timeline/notification_layout.h`
- `/tmp/pebbleos/include/pbl/services/notifications/ancs/ancs_known_apps.h`
- `/tmp/pebbleos/resources/normal/base/images/Pebble_*_notification.svg`
- `/tmp/pebbleos/tests/test_images/test_notification_window__*.png`

Firmware-only code cannot be linked into a third-party SDK app, so port the behavior and visuals into SDK-compatible layers, draw code, PDC assets, and `ActionMenu`.

## Companion Protocol

Bucket data must provide enough identity for the watch list before a details fetch:

- timestamp
- PebbleOS icon id
- PebbleOS color id
- app name
- sender/title
- content snippet
- body preview

The watch should not require streamed Android app icons for the list or detail notification chrome.
Current companion mapping covers the core PebbleOS set plus Instagram, Slack, LinkedIn, Amazon, Maps, Photos, Calendar, Google Messages, Outlook, Skype, Snapchat, Line, WeChat, Kik, Viber, KakaoTalk, BBM, Yahoo Mail, Weather, Music, Location, Reminder, Warning fallback categories, Discord, Teams, Google Chat, Signal, Reddit, YouTube, Zoom, Twitch, Google Tasks, and Tesla via PebbleOS car-rental fallback art.

Telegram must be matched before Messenger so `org.telegram.messenger` does not resolve to the Facebook Messenger asset.

## Watch Screens

### Notification List

- First screen after launch.
- PebbleOS Notifications app feel.
- Multiple notifications visible at once.
- 25x25 PebbleOS line-art icon at left.
- Large sender/title text.
- Smaller content snippet.
- PebbleOS colors for known apps.
- PebbleOS source SVG/PDC tiny icons loaded up front, with the shared tiny-icon draw data normalized once at load time to the stock PebbleOS 2px tiny-icon line weight.
- Up/down scroll list selection.
- Use Pebble SDK `MenuLayer` click behavior directly, matching PebbleOS: first rows pin to the top, middle rows center while scrolling, and bottom rows stop the viewport while selection moves down.
- Do not use spacer rows, center-focused mode, or custom up/down handlers.
- Select opens detail.
- Back exits app.

### Notification Detail

- Clone PebbleOS notification popup layout.
- Full display white content.
- Top banner in PebbleOS app color.
- Centered time and PebbleOS icon.
- Top-right PebbleOS status text is the notification position counter, such as `1/27`, not a date.
- App name, notification title/sender, body using PebbleOS font hierarchy.
- Right-edge black semicircle/action cue aligned exactly to the display edge.
- Right-edge action cue is a fixed root-layer sibling, not content drawn inside the scroll layer.
- Up/down scroll detail content through SDK `ScrollLayer` behavior.
- Disable ScrollLayer shadows so the old shaded top/bottom overlays do not appear.
- Back returns to list.
- Select opens action menu.
- Pressing down at the bottom of a detail notification advances to the next notification in the list.
- Pressing up at the top of a detail notification moves to the previous notification in the list.
- Within the body content, up/down keep the stock Pebble scroll behavior.

### Action Menu

- Use SDK `ActionMenu` because PebbleOS firmware action internals are not linkable.
- Color it from the same PebbleOS app color.
- Populate it only with Notification Center action choices.
- Preserve normal actions, submenu actions, and voice/freeform actions.

## Empty State

- Do not show dummy notifications in production.
- When no synced notifications exist, the watch notification list must be empty.
- Any future sample/demo notifications must be behind an explicit dev-only switch or external test build, never the default watch app behavior.

## Verification

- Build PBW.
- Build APK.
- Run watch app in Emery emulator.
- Verify the app opens to the list, not the old full-screen reader.
- Verify first item is pinned to the top with no blank spacer above.
- Verify two down clicks select the third item and the list naturally centers as PebbleOS does.
- Verify bottom-of-list behavior stops the viewport and lets the selection move toward the bottom.
- Verify selected notification opens a PebbleOS-style detail with a fixed right-edge action cue.
- Verify action menu opens and uses Notification Center actions.
- Verify Android module and full APK compile.

## Current Pass Notes

- Field-test fixes now in scope:
  - Remove all automatic dummy testing data from the no-notifications watch list.
  - Fix voice replies so dictated text is sent instead of silently failing.
  - When a watch action removes a notification, avoid stale/blank entries and replace the current view with a PebbleOS-style dismiss transition, then return to the list or close the app based on how the notification was opened.
  - Closing the app should not show the legacy `Closing...` status screen.
  - Add a notification timeout, defaulting to 10 minutes.
  - If timeout configuration requires Android work, add a companion setting in the Android app during the same pass.
- Watch detail now uses PebbleOS notification popup constants for the banner height, icon frame, body spacing, footer, right-edge cue, and scroll arrow.
- Watch list uses SDK `MenuLayer` selection behavior so top rows pin at the top, middle rows naturally center, and bottom rows stop the viewport before the final selection moves down.
- Detail up/down uses an SDK-compatible port of PebbleOS `SwapLayer` scroll math, then swaps in the adjacent notification at the PebbleOS boundary and keeps the notification counter/list selection synchronized.
- Detail up/down now ports the relevant PebbleOS `SwapLayer` behavior: 36px initial banner scroll, 48px normal scroll, 24px held-button scroll, the 36px bottom peek for the next notification, and the PebbleOS edge-delay before held-button swaps.
- Short notifications are intentionally given the PebbleOS next-notification peek space, so the first down action reveals the next banner and the next boundary action advances to that notification.
- Detail notification swaps now animate with the PebbleOS 200ms ease-out vertical swap motion before committing the new selected notification.
- Watch metadata is renamed to `Notification Sync`, the launcher icon is generated from the supplied PebbleOS generic notification SVG, and the build leaves a named bundle at `watch/build/Notifications.pbw`.
- Live Android sync now sends PebbleOS icon/color ids for the additional app set and strips repeated sender prefixes from group chat bodies before the watch receives them.
- This pass removes production dummy notifications entirely, keeps bucket-delete callbacks live while a detail notification is open, and animates deleted current notifications out before returning to the list or closing the phone-launched app.
- Voice replies now keep a stable copy of the selected voice action through dictation so the dictated text can be sent after the action menu closes/freezes.
- The legacy `Closing...` status window is removed from normal close, and phone-launched notifications now default to a 10 minute auto-close timeout that can be edited from the Android Settings tab.
- The delete/remove animation should use PebbleOS's real `RESULT_DISMISSED_LARGE` animated PDC (`Pebble_80x80_Dismiss.pdc`) as a full-screen white overlay, not the previous slide-away approximation.
- Android 16 blocks background activity launches from notification `PendingIntent`s unless the sender opts in; action sending now needs to provide `ActivityOptions.setPendingIntentBackgroundActivityStartMode(...)` so `Open on phone` can launch the source app without crashing.
- Voice reply must preserve both the selected action and the selected notification id while Pebble dictation is active, because the action menu/detail state may move or close before the transcription callback fires.
- Voice reply regression root cause: `dictation_session_start()` returns `DictationSessionStatus`, and `DictationSessionStatusSuccess` is zero. Upstream stored it in a bool but left the session alive; this fork cleared the voice state and destroyed the session on that zero success value. The watch must compare against `DictationSessionStatusSuccess` directly.
- Reply `PendingIntent` fill-in construction should match upstream Notification Center: populate `RemoteInput.EXTRA_RESULTS_DATA` and attach it as `RemoteInput.RESULTS_CLIP_LABEL` clip data. Keep the Android 16 BAL fix scoped to generic/open-phone actions.
- The APK builds successfully from `mobile` with `./gradlew :app:assembleDebug`.
- The PBW builds successfully from `watch` with `pebble build`.
- Voice-path regression checks pass for the watch action packet carrying dictated text and for the submenu handler forwarding dictated text to the reply action.
- When the watch notification list is empty, show PebbleOS-style centered `No Notifications` text instead of a blank white screen.
