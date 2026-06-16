package com.matejdro.pebblenotificationcenter.bluetooth

import com.matejdro.pebblenotificationcenter.bluetooth.api.WATCHAPP_UUID
import com.matejdro.pebblenotificationcenter.notification.NotificationServiceController
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import io.rebble.pebblekit2.client.PebbleInfoRetriever
import io.rebble.pebblekit2.client.PebbleSender
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import kotlinx.coroutines.flow.first
import logcat.logcat

@Inject
@ContributesBinding(AppScope::class)
class WatchMirrorResetterImpl(
   private val watchSyncer: WatchSyncer,
   private val notificationServiceController: NotificationServiceController,
   private val watchappOpenController: WatchappOpenController,
   private val pebbleInfoRetriever: PebbleInfoRetriever,
   private val pebbleSender: PebbleSender,
) : WatchMirrorResetter {
   override suspend fun resetWatchMirror() {
      watchSyncer.clearAllNotifications()
      sendWatchLocalMirrorReset()
      notificationServiceController.resyncActiveNotificationsNow()
      watchappOpenController.setNextWatchappOpenForMirrorReset()
      watchappOpenController.openWatchapp()
   }

   private suspend fun sendWatchLocalMirrorReset() {
      val connectedWatches = pebbleInfoRetriever.getConnectedWatches().first()
         .map { it.id }
      if (connectedWatches.isEmpty()) {
         return
      }

      val result = pebbleSender.sendDataToPebble(
         WATCHAPP_UUID,
         mapOf(0u to PebbleDictionaryItem.UInt8(PACKET_RESET_WATCH_MIRROR)),
         connectedWatches,
      )
      logcat { "Watch mirror reset packet result: $result" }
   }
}
