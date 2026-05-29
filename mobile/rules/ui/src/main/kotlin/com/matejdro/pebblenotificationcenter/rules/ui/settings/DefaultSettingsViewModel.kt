package com.matejdro.pebblenotificationcenter.rules.ui.settings

import androidx.compose.runtime.Stable
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import com.matejdro.pebblenotificationcenter.common.logging.ActionLogger
import com.matejdro.pebblenotificationcenter.navigation.keys.DefaultSettingsScreenKey
import com.matejdro.pebblenotificationcenter.rules.RULE_ID_DEFAULT_SETTINGS
import com.matejdro.pebblenotificationcenter.rules.RulesRepository
import com.matejdro.pebblenotificationcenter.rules.keys.PreferenceKeyWithDefault
import com.matejdro.pebblenotificationcenter.rules.keys.set
import com.matejdro.pebblenotificationcenter.rules.keys.setTo
import dev.zacsweers.metro.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.first
import si.inova.kotlinova.core.outcome.CoroutineResourceManager
import si.inova.kotlinova.core.outcome.Outcome
import si.inova.kotlinova.navigation.services.ContributesScopedService
import si.inova.kotlinova.navigation.services.SingleScreenViewModel

@Stable
@Inject
@ContributesScopedService
class DefaultSettingsViewModel(
   private val resources: CoroutineResourceManager,
   private val actionLogger: ActionLogger,
   private val rulesRepository: RulesRepository,
   private val preferenceStore: DataStore<Preferences>,
) : SingleScreenViewModel<DefaultSettingsScreenKey>(resources.scope) {
   private val _uiState = MutableStateFlow<Outcome<DefaultSettingsState>>(Outcome.Progress())
   val uiState: StateFlow<Outcome<DefaultSettingsState>> = _uiState

   override fun onServiceRegistered() {
      actionLogger.logAction { "DefaultSettingsViewModel.onServiceRegistered()" }

      resources.launchResourceControlTask(_uiState) {
         rulesRepository.getAll().first { it is Outcome.Success || it is Outcome.Error }

         emitAll(
            combine(
               rulesRepository.getRulePreferences(RULE_ID_DEFAULT_SETTINGS),
               preferenceStore.data
            ) { rulePreferences, globalPreferences ->
               Outcome.Success(DefaultSettingsState(rulePreferences, globalPreferences))
            }
         )
      }
   }

   fun <T> updatePreference(key: PreferenceKeyWithDefault<T>, value: T) = resources.launchWithExceptionReporting {
      actionLogger.logAction { "DefaultSettingsViewModel.updatePreference($key)" }
      rulesRepository.updateRulePreferences(RULE_ID_DEFAULT_SETTINGS, key setTo value)
   }

   fun <T> updateGlobalPreference(key: PreferenceKeyWithDefault<T>, value: T) = resources.launchWithExceptionReporting {
      actionLogger.logAction { "DefaultSettingsViewModel.updateGlobalPreference($key)" }
      preferenceStore.edit {
         it[key] = value
      }
   }
}

@Stable
data class DefaultSettingsState(
   val preferences: Preferences,
   val globalPreferences: Preferences,
)
