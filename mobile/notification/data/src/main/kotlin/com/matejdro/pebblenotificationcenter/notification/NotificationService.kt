package com.matejdro.pebblenotificationcenter.notification

import android.content.ComponentName
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
import kotlinx.coroutines.CoroutineExceptionHandler
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
import java.util.concurrent.ConcurrentHashMap
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
   private val events = Channel<ServiceEvent>(Channel.UNLIMITED)

   // Posts Android delivered that the event loop has not applied yet, per key.
   private val pendingPosts = ConcurrentHashMap<String, Int>()

   // Keys posted during this session: only a key's first post schedules a reconcile.
   private val seenKeys: MutableSet<String> = ConcurrentHashMap.newKeySet()

   @Volatile
   private var scheduledReconcile: Job? = null

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

      serviceScope = CoroutineScope(
         coroutineScope.coroutineContext +
            SupervisorJob(coroutineScope.coroutineContext[Job]) +
            CoroutineExceptionHandler { _, throwable -> errorReporter.report(throwable) }
      )
      instance = this

      super.onCreate()
      processEvents()
   }

   override fun onDestroy() {
      logcat { "Stopping notification service" }
      serviceScope.cancel()
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
      pendingPosts.merge(sbn.key, 1, Int::plus)
      events.trySend(ServiceEvent.Posted(sbn))
   }

   override fun onNotificationRankingUpdate(rankingMap: RankingMap) {
      super.onNotificationRankingUpdate(rankingMap)
      // Silencing a conversation, pausing an app (Digital Wellbeing) or a Do Not Disturb change arrives only as a
      // ranking update, with no post or removal.
      scheduleReconcile()
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
                     try {
                        mutex.withLock {
                           val parsed = parseNotification(event.sbn)
                           if (parsed == null) {
                              logcat { "Notification ${event.sbn.key} has no text. Skipping..." }
                              // An update that removed all text must not leave the old text on the watch.
                              if (notificationProcessor.getNotificationByKey(event.sbn.key) != null) {
                                 notificationProcessor.onNotificationDismissed(event.sbn.key)
                              }
                           } else {
                              notificationProcessor.onNotificationPosted(parsed)
                           }
                        }
                     } finally {
                        pendingPosts.computeIfPresent(event.sbn.key) { _, count -> (count - 1).takeIf { it > 0 } }
                     }
                     if (seenKeys.add(event.sbn.key)) {
                        // A new notification can come with group changes elsewhere in the shade (a summary
                        // replaced, siblings re-posted); one reconcile shortly after catches those.
                        scheduleReconcile()
                     }
                  }

                  is ServiceEvent.Removed -> {
                     seenKeys.remove(event.key)
                     mutex.withLock {
                        notificationProcessor.onNotificationDismissed(event.key)
                     }
                  }

                  is ServiceEvent.Reconcile -> {
                     resyncActiveNotificationsNow()
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
      // Some phones (Samsung, Xiaomi) unbind listeners in the background. Without this the mirror silently stops.
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
         try {
            requestRebind(ComponentName(this, NotificationService::class.java))
         } catch (e: SecurityException) {
            errorReporter.report(e)
         }
      }
   }

   val isListenerBound: Boolean
      get() = bound

   private suspend fun parseNotification(sbn: StatusBarNotification): ParsedNotification? {
      val ranking = Ranking()
      val hasRanking = currentRanking.getRanking(sbn.key, ranking)

      return notificationParser.parse(
         sbn,
         getFastNotificationChannel(sbn, ranking, hasRanking),
         ranking.takeIf { hasRanking },
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

      // Active on the phone but left out of this pass: they must not be mistaken for dismissed.
      val skippedKeys = HashSet<String>()
      val parsedNotifications = currentNotifications.mapNotNull { sbn ->
         if ((pendingPosts[sbn.key] ?: 0) > 0) {
            // Android already lists this version but its post is still queued. Let that post apply it (with its
            // buzz); applying it quietly here would turn the real post into a silent "identical" update.
            skippedKeys += sbn.key
            return@mapNotNull null
         }
         val parsed = try {
            parseNotification(sbn)
         } catch (e: CancellationException) {
            throw e
         } catch (e: Exception) {
            // One malformed notification must not stop the rest of the shade from being mirrored.
            errorReporter.report(e)
            skippedKeys += sbn.key
            null
         }
         if (parsed == null) {
            logcat { "Notification ${sbn.key} has no text. Skipping..." }
         }

         parsed
      }

      notificationProcessor.onActiveNotificationsResynced(parsedNotifications, skippedKeys)
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
      events.trySend(ServiceEvent.Removed(sbn.key))
   }

   /**
    * At most one reconcile is waiting at a time, so a notification that updates every second (navigation,
    * downloads) cannot flood the watch with full resyncs. It runs in order with posts and removals.
    */
   @Synchronized
   private fun scheduleReconcile() {
      if (scheduledReconcile?.isActive == true) {
         return
      }
      scheduledReconcile = serviceScope.launch {
         delay(NOTIFICATION_STABILIZATION_DELAY)
         events.trySend(ServiceEvent.Reconcile)
      }
   }

   /** @return true when Android still lists the notification and was asked to cancel it. */
   fun cancelIfActive(key: String): Boolean {
      val active = try {
         activeNotifications.any { it.key == key }
      } catch (e: SecurityException) {
         errorReporter.report(e)
         return false
      }
      if (!active) {
         return false
      }
      return try {
         cancelNotification(key)
         true
      } catch (e: SecurityException) {
         errorReporter.report(e)
         false
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
   data object Reconcile : ServiceEvent
}

private const val CDM_WAIT_ATTEMPTS = 10
private val NOTIFICATION_STABILIZATION_DELAY = 750.milliseconds
