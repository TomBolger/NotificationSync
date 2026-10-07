package com.matejdro.pebblenotificationcenter.notification

import android.os.Build
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebblenotificationcenter.bluetooth.WatchappOpenController
import com.matejdro.pebblenotificationcenter.common.di.NavigationInjectingApplication
import com.matejdro.pebblenotificationcenter.notification.di.NotificationInject
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.notification.parsing.NotificationParser
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.keys.get
import dev.zacsweers.metro.Inject
import dispatch.core.DefaultCoroutineScope
import io.rebble.pebblekit2.client.PebbleInfoRetriever
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import logcat.logcat
import si.inova.kotlinova.core.reporting.ErrorReporter
import kotlin.time.Duration.Companion.milliseconds

class NotificationService : NotificationListenerService() {
   @Inject
   private lateinit var notificationProcessor: NotificationProcessor

   @Inject
   private lateinit var notificationParser: NotificationParser

   @Inject
   private lateinit var coroutineScope: DefaultCoroutineScope

   @Inject
   private lateinit var pebbleInfoRetriever: PebbleInfoRetriever

   @Inject
   private lateinit var errorReporter: ErrorReporter

   @Inject
   private lateinit var preferenceStore: DataStore<Preferences>

   @Inject
   private lateinit var notificationServiceStatus: NotificationServiceStatus

   @Inject
   private lateinit var watchOpenController: WatchappOpenController

   private val mutex = Mutex()
   private val delayedResyncJobs = HashMap<String, Job>()
   private val events = Channel<ServiceEvent>(Channel.UNLIMITED)

   @Volatile
   private var bound = false
   private var listenersStarted = false
   private lateinit var serviceScope: CoroutineScope

   override fun onCreate() {
      logcat { "Starting notification service" }
      (application!! as NavigationInjectingApplication)
         .applicationGraph
         .let { it as NotificationInject }
         .inject(this)

      serviceScope = CoroutineScope(coroutineScope.coroutineContext + SupervisorJob(coroutineScope.coroutineContext[Job]))
      instance = this

      super.onCreate()
      processEvents()
   }

   override fun onDestroy() {
      logcat { "Stopping notification service" }
      serviceScope.cancel()
      delayedResyncJobs.values.forEach { it.cancel() }
      delayedResyncJobs.clear()
      events.close()
      instance = null
      bound = false
      super.onDestroy()
   }

   override fun onListenerConnected() {
      super.onListenerConnected()

      if (!bound) {
         bound = true
         if (!listenersStarted) {
            listenersStarted = true
            controlListenerHintsAndOpenOnReconnect()
         }
      }

      resyncActiveNotifications()
   }

   fun resyncActiveNotifications() {
      serviceScope.launch {
         resyncActiveNotificationsNow()
      }
   }

   suspend fun resyncActiveNotificationsNow(): Boolean {
      return mutex.withLock {
         resyncActiveNotificationsLocked()
      }
   }

   suspend fun resyncNotificationNow(key: String): Boolean {
      return mutex.withLock {
         resyncNotificationLocked(key)
      }
   }

   override fun onNotificationPosted(sbn: StatusBarNotification) {
      logcat { "Notification ${sbn.key} posted" }
      events.trySend(ServiceEvent.Posted(sbn))
   }

   /**
    * Posts and removals are applied strictly in the order Android delivered them. Launching a coroutine per
    * callback (as before) let a quick post+dismiss pair race, so the dismiss could be applied first and the
    * notification then stayed on the watch forever.
    */
   private fun processEvents() {
      serviceScope.launch {
         for (event in events) {
            try {
               when (event) {
                  is ServiceEvent.Posted -> {
                     val parsedSuccessfully = mutex.withLock {
                        val parsed = parseNotification(event.sbn)
                        if (parsed == null) {
                           logcat { "Notification ${event.sbn.key} has no text. Skipping..." }
                           false
                        } else {
                           notificationProcessor.onNotificationPosted(parsed)
                           true
                        }
                     }
                     if (parsedSuccessfully) {
                        scheduleDelayedActiveNotificationResync(event.sbn.key)
                     }
                  }

                  is ServiceEvent.Removed -> {
                     mutex.withLock {
                        notificationProcessor.onNotificationDismissed(event.key)
                     }
                  }
               }
            } catch (e: CancellationException) {
               throw e
            } catch (e: Exception) {
               errorReporter.report(e)
               // Reconcile against Android instead of letting one bad event kill the only consumer.
               try {
                  resyncActiveNotificationsNow()
               } catch (recoveryError: CancellationException) {
                  throw recoveryError
               } catch (recoveryError: Exception) {
                  errorReporter.report(recoveryError)
               }
            }
         }
      }
   }

   override fun onListenerDisconnected() {
      bound = false
      super.onListenerDisconnected()
   }

   private suspend fun parseNotification(sbn: StatusBarNotification): ParsedNotification? {
      val ranking = Ranking()
      val hasRanking = currentRanking.getRanking(sbn.key, ranking)

      return notificationParser.parse(
         sbn,
         getFastNotificationChannel(sbn, ranking, hasRanking),
         ranking,
         preferenceStore.data.first()[GlobalPreferenceKeys.showMessagingStyleChronologically]
      )
   }

   private fun getFastNotificationChannel(
      sbn: StatusBarNotification,
      ranking: Ranking,
      hasRanking: Boolean,
   ): Any? =
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
         if (hasRanking) {
            ranking.channel ?: sbn.notification.channelId
         } else {
            sbn.notification.channelId
         }
      } else {
         null
      }

   private suspend fun resyncActiveNotificationsLocked(): Boolean {
      if (!bound) return false
      val currentNotifications = try {
         activeNotifications
      } catch (exception: SecurityException) {
         errorReporter.report(exception)
         return false
      }

      val parsedNotifications = currentNotifications.mapNotNull { sbn ->
         val parsed = parseNotification(sbn)
         if (parsed == null) {
            logcat { "Notification ${sbn.key} has no text. Skipping..." }
         }

         parsed
      }

      notificationProcessor.onActiveNotificationsResynced(parsedNotifications)
      return true
   }

   private suspend fun resyncNotificationLocked(key: String): Boolean {
      if (!bound) return false
      val sbn = try {
         activeNotifications.firstOrNull { it.key == key }
      } catch (exception: SecurityException) {
         errorReporter.report(exception)
         return false
      } ?: return false

      val parsed = parseNotification(sbn)
      if (parsed == null) {
         logcat { "Notification ${sbn.key} has no text. Skipping..." }
         return false
      }

      notificationProcessor.onNotificationPosted(parsed, suppressVibration = true)
      return true
   }

   override fun onNotificationRemoved(sbn: StatusBarNotification) {
      logcat { "Notification ${sbn.key} removed" }
      delayedResyncJobs.remove(sbn.key)?.cancel()
      events.trySend(ServiceEvent.Removed(sbn.key))
   }

   private fun scheduleDelayedActiveNotificationResync(key: String) {
      delayedResyncJobs.remove(key)?.cancel()
      delayedResyncJobs[key] = serviceScope.launch {
         delay(NOTIFICATION_STABILIZATION_DELAY)
         mutex.withLock {
            resyncActiveNotificationsLocked()
         }
         delayedResyncJobs.remove(key)
      }
   }

   private fun controlListenerHintsAndOpenOnReconnect() {
      val anyWatchConnected = pebbleInfoRetriever.getConnectedWatches().map { it.isNotEmpty() }.distinctUntilChanged()

      controlListenerHints(anyWatchConnected)
      openOnReconnect(anyWatchConnected)
   }

   private fun controlListenerHints(
      anyWatchConnected: Flow<Boolean>,
   ) {
      val mutePhoneFlow = preferenceStore.data.map { preferences ->
         preferences[GlobalPreferenceKeys.mutePhone]
      }.distinctUntilChanged()

      serviceScope.launch {
         mutePhoneFlow.flatMapLatest { mutePhone ->
            if (mutePhone) {
               anyWatchConnected.map { connected ->
                  var listenerHints = 0
                  if (connected) {
                     listenerHints = listenerHints or HINT_HOST_DISABLE_NOTIFICATION_EFFECTS
                  }

                  listenerHints
               }.distinctUntilChanged()
            } else {
               flowOf(0)
            }
         }
            .collect { listenerHints ->
               try {
                  waitForCompanionDeviceManager()
                  requestListenerHints(listenerHints)
               } catch (e: SecurityException) {
                  errorReporter.report(e)
               }
            }
      }
   }

   private fun openOnReconnect(anyWatchConnected: Flow<Boolean>) {
      serviceScope.launch {
         var prevConnected: Boolean? = null
         anyWatchConnected.collect { connected ->
            logcat { "Watch connected: $connected" }

            if (connected &&
               prevConnected == false &&
               notificationProcessor.peekNextVibration() != null &&
               preferenceStore.data.first()[GlobalPreferenceKeys.notifyOnReconnect]
            ) {
               logcat { "Missed notifications while the watch was disconnected. Reopening..." }
               watchOpenController.openWatchapp()
            }

            prevConnected = connected
         }
      }
   }

   private suspend fun waitForCompanionDeviceManager(): Boolean {
      // CompanionDeviceManager sometimes takes a while to bind
      // Wait a bit
      repeat(CDM_WAIT_ATTEMPTS) {
         if (notificationServiceStatus.isPermissionGranted()) {
            return true
         }

         delay(100.milliseconds)
      }

      return false
   }

   companion object {
      internal var instance: NotificationService? = null
   }
}

private sealed interface ServiceEvent {
   data class Posted(val sbn: StatusBarNotification) : ServiceEvent
   data class Removed(val key: String) : ServiceEvent
}

private const val CDM_WAIT_ATTEMPTS = 10
private val NOTIFICATION_STABILIZATION_DELAY = 750.milliseconds
