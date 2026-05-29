package com.matejdro.pebblenotificationcenter.bluetooth

import io.rebble.pebblekit2.common.model.WatchIdentifier

interface WatchappOpenController {
   fun isNextWatchappOpenForAutoSync(): Boolean
   fun setNextWatchappOpenForAutoSync()
   fun getNextWatchappOpenNotificationBucket(): Int?
   fun setNextWatchappOpenNotificationBucket(bucketId: Int)
   fun resetNextWatchappOpen()

   suspend fun openWatchapp()

   suspend fun closeWatchappToTheLastApp(watch: WatchIdentifier)
}
