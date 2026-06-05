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

   val notifyOnReconnect = BooleanPreferenceKeyWithDefault("notify_on_reconnect", true)
}
