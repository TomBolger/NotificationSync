package com.matejdro.pebblenotificationcenter.notification.parsing

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Build
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationCompat
import androidx.core.os.BundleCompat
import com.matejdro.pebblenotificationcenter.notification.NotificationConstants
import com.matejdro.pebblenotificationcenter.notification.R
import com.matejdro.pebblenotificationcenter.notification.api.AppNameProvider
import com.matejdro.pebblenotificationcenter.notification.model.NativeAction
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_CATEGORY_NEWS
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_CATEGORY_NONE
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_CATEGORY_PROMOTIONS
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_CATEGORY_SOCIAL
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_CATEGORY_SUGGESTED
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_SECTION_ALERTING
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_SECTION_CONVERSATION
import com.matejdro.pebblenotificationcenter.notification.model.SHADE_SECTION_SILENT
import com.matejdro.pebblenotificationcenter.notification.utils.parseVibrationPattern
import dev.zacsweers.metro.Inject
import java.time.Instant

@Inject
class NotificationParser(
   private val context: Context,
   private val appNameProvider: AppNameProvider,
) {
   fun parse(
      sbn: StatusBarNotification,
      channel: Any?,
      ranking: NotificationListenerService.Ranking? = null,
      showMessagingStyleChronologically: Boolean = false,
   ): ParsedNotification? {
      val notification = sbn.notification
      if (sbn.packageName in PEBBLE_APP_PACKAGES) {
         // The Pebble app's own notifications are about the watch itself (it posts one briefly whenever a watch
         // app starts). Mirroring them popped this app over the app you just opened, which posted another, in a
         // loop.
         return null
      }
      if (sbn.packageName == context.packageName && !notification.isOwnTestNotification()) {
         // Our own status and error notifications never go to the watch: mirroring them made a loop where a
         // failed send (another watch app in front) posted an error, which popped this app open again.
         return null
      }
      val title = notification.extras.getString(NotificationConstants.KEY_TEST_APP_NAME)
         ?.takeIf { sbn.packageName == context.packageName }
         ?: appNameProvider.getAppName(sbn.packageName)

      val (imageUri, messagingStyleText) = notification.parseMessagingStyle(showMessagingStyleChronologically)
      val (subtitle, text) = parseSubtitleAndBody(notification, messagingStyleText)

      if (subtitle.isBlank() && text.isNullOrBlank()) {
         return null
      }

      val (channelId, isSilent) = processChannel(notification, channel, ranking)
      // Android 16 sorts some notifications into categories by moving them to these reserved channels.
      val category = when (channelId) {
         "android.app.recs" -> SHADE_CATEGORY_SUGGESTED
         "android.app.promotions" -> SHADE_CATEGORY_PROMOTIONS
         "android.app.news" -> SHADE_CATEGORY_NEWS
         "android.app.social" -> SHADE_CATEGORY_SOCIAL
         else -> SHADE_CATEGORY_NONE
      }

      val timestampMillis = if (NotificationCompat.getShowWhen(notification)) {
         notification.`when`
      } else {
         sbn.postTime
      }

      val largeImage = imageUri?.let { Icon.createWithContentUri(it) }
         ?: BundleCompat.getParcelable<Bitmap>(notification.extras, NotificationCompat.EXTRA_PICTURE, Bitmap::class.java)
            ?.let { Icon.createWithBitmap(it) }
         ?: BundleCompat.getParcelable<Icon>(notification.extras, NotificationCompat.EXTRA_PICTURE_ICON, Icon::class.java)

      val subtitleWithCameraEmoji = if (!subtitle.contains("\uD83D\uDCF7") && largeImage != null) {
         "\uD83D\uDCF7 $subtitle"
      } else {
         subtitle
      }

      return ParsedNotification(
         key = sbn.key,
         id = sbn.id,
         tag = sbn.tag,
         pkg = sbn.packageName,
         title = title,
         subtitle = subtitle,
         body = text.orEmpty(),
         timestamp = Instant.ofEpochMilli(notification.parseMessagingStyleTimestamp() ?: timestampMillis),
         isSilent = isSilent,
         isFilteredByDoNotDisturb = ranking?.matchesInterruptionFilter() == false,
         onlyAlertOnce = notification.flags and Notification.FLAG_ONLY_ALERT_ONCE != 0,
         groupAlertSuppressed = notification.isGroupAlertSuppressed(),
         groupKey = sbn.groupKey.takeIf { sbn.isGroup },
         shadeSection = when {
            isSilent || category != SHADE_CATEGORY_NONE -> SHADE_SECTION_SILENT
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && ranking?.isConversation == true -> SHADE_SECTION_CONVERSATION
            else -> SHADE_SECTION_ALERTING
         },
         shadeCategory = category,
         isSuspended = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && ranking?.isSuspended == true,
         nativeActions = notification.parseActions(),
         channel = channelId,
         isOngoing = sbn.isOngoing,
         groupSummary = NotificationCompat.isGroupSummary(notification),
         localOnly = NotificationCompat.getLocalOnly(notification),
         media = notification.extras.containsKey(NotificationCompat.EXTRA_MEDIA_SESSION),
         forceVibrate = sbn.packageName == context.packageName &&
            notification.extras.getBoolean(NotificationConstants.KEY_FORCE_VIBRATE, false),
         overrideVibrationPattern = parseVibrationPattern(notification),
         iconDrawable = notification.smallIcon?.loadDrawable(context),
         largeImage = largeImage
      )
   }

   private fun parseSubtitleAndBody(
      notification: Notification,
      messagingStyleText: String?,
   ): Pair<String, String?> {
      val extras = notification.extras

      val subtitle = (
         extras.getCharSequence(NotificationCompat.EXTRA_CONVERSATION_TITLE)
            ?: extras.getCharSequence(NotificationCompat.EXTRA_HIDDEN_CONVERSATION_TITLE)
            ?: extras.getCharSequence(NotificationCompat.EXTRA_TITLE)
            ?: extras.getCharSequence(NotificationCompat.EXTRA_TITLE_BIG)
         )?.removeUselessCharacaters().orEmpty()

      val text =
         (
            messagingStyleText
               ?: notification.parseInboxStyle()
               ?: extras.getCharSequence(NotificationCompat.EXTRA_BIG_TEXT)
               ?: extras.getCharSequence(NotificationCompat.EXTRA_TEXT)
               ?: extras.getCharSequence(NotificationCompat.EXTRA_SUMMARY_TEXT)
               ?: extras.getCharSequence(NotificationCompat.EXTRA_SUB_TEXT)
               ?: extras.getCharSequence(NotificationCompat.EXTRA_INFO_TEXT)
            )
            ?.removeUselessCharacaters()

      val updatedSubtitle: CharSequence
      val updatedText: CharSequence?
      if (subtitle.length > MAX_TITLE_LENGTH) {
         updatedSubtitle = ""
         updatedText = if (text != null) "$subtitle\n$text" else subtitle
      } else {
         updatedSubtitle = subtitle
         updatedText = text
      }
      return updatedSubtitle to updatedText
   }

   /**
    * Whether the phone itself would stay quiet for this notification.
    *
    * Android 8+: the effective importance from the ranking, which includes the user silencing a channel or a
    * conversation ("Silent" on the notification), adaptive demotion and app defaults. Importance below DEFAULT
    * never makes sound or vibrates, whatever the app asked for. Before Android 8: priority and the legacy
    * sound/vibrate fields.
    */
   private fun processChannel(
      notification: Notification,
      channel: Any?,
      ranking: NotificationListenerService.Ranking?,
   ): Pair<String?, Boolean> {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
         val channelCast: NotificationChannel? = channel as? NotificationChannel
         val channelIdOnly: String? = channel as? String
         val channelId = channelCast?.id ?: channelIdOnly ?: notification.channelId

         val importance = ranking?.importance?.takeIf { it != NotificationManager.IMPORTANCE_UNSPECIFIED }
            ?: channelCast?.importance?.takeIf { it != NotificationManager.IMPORTANCE_UNSPECIFIED }
         return channelId to (importance != null && importance < NotificationManager.IMPORTANCE_DEFAULT)
      }

      @Suppress("DEPRECATION") // We still need to support legacy pre-channel notifications
      val isSilent = notification.priority <= Notification.PRIORITY_LOW || (
         notification.vibrate == null &&
            notification.sound == null &&
            (notification.defaults and (NotificationCompat.DEFAULT_SOUND or NotificationCompat.DEFAULT_VIBRATE)) == 0
         )

      return null to isSilent
   }

   /**
    * Android skips the alert of grouped notifications the app asked to keep quiet: children when the group alerts
    * through its summary, and the summary when the group alerts through its children.
    */
   private fun Notification.isGroupAlertSuppressed(): Boolean {
      if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O || group == null) {
         return false
      }
      val summary = flags and Notification.FLAG_GROUP_SUMMARY != 0
      return when (groupAlertBehavior) {
         Notification.GROUP_ALERT_SUMMARY -> !summary
         Notification.GROUP_ALERT_CHILDREN -> summary
         else -> false
      }
   }

   private fun Notification.parseMessagingStyle(showChronologically: Boolean): Pair<Uri?, String?> {
      val messagingStyle = NotificationCompat.MessagingStyle.extractMessagingStyleFromNotification(this) ?: return (null to null)

      val messages = (messagingStyle.messages + messagingStyle.historicMessages).let { unsortedMessages ->
         if (showChronologically) {
            unsortedMessages.sortedBy { it.timestamp }
         } else {
            unsortedMessages.sortedByDescending { it.timestamp }
         }
      }
      if (messages.isEmpty()) {
         return (null to null)
      }

      var lastName: CharSequence? = null
      var firstImage: Uri? = null

      val text = messages.joinToString("\n") { message ->
         if (firstImage == null && message.dataMimeType?.startsWith("image/") == true) {
            firstImage = message.dataUri
         }

         val personName = message.person?.name ?: messagingStyle.user.name
         val text = message.text?.toString().orEmpty()
         if (personName != null && lastName != personName) {
            "$personName: $text"
         } else {
            text
         }.also {
            lastName = personName
         }
      }

      return firstImage to text
   }

   private fun Notification.parseMessagingStyleTimestamp(): Long? {
      val messagingStyle = NotificationCompat.MessagingStyle.extractMessagingStyleFromNotification(this) ?: return null

      val messages = (messagingStyle.messages + messagingStyle.historicMessages)
      if (messages.isEmpty()) {
         return null
      }

      return messages.maxOf { it.timestamp }
   }

   private fun Notification.parseInboxStyle(): String? {
      val textLines = extras.getCharSequenceArray(NotificationCompat.EXTRA_TEXT_LINES) ?: return null

      return textLines.joinToString("\n")
   }

   private fun Notification.parseActions(): List<NativeAction> {
      val normalActions = actions.orEmpty().mapNotNull { action ->
         val remoteInput = action.remoteInputs?.firstOrNull()
         val actionIntent = action.actionIntent ?: return@mapNotNull null

         NativeAction(
            text = action.title.toString(),
            pendingIntent = actionIntent,
            remoteInputResultKey = remoteInput?.resultKey,
            cannedTexts = remoteInput?.choices?.map { it.toString() }.orEmpty(),
            allowFreeFormInput = remoteInput?.allowFreeFormInput != false
         )
      }

      val openOnPhoneAction = listOfNotNull(
         contentIntent?.let { contentIntent ->
            NativeAction(
               text = context.getString(R.string.open_on_phone),
               pendingIntent = contentIntent,
            )
         }
      )

      return normalActions + openOnPhoneAction
   }

   private fun CharSequence.removeUselessCharacaters(): String {
      return CONTROL_CHARACTERS.replace(this, "")
   }
}

private fun parseVibrationPattern(notification: Notification): List<Short>? {
   notification.extras.getShortArray(NotificationConstants.KEY_VIBRATION_PATTERN)?.toList()?.let { return it }

   val stringPattern = notification.extras.getString(NotificationConstants.KEY_VIBRATION_PATTERN) ?: return null

   return parseVibrationPattern(stringPattern)
}

private const val MAX_TITLE_LENGTH = 20
private val CONTROL_CHARACTERS = Regex("\\p{Cf}|\\p{M}")

private val OWN_TEST_CHANNELS = setOf("TESTS", "TEST_ALERTS", "TEST_SILENT")

/** Phone apps that run the Pebble watch connection (Core Devices, Rebble/Cobble and the original Pebble apps). */
private val PEBBLE_APP_PACKAGES = setOf(
   "coredevices.coreapp",
   "io.rebble.cobble",
   "com.getpebble.android.basalt",
   "com.getpebble.android",
)

/** Test notifications from Tools and the vibration pattern test are the only ones of ours meant for the watch. */
private fun Notification.isOwnTestNotification(): Boolean {
   val channel = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) channelId else null
   return channel in OWN_TEST_CHANNELS ||
      extras.containsKey(NotificationConstants.KEY_TEST_APP_NAME) ||
      extras.getBoolean(NotificationConstants.KEY_FORCE_VIBRATE)
}
