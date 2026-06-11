package com.matejdro.pebblenotificationcenter.notification

import com.matejdro.pebblenotificationcenter.notification.model.LightNotificationChannel
import kotlin.time.Duration

interface NotificationServiceController {
   fun resyncActiveNotifications(): Boolean
   suspend fun resyncActiveNotificationsNow(): Boolean
   suspend fun resyncNotificationNow(key: String): Boolean
   fun cancelNotification(key: String): Boolean
   fun snoozeNotificationNotification(key: String, duration: Duration): Boolean
   fun triggerAction(pendingIntent: Any): Boolean
   fun triggerReplyAction(pendingIntent: Any, remoteInputKey: String, text: String): Boolean
   fun getNotificationChannels(pkg: String): List<LightNotificationChannel>
}
