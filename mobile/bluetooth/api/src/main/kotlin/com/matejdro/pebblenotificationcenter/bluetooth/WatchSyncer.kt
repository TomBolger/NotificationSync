package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import kotlinx.coroutines.flow.Flow

interface WatchSyncer {
   val stockNotificationActions: Flow<StockNotificationAction>

   suspend fun init()

   fun updateWatchPayloadLimits(watchBufferSize: Int)

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
