# Handoff: run the PebbleOS test suite for the notification-mirroring branch

You are picking this up on **Monolith**, in this folder (`Documents/PebbleOS-NS`). An earlier
cloud session wrote the code; it could not reach this computer, so the full test run has to happen
here. The owner of the work is Tom (GitHub `TomBolger`).

## Background

Tom maintains **Notification Sync**, a Pebble watch app plus an Android companion that mirrors the
phone's notification shade on the watch. The goal is to move that behavior into **stock PebbleOS**
(the Notifications app) and upstream it to `coredevices/pebbleos`.

Upstream already hides notifications removed on the phone while connected (the `notifDismissalSync`
preference, off by default and turned on by the Core phone app). The branch adds the missing parts:

| # | Commit (on branch `notifications-mirror-phone`) | What it does |
| --- | --- | --- |
| 1 | `services/notifications/ancs: keep the notification center when mirroring it` | With dismissal sync on, keeps pre-existing iOS notifications after a reconnect, whatever their age (alerts only for ones missed while away). Adds `ancs_notifications_handle_snapshot()`, which drops stored iOS notifications whose UID the phone no longer reports. A new notification replaces a stored one with the same UID. |
| 2 | `comm/ble/ancs: drop notifications removed from iOS while disconnected` | For 10 s after subscribing to the Notification Source, the ANCS client collects the UIDs iOS re-sends, then calls the snapshot handler. If it overflows (128 UIDs), an event was dropped, or the session ends, nothing is removed. |
| 3 | `services/notifications: add a preference to group notifications by app` | Adds the `notifGroupByApp` pref, off by default. |
| 4 | `apps/notifications: stack notifications by app` | Groups the history by the `AppName` attribute, for both iOS and Android. An opened stack shows the sender and text of each notification. |
| 5 | `apps/settings: add a Group by App notification setting` | Adds a toggle in Settings → Notifications. |

- **Repo:** https://github.com/TomBolger/PebbleOS-NS (a GitHub fork of `coredevices/pebbleos`).
- **Base:** `main` at `e8705dfcd`. **Branch:** `notifications-mirror-phone`, 5 commits on top of `e8705dfcd`.
- A companion `libpebble3` change (sending `AppName` for Android notifications) is in the
  NotificationSync repo under `pebbleos-native/patches/libpebble3/`. It is **not** part of this task.

### Already verified in the cloud (Linux x86_64)

- These suites pass on the branch: `test_ancs`, `test_ancs_notifications`,
  `test_notifications_history`, `test_notif_db`, `test_notification_storage`,
  `test_alerts_preferences`, `test_ancs_pebble_actions`.
- The `qemu_emery` firmware builds with no warnings, and Group by App was checked in the emulator.
- In the full suite, 110 of 439 tests failed. All of them are screen-rendering tests (graphics, health
  cards, music, launcher, notification window, timeline, workout and others). The cloud test build
  was configured with `-DPBL_TEST_IMAGES=OFF`, so their reference images and fonts were missing.
  **This run exists to settle whether those failures were only that.**

## Your task

1. Set up the PebbleOS build environment on this machine.
2. Run the **full** unit-test suite on the branch, with images on (the default).
3. Run the same suite on `main` (`e8705dfcd`) for comparison.
4. Report the results as described below.

Do not push, open PRs, rewrite commits, or change the code unless Tom asks. If a test fails
because of the branch, report it with the output. Do not fix it on your own.

## Steps

### 0. Platform

PebbleOS builds on Linux and macOS. **On Windows, work inside WSL2 (Ubuntu)**: clone into the WSL
filesystem (e.g. `~/pebbleos-ns`, not `/mnt/c/...`, which is very slow), and tell Tom the results
live there. If WSL isn't installed, stop and ask Tom before installing it.

### 1. Dependencies

Follow `docs/getting_started/index.md` in the repo. In short:

- **Ubuntu/WSL:**
  `sudo apt install bison clang cmake device-tree-compiler flex gcc gcc-multilib gettext git gperf libfreetype6-dev libglib2.0-dev libgtk-3-dev libncurses-dev librsvg2-bin libsdl2-dev make nodejs pkg-config python3-dev python3-venv`
- **macOS:** `brew install python git`, then after cloning, `brew install $(cat requirements-brew.txt)`.

The unit tests build with the host's `clang` and do not need the ARM toolchain. Only install the
PebbleOS SDK (from the getting-started doc) if Tom also asks for a firmware or emulator build.

### 2. Clone

```sh
git clone -b notifications-mirror-phone https://github.com/TomBolger/PebbleOS-NS.git pebbleos-ns
cd pebbleos-ns
git log --oneline -6        # expect the 5 commits above on top of e8705dfcd
git submodule update --init --depth 1
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
```

If one submodule fails to clone (`third_party/moddable/moddable` is large and flaky), remove its
directory and the matching `.git/modules/...` entry, then retry that one submodule on its own.

### 3. Run the suite on the branch

```sh
pbl test -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" 2>&1 | tee ../branch-test.log
```

This configures `build-test/` with the image fixtures on, builds about 440 test binaries, and runs
them. A clean build takes a long time; leave it running. Failed-render images land in
`build-test/failed/`.

### 4. Run the suite on main

```sh
git checkout e8705dfcd
pbl test -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" 2>&1 | tee ../main-test.log
git checkout notifications-mirror-phone
```

### 5. Report back

Give Tom a short report:

- The OS, and whether you used WSL.
- Pass and fail counts for the branch and for `main`.
- Tests that **fail on the branch but pass on `main`**. These are the ones that matter. For each,
  include the failing assertion and its output (`ctest --test-dir build-test -R <name> --output-on-failure`).
- Tests that fail on both. These are pre-existing or environment problems, so list the names only.
- Anything you had to work around during setup.

The tests most related to the branch are `test_ancs`, `test_ancs_notifications`,
`test_notifications_history`, `test_alerts_preferences`, `test_notif_db`,
`test_notification_storage` and `test_notification_window_*`. Call out their status explicitly.

## For reference after the tests (not part of this task)

Tom still has to do these before opening upstream PRs:

- Test on a real watch with an iPhone (clear notifications while out of range, then reconnect;
  reboot the watch; a full Notification Center; an iPhone reboot), and take photos.
- Re-sign the commits with his full name and real email:
  `git rebase e8705dfcd --exec 'git commit --amend --no-edit --reset-author -s'`, then
  `gitlint --commits e8705dfcd..HEAD`.
- Post the discussion issue, then open two PRs (commits 1–2, then 3–5). The drafts are in
  NotificationSync `pebbleos-native/PR_DRAFTS.md`.
