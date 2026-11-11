package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import kotlinx.coroutines.flow.Flow

interface WatchSyncer {
   val stockNotificationActions: Flow<StockNotificationAction>

   suspend fun init()

   fun updateWatchPayloadLimits(watchBufferSize: Int)

   /**
    * Changes every time [clearAllNotifications] wipes the watch, so callers know their earlier syncs are gone.
    */
   val mirrorGeneration: Int get() = 0

   /** Everything sent so far must be encoded again (a setting changed how notifications are sent). */
   fun invalidateEncodings() {}

   suspend fun clearAllNotifications()
   suspend fun clearNotification(key: String)

   /**
    * @return bucket id of the notification
    */
   suspend fun syncNotification(notification: ProcessedNotification, preferences: Preferences): Int

   suspend fun prepareNotificationReadStatus(notification: ProcessedNotification, preferences: Preferences)
}

sealed interface StockNotificationAction {
   data class Dismiss(val key: String) : StockNotificationAction
}
