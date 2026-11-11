# Publishing on Google Play

What's already done in the app: its own package name (`io.github.tombolger.notificationsync`), target Android 16
(API 36), no `QUERY_ALL_PACKAGES`, no internet permission, a privacy policy (`PRIVACY.md`), an in-app disclosure
before Notification Access, 16 KB page-size support, and a CI step that builds the Play bundle (`.aab`).

## One-time setup

1. **Upload key.** On your computer: `keytool -genkeypair -v -keystore upload.jks -alias upload -keyalg RSA -keysize 2048 -validity 10000`.
   Keep the file and password safe. In GitHub → Settings → Secrets → Actions add:
   `UPLOAD_KEYSTORE_BASE64` (output of `base64 -w0 upload.jks`), `UPLOAD_KEYSTORE_PASSWORD`, `UPLOAD_KEY_ALIAS` (`upload`).
   Each CI run then attaches a signed `notification-sync-play-bundle` artifact.
2. **Play Console** ($25 developer account). Create the app, turn on Play App Signing, upload the `.aab` to Internal testing.
3. **Privacy policy URL:** `https://github.com/TomBolger/NotificationSync/blob/main/PRIVACY.md`.

## Forms in Play Console

- **Data safety:** "No data collected" and "No data shared". Data is processed only on the device and sent to the
  user's own watch, which Google's definitions don't count as collection.
- **App access:** no login needed; say a Pebble watch and the Pebble app are needed for the main features.
- **Content rating:** utility, no user-generated content shared.
- **Ads:** none.
- **Notification listener:** Google may ask why. Answer: it's a companion app that mirrors notifications to the
  user's smartwatch (an allowed core use).

## Store listing

- Name: avoid "Pebble" as the first word (it's a trademark); "Notification Sync for Pebble watches" style is fine.
- Credit the original project: based on Matej Drobnič's Notification Center 2 (Apache 2.0).
- Needs a 512×512 icon, a 1024×500 feature graphic and at least 2 phone screenshots.

## Before each release

Raise `versionCode` (and `versionName`) in `mobile/app/build.gradle.kts`.
