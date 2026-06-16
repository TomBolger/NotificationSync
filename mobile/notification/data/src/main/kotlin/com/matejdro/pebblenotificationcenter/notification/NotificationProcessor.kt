@file:Suppress("LongMethod")

package com.matejdro.pebblenotificationcenter.notification

import android.content.Context
import android.os.Build
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebblenotificationcenter.bluetooth.WatchSyncer
import com.matejdro.pebblenotificationcenter.bluetooth.WatchappOpenController
import com.matejdro.pebblenotificationcenter.bluetooth.StockNotificationAction
import com.matejdro.pebblenotificationcenter.common.di.AndroidVersion
import com.matejdro.pebblenotificationcenter.notification.history.HideReason
import com.matejdro.pebblenotificationcenter.notification.history.HistoryInserter
import com.matejdro.pebblenotificationcenter.notification.history.MuteReason
import com.matejdro.pebblenotificationcenter.notification.model.Action
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.notification.model.PauseStatus
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import com.matejdro.pebblenotificationcenter.notification.model.any
import com.matejdro.pebblenotificationcenter.notification.utils.parseVibrationPattern
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.MasterSwitch
import com.matejdro.pebblenotificationcenter.rules.RuleOption
import com.matejdro.pebblenotificationcenter.rules.keys.get
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dev.zacsweers.metro.SingleIn
import dispatch.core.DefaultCoroutineScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import logcat.logcat
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicReference

@Inject
@SingleIn(AppScope::class)
@ContributesBinding(AppScope::class)
class NotificationProcessor(
   private val context: Context,
   private val watchSyncer: WatchSyncer,
   private val openController: WatchappOpenController,
   private val ruleResolver: RuleResolver,
   private val globalPreferenceStore: DataStore<Preferences>,
   private val pauseController: PauseController,
   private val historyInserter: HistoryInserter,
   private val phoneStateDetector: PhoneStateDetector,
   private val defaultScope: DefaultCoroutineScope,
   @AndroidVersion
   private val androidVersion: Int,
) : NotificationRepository {
   private val notifications = ConcurrentHashMap<Int, ProcessedNotification>()
   private val notificationIdsByKeys = HashMap<String, Int>()

   private var nextVibration: AtomicReference<IntArray?> = AtomicReference(null)

   init {
      defaultScope.launch {
         watchSyncer.stockNotificationActions.collect { action ->
            when (action) {
               is StockNotificationAction.Dismiss -> {
                  NotificationService.instance?.cancelNotification(action.key)
                  onNotificationDismissed(action.key)
               }
            }
         }
      }
   }

   suspend fun onNotificationPosted(parsedNotification: ParsedNotification, suppressVibration: Boolean = false) {
      val previousNotification = notificationIdsByKeys[parsedNotification.key]?.let { notifications[it] }
      val notification = parsedNotification.withRicherFieldsFrom(previousNotification?.systemData)
      val resolvedRules = ruleResolver.resolveRules(notification)
      val affectedRules = resolvedRules.involvedRules
      val settings = resolvedRules.preferences
      logcat { "Notification ${notification.key} rules: $affectedRules" }
      for (setting in settings.asMap()) {
         logcat { "   ${setting.key} = ${setting.value}" }
      }

      val hideReason = shouldHide(notification, settings)
      if (hideReason != null) {
         historyInserter.insertHistoryEntry(notification, affectedRules, hideReason, null)
         onNotificationDismissed(notification.key)
         return
      }

      if (shouldSkipBecausePhoneUnlocked()) {
         logcat { "Hiding: phone is unlocked" }
         historyInserter.insertHistoryEntry(notification, affectedRules, HideReason.PHONE_UNLOCKED, null)
         return
      }

      val isUpdate = previousNotification != null
      val pauseStatusBeforeInsert = pauseController.computePauseStatus(notification)
      if (!isUpdate) {
         pauseController.onNewNotification(notification, settings)
      }
      val pauseStatus = pauseController.computePauseStatus(notification)

      val actions = processActions(notification, pauseStatus, settings)

      val (muteReason, vibrationPattern) = getVibrationPattern(
         previousNotification,
         notification,
         suppressVibration,
         settings,
         pauseStatusBeforeInsert,
         resolvedRules.hasExplicitShowRule,
      )

      val regexesToReplace = settings[RuleOption.regexReplacements]
      val regexReplacedParsedNotification = notification.copy(
         title = replaceRegexes(notification.title, regexesToReplace),
         subtitle = replaceRegexes(notification.subtitle, regexesToReplace),
         body = replaceRegexes(notification.body, regexesToReplace),
      )

      val initialProcessedNotification = ProcessedNotification(
         regexReplacedParsedNotification,
         0,
         actions,
         unread = if (suppressVibration && previousNotification != null) previousNotification.unread else !suppressVibration,
         paused = pauseStatus,
         vibrated = if (suppressVibration && previousNotification != null) {
            previousNotification.vibrated
         } else {
            vibrationPattern != null
         }
      )
      val bucketId = watchSyncer.syncNotification(initialProcessedNotification, settings)

      val processedNotification = initialProcessedNotification.copy(bucketId = bucketId)

      logcat {
         "Notification flags: " +
            "suppress=$suppressVibration " +
            "silent=${parsedNotification.isSilent} " +
            "dnd=${parsedNotification.isFilteredByDoNotDisturb}"
      }
      if (previousNotification != null && previousNotification.bucketId != bucketId) {
         notifications.remove(previousNotification.bucketId)
      }
      notifications[bucketId] = processedNotification
      notificationIdsByKeys[notification.key] = bucketId
      if (vibrationPattern != null && !usingStockPebbleOsNotifications()) {
         logcat { "Vibrating with ${vibrationPattern.contentToString()}" }
         nextVibration.set(vibrationPattern)
         openController.setNextWatchappOpenNotificationBucket(bucketId)
         openController.openWatchapp()
      }

      historyInserter.insertHistoryEntry(regexReplacedParsedNotification, affectedRules, null, muteReason)
   }

   private fun ParsedNotification.withRicherFieldsFrom(previous: ParsedNotification?): ParsedNotification {
      if (previous == null) {
         return this
      }

      return copy(
         subtitle = richerText(
            current = subtitle,
            previous = previous.subtitle,
            preservePreviousByTimestamp = !timestamp.isAfter(previous.timestamp),
         ),
         body = richerText(
            current = body,
            previous = previous.body,
            preservePreviousByTimestamp = !timestamp.isAfter(previous.timestamp),
         ),
         nativeActions = if (previous.nativeActions.size > nativeActions.size) {
            previous.nativeActions
         } else {
            nativeActions
         },
         iconDrawable = iconDrawable ?: previous.iconDrawable,
         largeImage = largeImage ?: previous.largeImage,
      )
   }

   private fun richerText(current: String, previous: String, preservePreviousByTimestamp: Boolean): String {
      if (current.isBlank()) {
         return previous.ifBlank { current }
      }
      if (previous.isBlank()) {
         return current
      }

      if (preservePreviousByTimestamp && previous.length > current.length) {
         return previous
      }
      if (previous.length > current.length && previous.contains(current)) {
         return previous
      }

      return current
   }

   private fun shouldHide(
      notification: ParsedNotification,
      preferences: Preferences,
   ): HideReason? {
      if (notification.forceVibrate) {
         logcat { "Force notification: always show" }
         return null
      }

      if (preferences[RuleOption.masterSwitch] == MasterSwitch.HIDE) {
         logcat { "Hiding: master switch is hidden" }
         return HideReason.MASTER_SWITCH
      }

      if (notification.isOngoing && preferences[RuleOption.hideOngoingNotifications]) {
         logcat { "Hiding: ongoing" }
         return HideReason.ONGOING_NOTIFICATION
      }

      if (notification.groupSummary && preferences[RuleOption.hideGroupSummaryNotifications]) {
         logcat { "Hiding: group summary" }
         return HideReason.GROUP_SUMMARY_NOTIFICATION
      }

      if (notification.localOnly && preferences[RuleOption.hideLocalOnlyNotifications]) {
         logcat { "Hiding: local only" }
         return HideReason.LOCAL_ONLY_NOTIFICATION
      }

      if (notification.media && preferences[RuleOption.hideMediaNotifications]) {
         logcat { "Hiding: media" }
         return HideReason.MEDIA_NOTIFICATION
      }

      return null
   }

   private suspend fun shouldSkipBecausePhoneUnlocked(): Boolean {
      return globalPreferenceStore.data.first()[GlobalPreferenceKeys.skipNotificationsWhenPhoneUnlocked] &&
         phoneStateDetector.isPhoneUnlocked()
   }

   private suspend fun usingStockPebbleOsNotifications(): Boolean {
      return globalPreferenceStore.data.first()[GlobalPreferenceKeys.stockPebbleOsNotifications]
   }

   @Suppress("CyclomaticComplexMethod", "CognitiveComplexMethod") // Lots of successive checks
   private suspend fun getVibrationPattern(
      previousNotification: ProcessedNotification?,
      notification: ParsedNotification,
      suppressVibration: Boolean,
      preferences: Preferences,
      pausedBeforeInsert: PauseStatus,
      hasExplicitShowRule: Boolean,
   ): Pair<MuteReason?, IntArray?> {
      val pattern = (
         notification.overrideVibrationPattern.takeIf { preferences[RuleOption.useNotificationVibrationPattern] }
            ?: parseVibrationPattern(preferences[RuleOption.vibrationPattern])
            ?: error("Invalid vibration pattern '${preferences[RuleOption.vibrationPattern]}'")
         )
         .map { it.toInt() }
         .toIntArray()

      if (notification.forceVibrate) {
         logcat { "Force notification: always vibrate" }
         return null to pattern
      }

      if (suppressVibration) {
         logcat { "Not vibrating: suppressVibration flag" }
         return MuteReason.APP_STARTUP to null
      }

      if (globalPreferenceStore.data.first()[GlobalPreferenceKeys.muteWatch]) {
         logcat { "Not vibrating: watch muted" }
         return MuteReason.WATCH_MUTE to null
      }

      if (pausedBeforeInsert.any) {
         logcat { "Not vibrating: paused" }
         return MuteReason.PAUSE to null
      }

      if (preferences[RuleOption.masterSwitch] == MasterSwitch.MUTE) {
         logcat { "Not vibrating: master switch" }
         return MuteReason.MASTER_SWITCH to null
      }

      if (notification.isSilent && preferences[RuleOption.muteSilentNotifications] && !hasExplicitShowRule) {
         logcat { "Not vibrating: silent notification" }
         return MuteReason.SILENT_NOTIFICATION to null
      }

      if (notification.isFilteredByDoNotDisturb && preferences[RuleOption.muteDndNotifications]) {
         logcat { "Not vibrating: DND filter" }
         return MuteReason.DO_NOT_DISTURB to null
      }

      val identicalText = previousNotification != null &&
         previousNotification.systemData.title == notification.title &&
         previousNotification.systemData.subtitle == notification.subtitle &&
         previousNotification.systemData.body == notification.body

      if (identicalText && preferences[RuleOption.muteIdenticalNotifications]) {
         logcat { "Not vibrating: identical text notification" }
         return MuteReason.IDENTICAL_TEXT to null
      }

      return null to pattern
   }

   @Suppress("CognitiveComplexMethod") // A bunch of ifs for separate actions. Clearer when left together.
   private fun processActions(
      parsedNotification: ParsedNotification,
      pauseStatus: PauseStatus,
      settings: Preferences,
   ): List<Action> {
      val ncActions = buildList {
         add(Action.Dismiss(title = context.getString(R.string.dismiss), id = size.toUByte()))

         if (androidVersion >= Build.VERSION_CODES.O) {
            add(Action.Snooze(title = context.getString(R.string.snooze), id = size.toUByte()))
         }

         if (parsedNotification.largeImage != null) {
            add(Action.ShowImage(title = context.getString(R.string.show_image), id = size.toUByte()))
         }

         if (!DIAGNOSTIC_COARSE_NOTIFICATION_ACTIONS_ONLY) {
            for (taskerTask in settings[RuleOption.taskerTaskActions]) {
               add(Action.TaskerTask(taskerTask, size.toUByte()))
            }

            add(
               Action.PauseApp(
                  title = if (pauseStatus.app) {
                     context.getString(R.string.unpause_app)
                  } else {
                     context.getString(R.string.pause_app)
                  },
                  id = size.toUByte()
               )
            )
            add(
               Action.PauseConversation(
                  title = if (pauseStatus.conversation) {
                     context.getString(R.string.unpause_conversation)
                  } else {
                     context.getString(R.string.pause_conversation)
                  },
                  id = size.toUByte()
               )
            )
            if (settings[RuleOption.masterSwitch] != MasterSwitch.MUTE) {
               add(Action.SilenceApp(title = context.getString(R.string.silence_app), id = size.toUByte()))
            }
         }
      }

      val appActions = parsedNotification.nativeActions.mapIndexed { index, action ->
         val text = if (ncActions.any { it.title == action.text }) {
            context.getString(R.string.app_suffix, action.text)
         } else {
            action.text
         }

         val id = (ncActions.size + index).toUByte()

         val remoteInputResultKey = action.remoteInputResultKey
         if (remoteInputResultKey == null) {
            Action.Native(title = text, intent = action.pendingIntent, id)
         } else {
            Action.Reply(
               title = text,
               intent = action.pendingIntent,
               remoteInputResultKey = remoteInputResultKey,
               cannedTexts = action.cannedTexts,
               allowFreeFormInput = action.allowFreeFormInput,
               id = id
            )
         }
      }

      return ncActions + appActions
   }

   override suspend fun notifyPackagePauseStatusChanged(pkg: String) {
      val activeMatchingNotifications = getAllActiveNotifications().filter { it.systemData.pkg == pkg }
      for (notificationIterator in activeMatchingNotifications) {
         val newNotification = notifications.compute(notificationIterator.bucketId) { _, oldNotification ->
            if (oldNotification == null) {
               return@compute null
            }
            val newPaused = pauseController.computePauseStatus(oldNotification.systemData)

            if (newPaused != oldNotification.paused) {
               oldNotification.copy(
                  paused = newPaused,
                  actions = oldNotification.actions.renamePauseActions(newPaused)
               )
            } else {
               oldNotification
            }
         }
         if (newNotification != null) {
            notifications[newNotification.bucketId] = newNotification

            if (newNotification != notificationIterator) {
               val preferences = ruleResolver.resolveRules(newNotification.systemData).preferences
               watchSyncer.syncNotification(newNotification, preferences)
            }
         }
      }
   }

   suspend fun onNotificationDismissed(key: String) {
      val notificationId = notificationIdsByKeys.remove(key)
      if (notificationId != null) {
         val processedNotification = notifications.remove(notificationId)
         if (processedNotification != null) {
            pauseController.onNotificationDismissed(processedNotification.systemData)
         }
      }

      watchSyncer.clearNotification(key)
   }

   suspend fun onActiveNotificationsResynced(activeNotifications: List<ParsedNotification>) {
      if (activeNotifications.isEmpty()) {
         onNotificationsCleared()
         return
      }

      val activeKeys = activeNotifications.mapTo(HashSet()) { it.key }
      val missingKeys = notificationIdsByKeys.keys
         .filterNot { it in activeKeys }
         .toList()

      for (key in missingKeys) {
         onNotificationDismissed(key)
      }

      for (notification in activeNotifications) {
         onNotificationPosted(notification, suppressVibration = true)
      }
   }

   suspend fun onNotificationsCleared() {
      notifications.clear()
      notificationIdsByKeys.clear()

      watchSyncer.clearAllNotifications()
   }

   override fun getNotification(bucketId: Int): ProcessedNotification? {
      return notifications[bucketId]
   }

   override fun getAllActiveNotifications(): Collection<ProcessedNotification> {
      return notifications.values.toList()
   }

   fun getNotificationByKey(key: String): ProcessedNotification? {
      return notificationIdsByKeys[key]?.let { notifications[it] }
   }

   override fun pollNextVibration(): IntArray? {
      return nextVibration.getAndSet(null)
   }

   fun peekNextVibration(): IntArray? {
      return nextVibration.get()
   }

   override fun resetNextVibration(value: IntArray) {
      nextVibration.compareAndSet(null, value)
   }

   override suspend fun markAsRead(bucketId: Int) {
      logcat { "Marking $bucketId as read" }
      val notification = notifications.computeIfPresent(bucketId) { _, value ->
         value.copy(unread = false)
      } ?: return

      val preferences = ruleResolver.resolveRules(notification.systemData).preferences

      watchSyncer.prepareNotificationReadStatus(notification, preferences)
   }

   private fun List<Action>.renamePauseActions(newPausedStatus: PauseStatus): List<Action> = map { action ->
      when (action) {
         is Action.PauseApp -> {
            action.copy(
               title = if (newPausedStatus.app) {
                  context.getString(R.string.unpause_app)
               } else {
                  context.getString(R.string.pause_app)
               },
            )
         }

         is Action.PauseConversation -> {
            action.copy(
               title = if (newPausedStatus.conversation) {
                  context.getString(R.string.unpause_conversation)
               } else {
                  context.getString(R.string.pause_conversation)
               },
            )
         }

         else -> {
            action
         }
      }
   }
}

private const val DIAGNOSTIC_COARSE_NOTIFICATION_ACTIONS_ONLY = false
