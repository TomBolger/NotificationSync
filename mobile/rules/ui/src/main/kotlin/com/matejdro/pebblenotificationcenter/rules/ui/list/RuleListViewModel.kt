package com.matejdro.pebblenotificationcenter.rules.ui.list

import android.content.Context
import androidx.compose.runtime.Stable
import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebblenotificationcenter.common.logging.ActionLogger
import com.matejdro.pebblenotificationcenter.history.HistoryEntry
import com.matejdro.pebblenotificationcenter.history.HistoryRepository
import com.matejdro.pebblenotificationcenter.navigation.keys.RuleListScreenKey
import com.matejdro.pebblenotificationcenter.notification.NotificationServiceController
import com.matejdro.pebblenotificationcenter.notification.model.LightNotificationChannel
import com.matejdro.pebblenotificationcenter.rules.MasterSwitch
import com.matejdro.pebblenotificationcenter.rules.RULE_ID_DEFAULT_SETTINGS
import com.matejdro.pebblenotificationcenter.rules.RuleMetadata
import com.matejdro.pebblenotificationcenter.rules.RuleOption
import com.matejdro.pebblenotificationcenter.rules.RulesRepository
import com.matejdro.pebblenotificationcenter.rules.keys.get
import com.matejdro.pebblenotificationcenter.rules.keys.setTo
import dev.zacsweers.metro.Inject
import dispatch.core.withDefault
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.first
import si.inova.kotlinova.core.outcome.CoroutineResourceManager
import si.inova.kotlinova.core.outcome.Outcome
import si.inova.kotlinova.navigation.services.ContributesScopedService
import si.inova.kotlinova.navigation.services.SingleScreenViewModel
import java.time.Instant

@Stable
@Inject
@ContributesScopedService
class RuleListViewModel(
   private val resources: CoroutineResourceManager,
   private val actionLogger: ActionLogger,
   private val rulesRepository: RulesRepository,
   private val historyRepository: HistoryRepository,
   private val notificationServiceController: NotificationServiceController,
   private val context: Context,
) : SingleScreenViewModel<RuleListScreenKey>(resources.scope) {
   private val _uiState = MutableStateFlow<Outcome<RuleListState>>(Outcome.Progress())
   val uiState: StateFlow<Outcome<RuleListState>> = _uiState

   private val _appDetailsState = MutableStateFlow<Outcome<NotificationAppDetailsState>?>(null)
   val appDetailsState: StateFlow<Outcome<NotificationAppDetailsState>?> = _appDetailsState

   private val refreshRequests = MutableStateFlow(0)

   override fun onServiceRegistered() {
      actionLogger.logAction { "RuleListViewModel.onServiceRegistered()" }

      resources.launchResourceControlTask(_uiState) {
         combine(
            rulesRepository.getAll(),
            historyRepository.getHistory(),
            refreshRequests
         ) { rulesOutcome, historyOutcome, _ ->
            when {
               rulesOutcome is Outcome.Error -> Outcome.Error(rulesOutcome.exception)
               historyOutcome is Outcome.Error -> Outcome.Error(historyOutcome.exception)
               rulesOutcome is Outcome.Success && historyOutcome is Outcome.Success -> {
                  Outcome.Success(
                     buildState(
                        rules = rulesOutcome.data,
                        history = historyOutcome.data,
                     )
                  )
               }
               else -> Outcome.Progress()
            }
         }.let { emitAll(it) }
      }
   }

   fun setAllEnabled(enabled: Boolean) = resources.launchWithExceptionReporting {
      actionLogger.logAction { "RuleListViewModel.setAllEnabled($enabled)" }
      val masterSwitch = if (enabled) MasterSwitch.SHOW else MasterSwitch.MUTE
      rulesRepository.updateRulePreferences(RULE_ID_DEFAULT_SETTINGS, RuleOption.masterSwitch setTo masterSwitch)

      uiState.value.data?.apps.orEmpty().forEach { app ->
         if (app.packageName != null) {
            setAppRule(app.packageName, app.name, enabled, app.ruleId)
         }
      }
      refreshRequests.value += 1
   }

   fun setAppEnabled(app: NotificationAppState, enabled: Boolean) = resources.launchWithExceptionReporting {
      actionLogger.logAction { "RuleListViewModel.setAppEnabled(${app.packageName}, $enabled)" }
      val packageName = app.packageName ?: return@launchWithExceptionReporting

      setAppRule(packageName, app.name, enabled, app.ruleId)
      refreshRequests.value += 1
   }

   fun openAppDetails(app: NotificationAppState) = resources.launchWithExceptionReporting {
      actionLogger.logAction { "RuleListViewModel.openAppDetails(${app.packageName})" }
      val packageName = app.packageName ?: return@launchWithExceptionReporting

      _appDetailsState.value = Outcome.Progress()
      _appDetailsState.value = Outcome.Success(buildAppDetails(packageName, app.name))
   }

   fun closeAppDetails() {
      actionLogger.logAction { "RuleListViewModel.closeAppDetails()" }
      _appDetailsState.value = null
   }

   fun setAppMasterSwitch(masterSwitch: MasterSwitch) = resources.launchWithExceptionReporting {
      actionLogger.logAction { "RuleListViewModel.setAppMasterSwitch($masterSwitch)" }
      val details = appDetailsState.value?.data ?: return@launchWithExceptionReporting

      setAppRule(details.packageName, details.appName, masterSwitch, details.appWide.ruleId)
      refreshAfterDetailsRuleChange(details.packageName, details.appName)
   }

   fun setChannelMasterSwitch(channel: NotificationChannelRuleState, masterSwitch: MasterSwitch) {
      actionLogger.logAction { "RuleListViewModel.setChannelMasterSwitch(${channel.id}, $masterSwitch)" }
      resources.launchWithExceptionReporting {
         val details = appDetailsState.value?.data ?: return@launchWithExceptionReporting
         val ruleId = channel.ruleId ?: rulesRepository.insert("${details.appName}: ${channel.title}")

         rulesRepository.updateRulePreferences(
            ruleId,
            RuleOption.conditionAppPackage setTo details.packageName,
            RuleOption.conditionNotificationChannels setTo setOf(channel.id),
            RuleOption.masterSwitch setTo masterSwitch,
         )
         placeRuleAfterBroaderRules(ruleId)
         refreshAfterDetailsRuleChange(details.packageName, details.appName)
      }
   }

   private suspend fun setAppRule(packageName: String, appName: String, enabled: Boolean, existingRuleId: Int?) {
      setAppRule(packageName, appName, if (enabled) MasterSwitch.SHOW else MasterSwitch.MUTE, existingRuleId)
   }

   private suspend fun setAppRule(
      packageName: String,
      appName: String,
      masterSwitch: MasterSwitch,
      existingRuleId: Int?,
   ) {
      val ruleId = existingRuleId ?: rulesRepository.insert(appName)
      rulesRepository.updateRulePreferences(
         ruleId,
         RuleOption.conditionAppPackage setTo packageName,
         RuleOption.conditionNotificationChannels setTo emptySet(),
         RuleOption.masterSwitch setTo masterSwitch,
      )
      placeAppWideRuleBeforeChannelRules(ruleId)
   }

   private suspend fun refreshAfterDetailsRuleChange(packageName: String, appName: String) {
      refreshRequests.value += 1
      _appDetailsState.value = Outcome.Success(buildAppDetails(packageName, appName))
   }

   private suspend fun placeAppWideRuleBeforeChannelRules(ruleId: Int) {
      if (ruleId <= RULE_ID_DEFAULT_SETTINGS) return

      val rules = rulesRepository.getAll().firstSuccessOrThrow()
      if (rules.any { it.id == RULE_ID_DEFAULT_SETTINGS } && rules.indexOfFirst { it.id == ruleId } != 1) {
         rulesRepository.reorder(ruleId, 1)
      }
   }

   private suspend fun placeRuleAfterBroaderRules(ruleId: Int) {
      if (ruleId <= RULE_ID_DEFAULT_SETTINGS) return

      val rules = rulesRepository.getAll().firstSuccessOrThrow()
      val currentIndex = rules.indexOfFirst { it.id == ruleId }
      val targetIndex = rules.lastIndex
      if (currentIndex >= 0 && currentIndex != targetIndex && targetIndex > 0) {
         rulesRepository.reorder(ruleId, targetIndex)
      }
   }

   @Suppress("LongMethod")
   private suspend fun buildState(
      rules: List<RuleMetadata>,
      history: List<HistoryEntry>,
   ): RuleListState {
      val installedApps = loadInstalledApps()
      val installedByName = installedApps.groupBy { it.name.lowercase() }
      val historyStats = history.appStats()
      val preferencesByRule = rules.associateWith { rulesRepository.getRulePreferences(it.id).first() }
      val defaultMasterSwitch = preferencesByRule.entries
         .firstOrNull { it.key.id == RULE_ID_DEFAULT_SETTINGS }
         ?.value
         ?.get(RuleOption.masterSwitch)
         ?: MasterSwitch.SHOW

      val appRules = preferencesByRule.entries
         .mapNotNull { (rule, preferences) ->
            val packageName = preferences[RuleOption.conditionAppPackage] ?: return@mapNotNull null
            val channels = preferences[RuleOption.conditionNotificationChannels]
            AppRule(
               id = rule.id,
               packageName = packageName,
               masterSwitch = preferences[RuleOption.masterSwitch],
               channels = channels,
            )
         }
      val appRulesByPackage = appRules.groupBy { it.packageName }

      val rowsByPackage = LinkedHashMap<String, NotificationAppState>()
      installedApps.forEach { app ->
         val stats = historyStats[app.name.lowercase()]
         val rule = appRulesByPackage[app.packageName]?.firstOrNull { it.appWide }
         if (stats != null || rule != null) {
            rowsByPackage[app.packageName] = app.toNotificationAppState(
               stats = stats,
               appRule = rule,
               defaultMasterSwitch = defaultMasterSwitch,
            )
         }
      }

      appRules.forEach { rule ->
         if (!rowsByPackage.containsKey(rule.packageName)) {
            rowsByPackage[rule.packageName] = NotificationAppState(
               packageName = rule.packageName,
               name = installedApps.firstOrNull { it.packageName == rule.packageName }?.name ?: rule.packageName,
               notificationCount = 0,
               lastNotification = null,
               enabled = rule.masterSwitch == MasterSwitch.SHOW,
               masterSwitch = rule.masterSwitch,
               ruleId = rule.id,
            )
         }
      }

      val unknownHistoryRows = historyStats
         .filterKeys { appName -> installedByName[appName].isNullOrEmpty() }
         .map { (name, stats) ->
            NotificationAppState(
               packageName = null,
               name = stats.displayName.ifBlank { name },
               notificationCount = stats.count,
               lastNotification = stats.lastNotification,
               enabled = defaultMasterSwitch == MasterSwitch.SHOW,
               masterSwitch = defaultMasterSwitch,
               ruleId = null,
            )
         }

      val apps = (rowsByPackage.values + unknownHistoryRows)
         .sortedWith(
            compareByDescending<NotificationAppState> { it.notificationCount }
               .thenByDescending { it.lastNotification ?: Instant.EPOCH }
               .thenBy { it.name.lowercase() }
         )

      return RuleListState(
         apps = apps,
         defaultEnabled = defaultMasterSwitch == MasterSwitch.SHOW,
         defaultMasterSwitch = defaultMasterSwitch,
      )
   }

   private suspend fun buildAppDetails(
      packageName: String,
      appName: String,
   ): NotificationAppDetailsState {
      val rules = rulesRepository.getAll().firstSuccessOrThrow()
      val preferencesByRule = rules.associateWith { rulesRepository.getRulePreferences(it.id).first() }
      val defaultMasterSwitch = preferencesByRule.entries
         .firstOrNull { it.key.id == RULE_ID_DEFAULT_SETTINGS }
         ?.value
         ?.get(RuleOption.masterSwitch)
         ?: MasterSwitch.SHOW
      val appRules = preferencesByRule.entries
         .mapNotNull { (rule, preferences) ->
            if (preferences[RuleOption.conditionAppPackage] != packageName) return@mapNotNull null

            AppRule(
               id = rule.id,
               packageName = packageName,
               masterSwitch = preferences[RuleOption.masterSwitch],
               channels = preferences[RuleOption.conditionNotificationChannels],
            )
         }
      val appWideRule = appRules.lastOrNull { it.appWide }
      val appWideMasterSwitch = appWideRule?.masterSwitch ?: defaultMasterSwitch

      val channels = notificationServiceController.getNotificationChannels(packageName)
      val channelsById = LinkedHashMap<String, LightNotificationChannel>()
      channels.sortedBy { it.title.lowercase() }.forEach { channel ->
         channelsById[channel.id] = channel
      }
      appRules.flatMap { it.channels }.sorted().forEach { channelId ->
         channelsById.putIfAbsent(channelId, LightNotificationChannel(channelId, channelId))
      }

      val channelRules = channelsById.values.map { channel ->
         val matchingChannelRules = appRules.filter { !it.appWide && channel.id in it.channels }
         val exactChannelRule = matchingChannelRules.lastOrNull { it.channels == setOf(channel.id) }

         NotificationChannelRuleState(
            id = channel.id,
            title = channel.title.ifBlank { channel.id },
            masterSwitch = matchingChannelRules.lastOrNull()?.masterSwitch ?: appWideMasterSwitch,
            ruleId = exactChannelRule?.id,
            explicit = matchingChannelRules.isNotEmpty(),
         )
      }

      return NotificationAppDetailsState(
         packageName = packageName,
         appName = appName,
         appWide = NotificationAppWideRuleState(
            masterSwitch = appWideMasterSwitch,
            ruleId = appWideRule?.id,
            explicit = appWideRule != null,
         ),
         channels = channelRules,
      )
   }

   @Suppress("DEPRECATION")
   private suspend fun loadInstalledApps(): List<InstalledApp> = withDefault {
      val packageManager = context.packageManager
      packageManager.getInstalledPackages(0)
         .mapNotNull { packageInfo ->
            val info = packageInfo.applicationInfo ?: return@mapNotNull null
            InstalledApp(
               packageName = packageInfo.packageName,
               name = info.loadLabel(packageManager).toString(),
            )
         }
         .sortedBy { it.name.lowercase() }
   }
}

@Stable
data class RuleListState(
   val apps: List<NotificationAppState>,
   val defaultEnabled: Boolean,
   val defaultMasterSwitch: MasterSwitch,
)

@Stable
data class NotificationAppState(
   val packageName: String?,
   val name: String,
   val notificationCount: Int,
   val lastNotification: Instant?,
   val enabled: Boolean,
   val masterSwitch: MasterSwitch,
   val ruleId: Int?,
)

@Stable
data class NotificationAppDetailsState(
   val packageName: String,
   val appName: String,
   val appWide: NotificationAppWideRuleState,
   val channels: List<NotificationChannelRuleState>,
)

@Stable
data class NotificationAppWideRuleState(
   val masterSwitch: MasterSwitch,
   val ruleId: Int?,
   val explicit: Boolean,
)

@Stable
data class NotificationChannelRuleState(
   val id: String,
   val title: String,
   val masterSwitch: MasterSwitch,
   val ruleId: Int?,
   val explicit: Boolean,
)

private data class InstalledApp(
   val packageName: String,
   val name: String,
)

private data class AppRule(
   val id: Int,
   val packageName: String,
   val masterSwitch: MasterSwitch,
   val channels: Set<String>,
) {
   val appWide: Boolean = channels.isEmpty()
}

private data class AppHistoryStats(
   val displayName: String,
   val count: Int,
   val lastNotification: Instant,
)

private fun InstalledApp.toNotificationAppState(
   stats: AppHistoryStats?,
   appRule: AppRule?,
   defaultMasterSwitch: MasterSwitch,
) = NotificationAppState(
   packageName = packageName,
   name = name,
   notificationCount = stats?.count ?: 0,
   lastNotification = stats?.lastNotification,
   enabled = appRule?.let { it.masterSwitch == MasterSwitch.SHOW } ?: (defaultMasterSwitch == MasterSwitch.SHOW),
   masterSwitch = appRule?.masterSwitch ?: defaultMasterSwitch,
   ruleId = appRule?.id,
)

private fun List<HistoryEntry>.appStats(): Map<String, AppHistoryStats> {
   return groupBy { it.notificationTitle.lowercase() }.mapValues { (key, entries) ->
      val latest = entries.maxBy { it.time }
      AppHistoryStats(
         displayName = latest.notificationTitle.ifBlank { key },
         count = entries.size,
         lastNotification = latest.time,
      )
   }
}

private suspend fun <T> Flow<Outcome<T>>.firstSuccessOrThrow(): T {
   val result = first {
      it is Outcome.Success || it is Outcome.Error
   }

   return when (result) {
      is Outcome.Success -> result.data
      is Outcome.Error -> throw result.exception
      is Outcome.Progress -> error("Result should never be progress")
   }
}
