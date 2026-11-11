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

   /** Entries the watch mirror kept from before the app restarted. */
   val leftoverKeys = mutableSetOf<String>()

   override suspend fun syncedNotificationKeys(): Set<String> = bucketIdsByKey.keys + leftoverKeys

   override suspend fun init() {
   }

   override fun updateWatchPayloadLimits(watchBufferSize: Int) {
      this.watchBufferSize = watchBufferSize
   }

   override var mirrorGeneration: Int = 0

   override fun invalidateEncodings() {
      mirrorGeneration++
   }

   override suspend fun clearAllNotifications() {
      mirrorGeneration++
      clearAllCalled = true
      bucketIdsByKey.clear()
      onClearAllNotifications()
   }

   override suspend fun clearNotification(key: String) {
      clearedNotifications.add(key)
      bucketIdsByKey.remove(key)
      leftoverKeys.remove(key)
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
