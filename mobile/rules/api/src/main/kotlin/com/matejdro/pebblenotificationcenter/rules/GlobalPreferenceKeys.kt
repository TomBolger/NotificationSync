package com.matejdro.pebblenotificationcenter.rules

import com.matejdro.pebblenotificationcenter.rules.keys.BooleanPreferenceKeyWithDefault
import com.matejdro.pebblenotificationcenter.rules.keys.IntPreferenceKeyWithDefault
import com.matejdro.pebblenotificationcenter.rules.keys.StringListPreferenceKeyWithDefault

object GlobalPreferenceKeys {
   val muteWatch = BooleanPreferenceKeyWithDefault("mute_watch", false)
   val mutePhone = BooleanPreferenceKeyWithDefault("mute_phone", false)
   val skipNotificationsWhenPhoneUnlocked = BooleanPreferenceKeyWithDefault(
      "skip_notifications_when_phone_unlocked",
      false
   )
   val waitForWatchfaceBeforeOpening = BooleanPreferenceKeyWithDefault(
      "wait_for_watchface_before_opening",
      false
   )
   val stockPebbleOsNotifications = BooleanPreferenceKeyWithDefault(
      "stock_pebble_os_notifications",
      false
   )
   val deferNewNotificationsWhileInteracting = BooleanPreferenceKeyWithDefault(
      "defer_new_notifications_while_interacting",
      true
   )
   val actionOrder = StringListPreferenceKeyWithDefault("action_order", emptyList())

   val autoCloseSeconds = IntPreferenceKeyWithDefault("auto_close", 600)
   val newNotificationInteractionTimeoutSeconds = IntPreferenceKeyWithDefault(
      "new_notification_interaction_timeout",
      10
   )
   val showMessagingStyleChronologically = BooleanPreferenceKeyWithDefault("show_messaging_style_chronologically", false)

   /** Notification text size on the watch: 0 = small, 1 = default, 2 = large. */
   val watchTextSize = IntPreferenceKeyWithDefault("watch_text_size", 1)

   /** Sender name and message text weights on the watch. Bold for both is how PebbleOS draws them. */
   val watchSenderBold = BooleanPreferenceKeyWithDefault("watch_sender_bold", true)
   val watchMessageBold = BooleanPreferenceKeyWithDefault("watch_message_bold", true)

   /**
    * When off, the watchapp never buzzes or opens by itself: it is only a mirror of the shade, e.g. alongside
    * PebbleOS's own notifications from the Pebble app.
    */
   val popUpOnWatch = BooleanPreferenceKeyWithDefault("pop_up_on_watch", true)

   val notifyOnReconnect = BooleanPreferenceKeyWithDefault("notify_on_reconnect", true)
}
