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
import kotlinx.coroutines.Job
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

   private var bound = false

   override fun onCreate() {
      logcat { "Starting notification service" }
      (application!! as NavigationInjectingApplication)
         .applicationGraph
         .let { it as NotificationInject }
         .inject(this)

      instance = this

      super.onCreate()
   }

   override fun onDestroy() {
      logcat { "Stopping notification service" }
      delayedResyncJobs.values.forEach { it.cancel() }
      delayedResyncJobs.clear()
      instance = null
      bound = false
      super.onDestroy()
   }

   override fun onListenerConnected() {
      super.onListenerConnected()

      if (!bound) {
         bound = true
         controlListenerHintsAndOpenOnReconnect()
      }

      resyncActiveNotifications()
   }

   fun resyncActiveNotifications() {
      coroutineScope.launch {
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
      coroutineScope.launch {
         var parsedSuccessfully = false
         mutex.withLock {
            val parsed = parseNotification(sbn)
            if (parsed == null) {
               logcat { "Notification ${sbn.key} has no text. Skipping..." }
               return@launch
            }
            notificationProcessor.onNotificationPosted(parsed)
            parsedSuccessfully = true
         }
         if (parsedSuccessfully) {
            scheduleDelayedActiveNotificationResync(sbn.key)
         }
      }
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

      coroutineScope.launch {
         mutex.withLock {
            notificationProcessor.onNotificationDismissed(sbn.key)
         }
      }
   }

   private fun scheduleDelayedActiveNotificationResync(key: String) {
      delayedResyncJobs.remove(key)?.cancel()
      delayedResyncJobs[key] = coroutineScope.launch {
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

      coroutineScope.launch {
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
      coroutineScope.launch {
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

private const val CDM_WAIT_ATTEMPTS = 10
private val NOTIFICATION_STABILIZATION_DELAY = 750.milliseconds
