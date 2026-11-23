# Drafts for upstream

Review these, edit them into your own words, and fill in the hardware results before you post.
Both projects require the PR text to say that AI was used.

---

## 1. Discussion issue (coredevices/pebbleos): post first

**Title:** Mirror the phone's notification shade: iOS reconnect sync and stacking by app

With dismissal sync on, the stock Notifications app already follows live removals from both
phones. Two gaps remain before it can stand in for a shade-mirroring app (I maintain one,
Notification Sync):

1. **iOS across disconnections.** Pre-existing ANCS notifications from before the disconnection
   are dropped. Anything cleared on the iPhone while the watch was away never leaves the watch.
   After a watch reboot the history starts empty, even though Notification Center isn't.
2. **Organization.** Phones stack notifications by app. The watch has Group by Sender, but only
   for Android and only within an age range.

Proposal, all gated on `notifDismissalSync` except the new setting:

- After subscribing to the NS, collect the UIDs iOS re-sends for 10 s. Then drop stored iOS
  notifications whose UID is missing. If the snapshot overflows or an event is dropped, delete
  nothing.
- Store recovered notifications whatever their age, without alerting. Only ones missed during
  the disconnection alert, as today.
- Add a `notifGroupByApp` setting that stacks the history by the `AppName` attribute, for iOS and
  Android. libpebble3 would start sending `AppName` for Android notifications (a 6-line change).

I have this as two small PRs (about 190 and 140 lines including tests). Would you take them? Do
you prefer gating on dismissal sync or a separate setting?

---

## 2. PR (coredevices/pebbleos): iOS mirroring

**Title:** ancs: keep the iOS notification center in sync across disconnections

**Commits:**
- services/notifications/ancs: keep the notification center when mirroring it
- comm/ble/ancs: drop notifications removed from iOS while disconnected

**What**

With notification dismissal sync on, the watch is supposed to show what is in the phone's
notification shade. On iOS, removals only arrive while connected, and pre-existing notifications
from before a disconnection were ignored. This PR:

- stores recovered notifications whatever their age when mirroring, alerting only for ones
  missed while disconnected (unchanged);
- has the ANCS client collect the UIDs iOS re-sends for 10 s after subscribing to the
  Notification Source, then drops stored iOS notifications that are no longer on the phone
  (in batches, with no allocation);
- abandons the snapshot, deleting nothing, if more than 128 UIDs arrive, if a notification event
  was dropped on its way to KernelMain, or if the session ends first;
- fetches pre-existing notifications on the first connection after boot too, so the history
  starts out matching Notification Center;
- replaces a stored notification that has the same ANCS UID as a new one (UIDs start over after
  an iOS restart).

Nothing changes with dismissal sync off. Android/BlobDB notifications are never touched.

**Testing**

- Unit: 6 new `test_ancs` cases and 2 new `test_ancs_notifications` cases. `pbl test` passes.
- Hardware: _<watch model, firmware build, iPhone model + iOS version>_
  - Cleared N notifications on the iPhone while out of range, then reconnected: _<result/log>_
  - Rebooted the watch with N in Notification Center: _<result>_
  - Notification Center with 30+ items: _<result>_
  - iPhone reboot, then reconnect: _<result>_
- Power: _<only extra work is at most one storage scan per reconnect; measurement if asked>_

AI was used to help write this change (`Assisted-by: LLM` on every commit). I reviewed and tested
all of it.

---

## 3. PR (coredevices/pebbleos): Group by App

**Title:** apps/notifications: stack notifications by app

**Commits:**
- services/notifications: add a preference to group notifications by app
- apps/notifications: stack notifications by app
- apps/settings: add a Group by App notification setting

**What**

Adds Settings → Notifications → **Group by App** (off by default). When it is on, the history is
stacked by the `AppName` attribute for iOS and Android notifications, regardless of age. This
takes precedence over Group by Sender. Notifications without an app name stay on their own rows.
An opened stack lists the sender and text of each notification, because the stack's title is
already the app's name.

Android notifications need `AppName` from the phone app: coredevices/libpebble3#_<n>_.
iOS notifications already carry it.

**Before / after** (qemu_emery; replace these with photos from a watch)

_<attach screenshots/1..4>_

**Testing**

- Unit: 3 new `test_notifications_history` cases.
- Emulator: `pbl emulator feed notifications`, then toggled the setting and walked the list and
  stacks.
- Hardware: _<watch, Android + iPhone results>_
- Third-party apps: no public API changed.

AI was used to help write this change (`Assisted-by: LLM` on every commit). I reviewed and tested
all of it.

---

## 4. PR (coredevices/libpebble3)

**Title:** Send the app name with Android notifications

Adds the `AppName` timeline attribute to Android notifications, as iOS notifications already
have. This lets the watch stack notifications by app (coredevices/pebbleos#_<n>_).

Side effect: when an Android notification uses the generic icon, the watch's notification list
already shows the app's name in place of the title, as it does for iOS.

Testing: _<built the Android app, checked notifications on a watch running the firmware PR and
on current stock firmware>_

AI was used to help write this change. I reviewed and tested it.
