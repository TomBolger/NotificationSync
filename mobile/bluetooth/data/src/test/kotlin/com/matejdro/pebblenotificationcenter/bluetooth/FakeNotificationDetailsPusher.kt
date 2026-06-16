package com.matejdro.pebblenotificationcenter.bluetooth

class FakeNotificationDetailsPusher : NotificationDetailsPusher {
   var lastPushRequestId: Int? = null
   var lastPreloadRequestId: Int? = null
   var lastOpenedPreloadRequestId: Int? = null
   var lastMaxPacketSize: Int? = null
   var lastColorWatch: Boolean? = null

   override fun pushNotificationDetails(bucketId: Int, maxPacketSize: Int, colorWatch: Boolean) {
      lastPushRequestId = bucketId
      lastMaxPacketSize = maxPacketSize
      lastColorWatch = colorWatch
   }

   override suspend fun preloadNotificationDetails(bucketId: Int, maxPacketSize: Int, colorWatch: Boolean) {
      lastPreloadRequestId = bucketId
      lastMaxPacketSize = maxPacketSize
      lastColorWatch = colorWatch
   }

   override suspend fun preloadOpenedNotificationDetails(bucketId: Int, maxPacketSize: Int, colorWatch: Boolean) {
      lastOpenedPreloadRequestId = bucketId
      lastMaxPacketSize = maxPacketSize
      lastColorWatch = colorWatch
   }
}
