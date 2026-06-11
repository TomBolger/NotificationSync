# NotificationSync2 Native PebbleOS Notification Plan

## Paused Handoff - 2026-05-29

Status: paused by request. Do not continue the Core/root overlay path until explicitly resumed.

What was built:

- Patched local Core source in `/mnt/c/Users/tombo/Documents/core-mobileapp` on branch
  `NotificationSync2`.
- Root overlay test artifact in this repo:
  - `artifacts/core-notification-delete-overlay/core-patched.apk`
  - `artifacts/core-notification-delete-overlay.zip`
- Copies placed in Google Drive:
  - `I:\My Drive\core-patched.apk`
  - `I:\My Drive\core-notification-delete-overlay.zip`

Core patch summary:

- `LibPebbleNotificationListenerConnection` now sends notification removals to
  `libPebble.deleteNotification(...)` instead of `markNotificationRead(...)`.
- `LibPebble.deleteNotification(...)` calls `timelineNotificationsDao.markForDeletion(...)`.
- `TimelineNotification` now has `sendDeletions = true`.
- `TimelineNotification.windowAfterSecs` was set to `-1` so enabling deletes does not cause
  active notifications to age out after 24 hours.
- `:composeApp:assembleDebug` passed after adding a local dummy
  `/mnt/c/Users/tombo/Documents/core-mobileapp/composeApp/google-services.json`.

Important caveat:

- This is not an officially signed Core update. It is a root bind-mount overlay experiment intended
  to preserve the installed Core package data and pairing state.
- User does not want to reinstall or set up Core again.

Next step if resumed:

- Test live overlay on the rooted phone with the scripts in
  `artifacts/core-notification-delete-overlay/README.md`.
- If Android rejects the bind-mounted debug APK or behaves inconsistently, pivot to an LSPosed hook
  scoped to `coredevices.coreapp`.

Current pivot:

- Return to V1 companion app work.
- Add a companion-app toggle that suppresses watch sync and watchapp launch while the phone is
  unlocked.

## Goal

Move Notification Sync away from a foreground watchapp notification UI and toward the stock
PebbleOS notification pipeline:

- stock notification popup over any running watchapp
- stock Notifications app/list
- stock PebbleOS notification storage semantics
- Android-side filtering, mirroring, actions, and two-way cleanup retained

The most important behavior is exact lifecycle sync: if a notification disappears on the phone,
it must disappear from the watch's stock notification list without opening, closing, or relaunching
any watchapp.

## High-Confidence Mechanism

PebbleOS already has the deletion primitive we need.

Source evidence:

- `/tmp/pebbleos/src/fw/services/blob_db/notif_db.c`
  - `notif_db_delete()` calls `notification_storage_remove()` and then
    `notifications_handle_notification_removed()`.
- `/tmp/pebbleos/src/fw/services/notifications/notification_storage.c`
  - `notification_storage_remove()` marks the item as `TimelineItemStatusDeleted`.
  - normal lookup skips deleted notifications in `prv_find_next_notification()`.
  - iteration also skips deleted notifications.
- `/tmp/pebbleos/src/fw/popups/notifications/notification_window.c`
  - `NotificationRemoved` calls `prv_handle_notification_removed_common()`.
  - removal drops the id from `notifications_presented_list`; if the list is empty, the modal
    notification window is popped.
- `/tmp/pebbleos/src/fw/apps/system/notifications.c`
  - the stock Notifications app handles `NotificationRemoved` by removing the row from its app
    list and current notification window.

So the intended "dynamic cut" operation is:

1. Android notification is swiped away or cancelled.
2. Patched Pebble/Rebble Android service looks up the Pebble notification UUID for that Android
   notification key.
3. Service sends BlobDB DELETE to endpoint `0xb1db`, database `BlobDBIdNotifs` (`0x04`), key
   equal to the notification UUID.
4. PebbleOS marks that notification deleted and emits `NotificationRemoved`.
5. If the stock popup is showing it, the popup updates or closes.
6. If the stock Notifications app is open, its list row disappears.
7. If the stock Notifications app is opened later, deleted entries are skipped during load.

This is better than trying to simulate list state in our watchapp. The watch firmware already owns
the list and already knows how to remove one notification from it.

## Service Target Chosen

After inspecting the available service code, the best target is:

- `/mnt/c/Users/tombo/Documents/core-mobileapp`
- branch: `NotificationSync2`

Reason:

- `libpebble3` already has a native Android notification listener.
- It already converts Android notifications to `TimelineNotification`.
- It already inserts those notifications into BlobDB database `Notification`.
- It already receives stock Pebble timeline actions and maps them back to Android actions/replies/dismiss.
- It already tracks `StatusBarNotification.key -> LibPebbleNotification.uuid` while notifications are inflight.

This means the first working version does not require a new serializer or a new action bridge. The
critical missing behavior was delete semantics.

Implementation started in the service branch:

- `TimelineNotification` now has `sendDeletions = true`.
- Its notification age window is disabled (`windowAfterSecs = -1`) so enabling deletes does not
  accidentally auto-delete still-active stock notifications after 24 hours.
- Android notification removal now calls a real service-level `deleteNotification(itemId)` instead of
  `markNotificationRead(itemId)`.
- `deleteNotification(itemId)` marks the timeline notification row for deletion, which lets the
  existing BlobDB sync loop emit a BlobDB DELETE to the watch.
- A focused Android Kotlin compile of `:libpebble3:compileDebugKotlinAndroid` passed.
- The generated Room/KSP DAO now contains a real `dirtyRecordsForWatchDelete()` query for
  `TimelineNotificationEntity` and a `markForDeletion(itemId)` update.

This is a stronger result than the original Phase 0 shape: Core already had native insert/action
support, so the first code change can target the exact phone-swipe -> stock-list-removal gap.

## Root Overlay Test Package

Because the phone is rooted and preserving the existing Core install/setup is more important than
a normal reinstall, the first test artifact is a systemless-style overlay:

- Build output: `artifacts/core-notification-delete-overlay/core-patched.apk`
- Bundle: `artifacts/core-notification-delete-overlay.zip`
- Google Drive copies:
  - `I:\My Drive\core-patched.apk`
  - `I:\My Drive\core-notification-delete-overlay.zip`

The overlay does not uninstall Core and does not clear Core data. The live test script bind-mounts
the patched APK over the installed `coredevices.coreapp` APK path, then force-stops Core so Android
reloads it. A reboot removes the live mount. The bundle also includes a Magisk-style `service.sh`
for persistent reapplication after boot.

Test scope for this artifact:

- keep the current Core pairing/setup
- use stock PebbleOS notification popups/list
- verify that swiping a phone notification removes the matching stock Pebble notification
- verify no watchapp launch/relaunch is involved

Risks:

- Android may reject or ignore a bind-mounted APK on some builds if package scanning/signature
  state is stricter than expected.
- The debug APK is signed with a different key than the installed Core APK, so this is intentionally
  a root filesystem overlay test, not a normal package update.
- The first run after overlay may need a Core force-stop or device reboot to clear compiled code.

## Required Architecture

### 1. Notification Sync app remains the notification brain

Keep:

- Android `NotificationListenerService`
- existing rule/filter/mute decisions
- per-app preferences
- notification history/debug reporting
- Android action execution code
- reply/action mapping knowledge

Change:

- stop launching the watchapp for normal notification display
- send native-notification insert/update/delete requests to the patched Pebble/Rebble service

### 2. Patched Pebble/Rebble Android service becomes the native bridge

Add a private service API, probably Binder/AIDL or a PebbleKit2 extension, for:

- `upsertNativeNotification(payload)`
- `deleteNativeNotification(pebbleNotificationUuid)`
- `clearNativeNotificationsForSource(sourceId)` if bulk cleanup is needed
- action callback delivery from watch to Notification Sync

This code must run in the Android app that owns the Pebble system session, because PebbleOS marks
BlobDB notifications (`0xb1db`) and timeline actions (`0x2cb0`) as private endpoints.

### 3. Native notification serializer

The service must convert our payload into a PebbleOS `TimelineItem`:

- `TimelineItemTypeNotification`
- `LayoutIdNotification` or `LayoutIdCommNotification`
- deterministic Pebble UUID
- parent/source UUID
- timestamp
- title/subtitle/body/sender/app name attributes
- tiny/small/large icon resource ids
- optional vibration pattern attribute
- action group

Minimum viable payload should support:

- title
- body
- app name
- sender when available
- icon/category fallback
- dismiss/remove action

Full parity payload should support:

- reply
- canned responses
- custom Android actions
- open on phone
- notification grouping/replacement
- muting/filtering metadata if we want PebbleOS to reflect it

### 4. Stable identity map

Every Android notification key must map to a stable Pebble UUID.

Recommended:

- Generate UUID v5-style from a namespace plus Android `StatusBarNotification.key`.
- Store the mapping in Notification Sync and/or the patched service.
- Use the same UUID for updates and deletes.

Why deterministic IDs matter:

- Android may call `onNotificationRemoved()` after process death or service reconnect.
- Duplicate inserts with a different UUID would leave stale notifications on the watch.
- A deterministic UUID means delete can be reconstructed from the Android key.

Collision risk is negligible if we hash the full Android notification key into 128 bits.

### 5. Phone-to-watch lifecycle

Notification posted:

- Notification Sync parses and filters it.
- If allowed, call `upsertNativeNotification`.
- Patched service sends BlobDB INSERT to `BlobDBIdNotifs`.
- PebbleOS stores it and emits `NotificationAdded`.
- Stock popup/list handles it.

Notification updated:

- Use the same UUID.
- Prefer BlobDB INSERT with same key and a full replacement payload when the visible content
  changes.
- For pure status changes, PebbleOS already supports status updates when inserting an existing
  notification with status bits set, but full replacement behavior needs testing on hardware.

Notification removed on phone:

- Notification Sync receives `onNotificationRemoved`.
- Call `deleteNativeNotification(uuid)`.
- Patched service sends BlobDB DELETE to `BlobDBIdNotifs`.
- PebbleOS emits `NotificationRemoved`.

Clear all:

- Either send BlobDB DELETE for every known active UUID, or use BlobDB CLEAR only if we are certain
  it should delete every stock notification from every source. Per-UUID delete is safer.

### 6. Watch-to-phone lifecycle

Watch dismiss/action:

- Stock PebbleOS invokes the timeline action endpoint (`0x2cb0`) on the system session.
- Patched Pebble/Rebble service receives the action request.
- Service maps Pebble UUID back to Android notification key/action id.
- Service calls Notification Sync or directly performs the Android operation.
- Notification Sync executes:
  - cancel/dismiss if possible
  - send reply
  - trigger action `PendingIntent`
  - open app/phone action
- Service replies on the timeline action endpoint with success/failure.
- Android notification removal later triggers an idempotent BlobDB DELETE.

This gives true bidirectional sync:

- phone swipe removes watch notification
- watch dismiss removes or marks the phone notification when Android permits it
- watch action/reply can update both sides through the same UUID mapping

## Confidence

High confidence for phone swipe -> stock watch list removal.

Reason: PebbleOS has a direct delete path from BlobDB notification delete to
`NotificationRemoved`, and both the modal popup and stock Notifications app subscribe to that event.
Deleted entries are skipped by future notification storage reads.

Medium-high confidence for basic phone -> watch native notification mirroring.

Reason: BlobDB notification insert is exactly how phone-originated notifications enter stock UI.
The work is mostly serialization and access through the system Pebble service.

Medium confidence for full action parity.

Reason: PebbleOS action flow is clear, but Android actions vary wildly. Replies and simple actions
should work; edge cases around background activity launch limits, expired `PendingIntent`s, grouped
notifications, and app-specific actions need real-device testing.

Low confidence that this can ship as "only Notification Sync APK".

Reason: the needed endpoints are private to the Pebble/Rebble Android service. Unless that service
adds an API for us, a standalone third-party app cannot legally reach them through normal PebbleKit.

## Biggest Risks

- Distribution: users may need a patched Pebble/Rebble Android app or an upstream Rebble change.
- Serialization accuracy: PebbleOS timeline item binary format must be matched exactly.
- Action result protocol: watch expects responses on the private timeline action endpoint.
- Android permission limits: some dismisses/actions are not possible for every notification.
- Duplicate mirrors: if the regular Pebble app is also mirroring Android notifications, we must
  disable or coordinate with it to avoid double notifications.
- Multi-watch behavior: UUID mapping is global, but delivery/delete success is per watch.
- Reconnect/resync: after phone/service restart, active Android notifications must be reconciled
  against what the watch may still have.

## Implementation Phases

### Phase 0: Spike outside the product path

- Add a small test command in the patched Pebble/Rebble service that inserts one hard-coded native
  notification via BlobDB Notifs.
- Add a second command that deletes that UUID.
- Verify on hardware/emulator:
  - popup appears over an unrelated running app
  - stock Notifications app shows the item
  - delete removes it live from popup/list
  - delete remains gone after reopening Notifications

Do not touch Notification Sync's main flow until this passes.

### Phase 1: Minimal one-way mirror

- Deterministic Android key -> Pebble UUID mapping.
- Serialize title/body/app name/icon fallback.
- Insert on `onNotificationPosted`.
- Delete on `onNotificationRemoved`.
- Keep current watchapp path behind a fallback flag.

Success criterion:

- swiping a phone notification removes the exact stock Pebble notification quickly and reliably.

### Phase 2: Reconnect and replacement correctness

- On service start/reconnect, enumerate active Android notifications.
- Upsert missing active notifications.
- Delete watch notifications no longer active.
- Handle Android notification updates with same UUID.
- Add per-watch delivery state.

### Phase 3: Watch-to-phone dismiss

- Add native dismiss action id.
- Receive watch action via `0x2cb0`.
- Map UUID back to Android key.
- Cancel/dismiss on Android when permitted.
- Reply success/failure to PebbleOS.
- Make phone-side removal event idempotent.

### Phase 4: Replies and custom actions

- Add action-group serialization.
- Add canned/freeform reply support.
- Map action ids to Android actions.
- Preserve Android 16+ background activity launch handling for open-on-phone actions.

### Phase 5: Retire the watchapp path

- Remove normal notification launch behavior.
- Keep the watchapp only if needed for settings/debug/manual sync.
- Otherwise retire the PBW completely.

## Open Questions Before Coding

- Which Android Pebble/Rebble app/service source are we targeting for modification?
- Can we upstream this API, or is this a private fork?
- Does the target service already have BlobDB/timeline serializers we can reuse?
- How will users prevent duplicate stock Pebble notification mirroring?
- Do we want Notification Sync or the patched service to own persistent UUID mappings?
- What is our first test device/firmware target?

## Recommendation

Proceed only after a Phase 0 spike proves insert and delete against the stock PebbleOS notification
store. If Phase 0 passes, I am comfortable treating native stock notifications as the new direction.
If Phase 0 fails, the private endpoint/access assumptions are wrong and we should stop before
rewriting Notification Sync around it.

---

# Notification Sync 2.0 Hybrid Stock Handoff Plan

## Added Concept - 2026-06-07

Use stock PebbleOS notifications as the first interruption surface, but repurpose the stock
notification "Open on Phone" action as a deliberate handoff into the Notification Sync watchapp.

User-facing goal:

- A stock PebbleOS notification can appear without fully taking over the currently open watch app.
- If the user only wants to glance, dismiss, or ignore it, stock behavior remains lightweight.
- If the user wants richer Notification Sync behavior, they choose the stock notification's
  "Open on Phone" action.
- When the phone is locked, Notification Sync treats that action as "open this notification in
  Notification Sync on the watch" instead of launching the phone app.
- The watch transitions from the stock notification to the Notification Sync detail view for the
  same Android notification, with extra actions such as dismiss, reply, app-specific actions, and
  image viewing.

This is not a replacement for the full native-notification plan above. It is a lower-risk hybrid
mode that uses stock PebbleOS for low-friction interruption and keeps the Notification Sync watchapp
for high-power interactions.

## Evaluation

The idea is strong, but the implementation should not depend on detecting an arbitrary Android app
launch after the fact.

Why app-launch detection is the wrong primary hook:

- Android does not give a normal notification listener reliable, immediate, app-specific "this app
  was launched by Pebble's Open on Phone action from the lockscreen" events.
- Usage Stats or Accessibility could infer foreground app changes, but timing and attribution would
  be weak.
- Many "Open on Phone" actions resolve through `PendingIntent`s, app launchers, activities, or
  notification content intents differently per app.
- If we wait until after Android launches the target app, we have already lost the clean handoff
  point and may flash the phone UI unnecessarily.

The reliable hook is earlier:

- PebbleOS sends the stock notification action selection to the phone-side Pebble/Rebble/Core
  service.
- That service knows the Pebble notification UUID/action id and usually knows which Android
  notification it came from.
- A patched service or hook can decide: pass through normal Open on Phone behavior, or hand the
  action to Notification Sync.

Recommendation:

- Implement this as an explicit "Open on Phone handoff" action interception inside the patched
  Pebble/Rebble/Core notification action path.
- Use phone locked/unlocked state as the default policy:
  - locked: hijack into Notification Sync watch detail
  - unlocked: preserve stock Open on Phone behavior
- Add a user setting to override that policy.

## Required System Pieces

### 1. Stock Action Interceptor

Lives in the patched Pebble/Rebble/Core app, or in an LSPosed/root hook scoped to that app.

Responsibilities:

- Receive PebbleOS stock notification action callbacks.
- Identify whether the action is the stock Open on Phone action.
- Resolve the Pebble notification UUID back to the Android notification key.
- Check phone interactive/keyguard state.
- If policy says pass through, execute the original Open on Phone behavior unchanged.
- If policy says hijack, notify Notification Sync and suppress the original Open on Phone launch.

Suggested API from patched service to Notification Sync:

- `onStockOpenOnPhoneIntercepted(watchId, pebbleNotificationUuid, androidNotificationKey, actionId)`
- `deleteStockNotification(pebbleNotificationUuid)` after Notification Sync confirms handoff.

This should be Binder if both apps are installed normally. For quick root/hook experiments, an
explicit broadcast with a private signature permission is acceptable.

### 2. Notification Sync Handoff Receiver

Lives in the Notification Sync Android app.

Responsibilities:

- Receive the intercepted stock action event.
- Resolve `androidNotificationKey` to the existing `ProcessedNotification` / bucket id.
- If the notification is active but not yet synced to the watchapp bucket store, sync it first.
- Mark the next watchapp open as a notification-detail launch with the target bucket id.
- Start the Notification Sync watchapp.
- Prefer a no-vibration/no-new-notification path, because the user already initiated the handoff.
- Return an acknowledgement to the patched service once the watchapp launch/sync request has been
  queued.

Existing code to reuse:

- `WatchappOpenController.setNextWatchappOpenNotificationBucket(bucketId)`
- `WatchappOpenController.openWatchapp()`
- current bucket sync notification detail payload
- existing watch-side `APP_LAUNCH_PHONE` detail-opening path

Likely new code:

- `StockOpenOnPhoneHandoffController`
- `StockOpenOnPhoneHandoffReceiver` or Binder service endpoint
- preference key for handoff policy
- history/debug entry for intercepted handoffs

### 3. Watchapp Handoff Detail Path

Mostly reuse the current watchapp launch behavior.

Required behavior:

- Phone sends the target bucket id in the first sync/welcome packet.
- Watch opens the detail view for that bucket.
- If the detail was already restored before the phone packet arrives, preserve
  `detail_opened_from_phone_launch`.
- Back from that detail exits the watchapp without showing the list.
- Detail refreshes remain live for stacked/conversation updates.

Nice-to-have:

- Add a distinct "handoff launch" flag in the phone welcome packet instead of reusing only
  `APP_LAUNCH_PHONE`.
- That would let us test this mode in emulator and avoid ambiguity between true phone notification
  launch, app-list launch, and stock-action handoff launch.

### 4. Stock Notification Replacement / Cleanup

The handoff should feel like the stock notification became the Notification Sync notification.

Recommended sequence:

1. User selects Open on Phone from the stock notification.
2. Patched service intercepts and sends handoff event to Notification Sync.
3. Notification Sync queues bucket sync and starts the watchapp to the target bucket.
4. Watchapp opens the matching detail.
5. Notification Sync sends a handoff-open acknowledgement to patched service.
6. Patched service deletes or dismisses the stock PebbleOS notification from BlobDB.

Why delete after acknowledgement:

- Deleting before the watchapp opens may make the stock popup disappear into a blank/launcher frame.
- Deleting too late leaves duplicate entries in the stock Notifications list.
- Acknowledgement gives us the best chance of a smooth visual replacement.

Fallback:

- If the watchapp does not acknowledge within a short timeout, keep the stock notification and
  optionally let the original Open on Phone action pass through.

### 5. Settings

Add a Watch Experience setting group:

- `Stock Open on Phone handoff`
  - Off
  - When phone is locked
  - Always
- `After handoff`
  - Remove stock notification from watch
  - Keep stock notification
- `Fallback if handoff fails`
  - Open on phone normally
  - Do nothing

Default recommendation:

- Handoff: When phone is locked
- After handoff: Remove stock notification from watch
- Fallback: Open on phone normally

## Implementation Phases

### Phase 0: Prove Action Interception

Goal: confirm the patched Pebble/Rebble/Core service can identify and intercept stock Open on Phone
without launching the Android target app.

Tasks:

- Add logging around stock notification action callbacks.
- Press Open on Phone on a stock Pebble notification.
- Capture:
  - Pebble notification UUID
  - action id/type
  - Android notification key if available
  - phone locked/unlocked state
- Suppress original Open on Phone in a test build and verify the phone app does not launch.

Exit criterion:

- We can intercept Open on Phone before Android launches the target app.

### Phase 1: Fire Notification Sync Handoff

Goal: pressing stock Open on Phone starts Notification Sync watchapp detail for the same
notification.

Tasks:

- Add patched-service -> Notification Sync handoff event.
- Add Notification Sync receiver/controller.
- Resolve Android notification key -> bucket id.
- Start watchapp with `setNextWatchappOpenNotificationBucket(bucketId)`.
- Add a temporary debug log/Toast/history row for handoff attempts.

Exit criterion:

- From a stock notification, Open on Phone opens the matching Notification Sync detail on the watch.

### Phase 2: Make Replacement Seamless

Goal: remove the stock notification only after Notification Sync has taken over.

Tasks:

- Add watchapp/phone acknowledgement that the target detail was opened or at least queued.
- Trigger stock BlobDB delete from patched service after acknowledgement.
- Tune timeout and fallback behavior.
- Verify no stock list flash and no duplicate stock/watchapp entries remain.

Exit criterion:

- User sees stock notification transition into Notification Sync detail with minimal visual churn.

### Phase 3: Action Parity and Images

Goal: justify the handoff by exposing richer Notification Sync actions.

Tasks:

- Ensure dismiss action cancels phone notification and cleans stock watch notification.
- Ensure replies/actions still map to current Android notification actions.
- Ensure Show Image action works from handoff-opened details.
- Preserve dictation behavior on Basalt/Emery.

Exit criterion:

- Handoff gives real extra capability over stock Open on Phone.

### Phase 4: Preferences and Polishing

Goal: make the feature safe enough for tester builds.

Tasks:

- Add settings UI.
- Add per-app/rule override if needed.
- Add history/debug logging for handoff outcomes.
- Add tests for locked/unlocked policy and fallback behavior.
- Add a quick recovery path if the target bucket is missing/stale.

Exit criterion:

- Feature can ship behind a disabled-by-default or locked-phone-only toggle.

## Main Risks

- Requires patched Pebble/Rebble/Core service or hook. A standalone Notification Sync APK probably
  cannot intercept stock Open on Phone actions.
- Mapping Pebble stock notification UUID back to Notification Sync bucket id may require shared
  deterministic UUID generation or a bridge table in the patched service.
- If the original stock notification was generated by Core and our bucket notification was generated
  separately, duplicate identity can drift unless both systems use the same Android notification key.
- Timing is delicate: deleting the stock notification too early causes visual gaps; too late causes
  duplicates.
- Some notifications may not have a meaningful Open on Phone action, or Android may restrict the
  original action in ways that vary by app/version.

## Recommended 2.0 Direction

Proceed with this as the first practical 2.0 hybrid mode, ahead of a full native-notification
replacement.

It gives us the main UX benefit of stock notifications:

- lightweight interruption
- stock notification list/popup behavior
- no full watchapp takeover unless the user asks for it

It keeps Notification Sync valuable:

- richer actions
- image viewing
- controlled dismiss/reply behavior
- existing rules/history/settings

The first engineering spike should be action interception in the patched Pebble/Rebble/Core service.
If that cannot be intercepted before Android launches the target app, stop and reassess before
building Notification Sync-side handoff code.
