package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.annotation.VisibleForTesting
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import com.matejdro.bucketsync.BucketSyncRepository
import com.matejdro.bucketsync.BucketSyncRepository.Companion.MAX_BUCKET_ID
import com.matejdro.pebble.bluetooth.common.util.LimitingStringEncoder
import com.matejdro.pebble.bluetooth.common.util.fixPebbleIndentation
import com.matejdro.pebble.bluetooth.common.util.writeUByte
import com.matejdro.pebble.bluetooth.common.util.writeUInt
import com.matejdro.pebble.bluetooth.common.util.writeUShort
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.notification.model.any
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.RuleOption
import com.matejdro.pebblenotificationcenter.rules.keys.get
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dispatch.core.DefaultCoroutineScope
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.launch
import logcat.logcat
import okio.Buffer
import kotlin.experimental.or
import kotlin.time.Duration.Companion.milliseconds

@Inject
@ContributesBinding(AppScope::class)
class WatchSyncerImpl(
   private val bucketSyncRepository: BucketSyncRepository,
   private val preferenceStore: DataStore<Preferences>,
   private val defaultScope: DefaultCoroutineScope,
) : WatchSyncer {
   private val utf8Encoder = LimitingStringEncoder()

   override suspend fun init() {
      init(enablePreferences = true)
   }

   @VisibleForTesting
   internal suspend fun init(enablePreferences: Boolean) {
      val reloadAllData = !bucketSyncRepository.init(
         BUCKET_DATA_VERSION.toInt(),
         dynamicPool = 2..MAX_BUCKET_ID
      )
      if (reloadAllData) {
         logcat { "Got different protocol version, resetting all data" }
      }

      if (enablePreferences) {
         syncPreferences()
      }
   }

   // Magic numbers are a whole point of this function (protocol constants).
   // Use is not required for memory-only Buffer
   @Suppress("MagicNumber", "MissingUseCall")
   override suspend fun syncNotification(notification: ProcessedNotification, preferences: Preferences): Int {
      val buffer = Buffer()

      val notificationData = notification.systemData
      logcat { "Syncing notification ${notificationData.key} ${notificationData.title}" }

      val epochSecond = notificationData.timestamp.epochSecond
      val watchTitle = notificationData.watchTitle()
      val watchBody = notificationData.watchBody(watchTitle)
      buffer.writeUInt(epochSecond.toUInt())
      buffer.writeUByte(notificationData.pebbleOsIconId().toUByte())
      buffer.writeUByte(notificationData.pebbleOsColorId().toUByte())

      buffer.write(
         utf8Encoder.encodeSizeLimited(
            notificationData.title,
            MAX_APP_NAME_TEXT_LENGTH,
            true
         ).encodedString
      )
      buffer.writeUByte(0u)
      buffer.write(
         utf8Encoder.encodeSizeLimited(
            watchTitle,
            MAX_TITLE_TEXT_LENGTH,
            true
         ).encodedString
      )
      buffer.writeUByte(0u)
      val leftoverSize = BucketSyncRepository.MAX_BUCKET_SIZE_BYTES - buffer.size.toInt()
      buffer.write(
         utf8Encoder.encodeSizeLimited(
            watchBody.fixPebbleIndentation(),
            leftoverSize,
            true
         ).encodedString
      )

      val flags: UByte = getNotificationFlags(notification, preferences)

      val id = bucketSyncRepository.updateBucketDynamic(
         notificationData.key,
         buffer.readByteArray(),
         sortKey = -epochSecond,
         flags = flags
      )

      logcat { "Synced" }

      return id
   }

   @Suppress("MagicNumber") // Protocol constants
   private fun getNotificationFlags(notification: ProcessedNotification, preferences: Preferences): UByte {
      var flags: UByte = 0u

      if (notification.unread) {
         flags = flags or 0x01u
      }

      if (notification.paused.any) {
         flags = flags or 0x02u
      }

      if (notification.vibrated && preferences[RuleOption.periodicVibration]) {
         flags = flags or 0x04u
      }

      return flags
   }

   override suspend fun clearAllNotifications() {
      bucketSyncRepository.clearAllDynamic()
   }

   override suspend fun clearNotification(key: String) {
      bucketSyncRepository.deleteBucketDynamic(key)
      logcat { "Deleting Notification $key from the store" }
   }

   override suspend fun prepareNotificationReadStatus(notification: ProcessedNotification, preferences: Preferences) {
      bucketSyncRepository.updateBucketFlagsSilently(
         id = notification.bucketId.toUByte(),
         flags = getNotificationFlags(notification, preferences)
      )
   }

   @Suppress("MissingUseCall") // Buffer does not need to be closed
   private fun syncPreferences() {
      defaultScope.launch {
         preferenceStore.data.debounce(50.milliseconds).collect { preferences ->
            var flags: Byte = 0
            if (preferences[GlobalPreferenceKeys.muteWatch]) {
               flags = flags or 0x01
            }
            if (preferences[GlobalPreferenceKeys.mutePhone]) {
               flags = flags or 0x02
            }

            val autoClose = preferences[GlobalPreferenceKeys.autoCloseSeconds]

            val buffer = Buffer()

            buffer.writeByte(flags.toInt())
            buffer.writeUShort(autoClose.toUShort())

            bucketSyncRepository.updateBucket(
               1u,
               buffer.readByteArray()
            )
         }
      }
   }
}

private const val MAX_APP_NAME_TEXT_LENGTH = 48
private const val MAX_TITLE_TEXT_LENGTH = 64

private fun ParsedNotification.watchTitle(): String {
   return subtitle.ifBlank { body.lineSequence().firstOrNull().orEmpty() }
}

private fun ParsedNotification.watchBody(watchTitle: String): String {
   if (subtitle.isNotBlank()) {
      return body.removeSenderPrefix(subtitle)
   }

   val lines = body.lineSequence().toList()
   if (lines.firstOrNull() != watchTitle) {
      return body.removeSenderPrefix(watchTitle)
   }

   return lines.drop(1).joinToString("\n").removeSenderPrefix(watchTitle)
}

private fun String.removeSenderPrefix(sender: String): String {
   if (sender.isBlank()) {
      return this
   }

   return when {
      startsWith("$sender: ") -> removePrefix("$sender: ")
      startsWith("$sender\n") -> removePrefix("$sender\n")
      else -> this
   }
}

private fun ParsedNotification.pebbleOsIconId(): Int {
   val pkg = this.pkg.lowercase()
   val app = title.lowercase()
   return when {
      "gmail" in pkg || "google.android.gm" in pkg || "gmail" in app -> 1
      "whatsapp" in pkg || "whatsapp" in app -> 2
      "telegram" in pkg || "telegram" in app -> 6
      "discord" in pkg || "discord" in app -> 35
      "teams" in pkg || "teams" in app -> 36
      "google.android.apps.dynamite" in pkg || "google chat" in app -> 37
      "signal" in pkg || "signal" in app -> 38
      "reddit" in pkg || "reddit" in app -> 39
      "youtube" in pkg || "youtube" in app -> 40
      "zoom" in pkg || "zoom" in app -> 41
      "twitch" in pkg || "twitch" in app -> 42
      "google.android.apps.tasks" in pkg || "google tasks" in app || app == "tasks" -> 43
      "teslamotors" in pkg || "tesla" in pkg || "tesla" in app -> 44
      "google.android.apps.docs" in pkg || "drive" in pkg || "google drive" in app ||
         app == "drive" -> 8
      "messenger" in pkg || "orca" in pkg || "messenger" in app -> 3
      "facebook" in pkg || "katana" in pkg || app == "facebook" -> 4
      "twitter" in pkg || "x.android" in pkg || app == "x" || "twitter" in app -> 5
      "hangouts" in pkg || "hangouts" in app -> 7
      "inbox" in pkg || app == "inbox" -> 8
      "google.android.apps.messaging" in pkg || "google messages" in app -> 19
      "sms" in pkg || "mms" in pkg || "messaging" in pkg || "messages" in pkg || app == "messages" -> 9
      "outlook" in pkg || "outlook" in app -> 20
      "mail" in pkg || "email" in pkg || "mail" in app -> 10
      "phone" in pkg || "dialer" in pkg || app == "phone" -> 11
      "instagram" in pkg || "instagram" in app -> 12
      "slack" in pkg || "tinyspeck" in pkg || "slack" in app -> 13
      "linkedin" in pkg || "linkedin" in app -> 14
      "amazon" in pkg || "amazon" in app -> 15
      "maps" in pkg || "waze" in pkg || app == "maps" || "google maps" in app -> 16
      "photos" in pkg || app == "photos" || "google photos" in app -> 17
      "calendar" in pkg || "calendar" in app -> 18
      "skype" in pkg || "skype" in app -> 21
      "snapchat" in pkg || "snapchat" in app -> 22
      "line" in pkg || app == "line" -> 23
      "wechat" in pkg || "tencent.mm" in pkg || "wechat" in app -> 24
      "kik" in pkg || app == "kik" -> 25
      "viber" in pkg || "viber" in app -> 26
      "kakao" in pkg || "kakao" in app -> 27
      "blackberry" in pkg || "bbm" in pkg || "blackberry" in app || app == "bbm" -> 28
      "yahoo" in pkg || "yahoo" in app -> 29
      "weather" in pkg || "weather" in app -> 30
      "spotify" in pkg || "music" in pkg || "spotify" in app || "music" in app -> 31
      "uber" in pkg || "doordash" in pkg || "lyft" in pkg || "maps" in app || "delivery" in app -> 32
      "reminder" in pkg || "todo" in pkg || "tasks" in pkg || "reminder" in app || "tasks" in app -> 33
      else -> 0
   }
}

private fun ParsedNotification.pebbleOsColorId(): Int {
   val pkg = this.pkg.lowercase()
   val app = title.lowercase()
   return when {
      "gmail" in pkg || "google.android.gm" in pkg || "youtube" in pkg || "tesla" in pkg ||
         "gmail" in app || "youtube" in app || "tesla" in app -> 1
      "whatsapp" in pkg || "hangouts" in pkg || "kik" in pkg || "line" in pkg ||
         "whatsapp" in app || "hangouts" in app || app == "kik" || app == "line" -> 2
      "messenger" in pkg || "telegram" in pkg || "inbox" in pkg || "outlook" in pkg ||
         "maps" in pkg || "photos" in pkg || "signal" in pkg || "zoom" in pkg ||
         "google.android.apps.docs" in pkg || "drive" in pkg ||
         "messenger" in app || "telegram" in app || "outlook" in app || "maps" in app || "signal" in app ||
         "zoom" in app || "google drive" in app || app == "drive" ||
         "photos" in app -> 3
      "facebook" in pkg || app == "facebook" -> 4
      "twitter" in pkg || app == "x" || "twitter" in app -> 5
      "sms" in pkg || "messaging" in pkg || "messages" in pkg || app == "messages" -> 6
      "phone" in pkg || "dialer" in pkg || app == "phone" -> 7
      "instagram" in pkg || "linkedin" in pkg || "skype" in pkg ||
         "instagram" in app || "linkedin" in app || "skype" in app -> 8
      "slack" in pkg || "tinyspeck" in pkg || "reminder" in pkg || "tasks" in pkg ||
         "slack" in app || "reminder" in app || "tasks" in app -> 9
      "amazon" in pkg || "snapchat" in pkg || "yahoo" in pkg ||
         "amazon" in app || "snapchat" in app || "yahoo" in app -> 10
      "viber" in pkg || "wechat" in pkg || "viber" in app || "wechat" in app -> 11
      "teams" in pkg || "discord" in pkg || "twitch" in pkg ||
         "teams" in app || "discord" in app || "twitch" in app -> 12
      "calendar" in pkg || "weather" in pkg || "calendar" in app || "weather" in app -> 13
      "reddit" in pkg || "doordash" in pkg || "uber" in pkg || "lyft" in pkg ||
         "reddit" in app || "delivery" in app || "uber" in app || "lyft" in app -> 14
      "kakao" in pkg || "kakao" in app -> 15
      "blackberry" in pkg || "bbm" in pkg || "blackberry" in app || app == "bbm" -> 16
      else -> 0
   }
}
