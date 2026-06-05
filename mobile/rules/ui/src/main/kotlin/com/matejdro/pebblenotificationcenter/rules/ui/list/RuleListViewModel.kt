package com.matejdro.pebblenotificationcenter.rules.ui.list

import android.content.Context
import androidx.compose.runtime.Stable
import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebblenotificationcenter.common.logging.ActionLogger
import com.matejdro.pebblenotificationcenter.history.HistoryEntry
import com.matejdro.pebblenotificationcenter.history.HistoryRepository
import com.matejdro.pebblenotificationcenter.navigation.keys.RuleListScreenKey
import com.matejdro.pebblenotificationcenter.rules.MasterSwitch
import com.matejdro.pebblenotificationcenter.rules.RULE_ID_DEFAULT_SETTINGS
import com.matejdro.pebblenotificationcenter.rules.RuleMetadata
import com.matejdro.pebblenotificationcenter.rules.RuleOption
import com.matejdro.pebblenotificationcenter.rules.RulesRepository
import com.matejdro.pebblenotificationcenter.rules.keys.get
import com.matejdro.pebblenotificationcenter.rules.keys.setTo
import dev.zacsweers.metro.Inject
import dispatch.core.withDefault
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
   private val context: Context,
) : SingleScreenViewModel<RuleListScreenKey>(resources.scope) {
   private val _uiState = MutableStateFlow<Outcome<RuleListState>>(Outcome.Progress())
   val uiState: StateFlow<Outcome<RuleListState>> = _uiState

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

   private suspend fun setAppRule(packageName: String, appName: String, enabled: Boolean, existingRuleId: Int?) {
      val ruleId = existingRuleId ?: rulesRepository.insert(appName)
      rulesRepository.updateRulePreferences(
         ruleId,
         RuleOption.conditionAppPackage setTo packageName,
         RuleOption.conditionNotificationChannels setTo emptySet(),
         RuleOption.masterSwitch setTo if (enabled) MasterSwitch.SHOW else MasterSwitch.MUTE,
      )
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
               appWide = channels.isEmpty(),
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

private data class InstalledApp(
   val packageName: String,
   val name: String,
)

private data class AppRule(
   val id: Int,
   val packageName: String,
   val masterSwitch: MasterSwitch,
   val appWide: Boolean,
)

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
