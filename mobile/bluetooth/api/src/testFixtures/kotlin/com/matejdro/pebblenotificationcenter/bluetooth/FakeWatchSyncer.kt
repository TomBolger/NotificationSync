package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow

class FakeWatchSyncer : WatchSyncer {
   override val stockNotificationActions: Flow<StockNotificationAction> = emptyFlow()

   val syncedNotifications = mutableListOf<ProcessedNotification>()
   val syncedNotificationReadStatuses = mutableListOf<ProcessedNotification>()
   val clearedNotifications = mutableListOf<String>()
   var clearAllCalled = false

   var nextBucketId = 1
   var watchBufferSize = 0
   var onClearAllNotifications: suspend () -> Unit = {}
   private val bucketIdsByKey = HashMap<String, Int>()

   override suspend fun init() {
   }

   override fun updateWatchPayloadLimits(watchBufferSize: Int) {
      this.watchBufferSize = watchBufferSize
   }

   override suspend fun clearAllNotifications() {
      clearAllCalled = true
      bucketIdsByKey.clear()
      onClearAllNotifications()
   }

   override suspend fun clearNotification(key: String) {
      clearedNotifications.add(key)
      bucketIdsByKey.remove(key)
   }

   override suspend fun syncNotification(
      notification: ProcessedNotification,
      preferences: Preferences,
   ): Int {
      syncedNotifications.add(notification)
      return bucketIdsByKey.getOrPut(notification.systemData.key) { nextBucketId++ }
   }

   override suspend fun prepareNotificationReadStatus(notification: ProcessedNotification, preferences: Preferences) {
      syncedNotificationReadStatuses.add(notification)
   }
}
