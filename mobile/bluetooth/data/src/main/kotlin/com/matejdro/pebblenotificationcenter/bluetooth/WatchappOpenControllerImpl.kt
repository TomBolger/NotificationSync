package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import com.matejdro.bucketsync.BucketSyncWatchappOpenController
import com.matejdro.pebblenotificationcenter.bluetooth.api.WATCHAPP_UUID
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.keys.get
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dev.zacsweers.metro.SingleIn
import dev.zacsweers.metro.binding
import dispatch.core.DefaultCoroutineScope
import io.rebble.pebblekit2.client.PebbleInfoRetriever
import io.rebble.pebblekit2.client.PebbleSender
import io.rebble.pebblekit2.common.model.WatchIdentifier
import io.rebble.pebblekit2.model.Watchapp
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import logcat.logcat
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import kotlin.time.Duration.Companion.minutes

@Inject
@SingleIn(AppScope::class)
@ContributesBinding(AppScope::class, binding<WatchappOpenController>())
@ContributesBinding(AppScope::class, binding<BucketSyncWatchappOpenController>())
class WatchappOpenControllerImpl(
   private val pebbleSender: PebbleSender,
   private val pebbleInfoRetriever: PebbleInfoRetriever,
   private val preferenceStore: DataStore<Preferences>,
   private val defaultCoroutineScope: DefaultCoroutineScope,
) : WatchappOpenController, BucketSyncWatchappOpenController {
   private var nextWatchappOpenForAutoSync: Boolean = false
   private var nextWatchappOpenNotificationBucket: Int? = null
   private var nextWatchappOpenForMirrorReset: Boolean = false
   private val lastOpenedApps = ConcurrentHashMap<WatchIdentifier, UUID>()
   private val deferredOpenJobs = ConcurrentHashMap<WatchIdentifier, Job>()

   override fun isNextWatchappOpenForAutoSync(): Boolean {
      return nextWatchappOpenForAutoSync
   }

   override fun setNextWatchappOpenForAutoSync() {
      nextWatchappOpenForAutoSync = true
   }

   override fun getNextWatchappOpenNotificationBucket(): Int? {
      return nextWatchappOpenNotificationBucket
   }

   override fun setNextWatchappOpenNotificationBucket(bucketId: Int) {
      nextWatchappOpenNotificationBucket = bucketId
   }

   override fun isNextWatchappOpenForMirrorReset(): Boolean {
      return nextWatchappOpenForMirrorReset
   }

   override fun setNextWatchappOpenForMirrorReset() {
      nextWatchappOpenForMirrorReset = true
   }

   override fun resetNextWatchappOpen() {
      nextWatchappOpenForAutoSync = false
      nextWatchappOpenNotificationBucket = null
      nextWatchappOpenForMirrorReset = false
   }

   override suspend fun openWatchapp() {
      val connectedWatches = pebbleInfoRetriever.getConnectedWatches().first()
      val waitForWatchface = preferenceStore.data.first()[GlobalPreferenceKeys.waitForWatchfaceBeforeOpening]
      for (watch in connectedWatches) {
         val watchId = watch.id
         openWatchappOnWatch(watchId, waitForWatchface)
      }
   }

   private suspend fun openWatchappOnWatch(watchId: WatchIdentifier, waitForWatchface: Boolean) {
      val activeApp = pebbleInfoRetriever.getActiveApp(watchId).first()
      if (!waitForWatchface ||
         activeApp?.type == Watchapp.Type.WATCHFACE ||
         activeApp?.id == WATCHAPP_UUID
      ) {
         openWatchappNow(watchId, activeApp)
         return
      }

      logcat { "Deferring watchapp open on $watchId until watch returns to a watchface. Active app: $activeApp" }
      deferredOpenJobs.remove(watchId)?.cancel()
      deferredOpenJobs[watchId] = defaultCoroutineScope.launch {
         // A notification that waited longer than this is old news: don't pop it up out of nowhere later.
         val watchface = withTimeoutOrNull(DEFERRED_OPEN_TIMEOUT) {
            pebbleInfoRetriever.getActiveApp(watchId)
               .filter { it?.type == Watchapp.Type.WATCHFACE || it?.id == WATCHAPP_UUID }
               .first()
         }
         deferredOpenJobs.remove(watchId)
         if (watchface == null) {
            logcat { "Gave up waiting for the watchface on $watchId" }
            return@launch
         }
         openWatchappNow(watchId, watchface)
      }
   }

   private suspend fun openWatchappNow(watchId: WatchIdentifier, openedApp: Watchapp?) {
      val openedAppId = openedApp?.id
      if (openedAppId != WATCHAPP_UUID) {
         if (openedAppId == null) lastOpenedApps.remove(watchId) else lastOpenedApps[watchId] = openedAppId
      }

      logcat { "Opening app on the $watchId, from ${openedApp ?: "null"}" }
      pebbleSender.startAppOnTheWatch(WATCHAPP_UUID, listOf(watchId))
   }

   override suspend fun closeWatchappToTheLastApp(watch: WatchIdentifier) {
      deferredOpenJobs.remove(watch)?.cancel()
      val lastApp = lastOpenedApps.remove(watch)
      logcat { "Last open app: ${lastApp ?: "null"}" }
      if (lastApp != null) {
         pebbleSender.startAppOnTheWatch(lastApp, listOf(watch))
      } else {
         pebbleSender.stopAppOnTheWatch(WATCHAPP_UUID, listOf(watch))
      }
   }
}

private val DEFERRED_OPEN_TIMEOUT = 10.minutes
