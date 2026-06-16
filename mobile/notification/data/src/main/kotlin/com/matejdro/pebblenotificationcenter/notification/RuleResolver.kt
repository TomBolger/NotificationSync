package com.matejdro.pebblenotificationcenter.notification

import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.emptyPreferences
import com.matejdro.pebblenotificationcenter.common.preferences.plus
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.rules.RULE_ID_DEFAULT_SETTINGS
import com.matejdro.pebblenotificationcenter.rules.MasterSwitch
import com.matejdro.pebblenotificationcenter.rules.RuleOption
import com.matejdro.pebblenotificationcenter.rules.RulesRepository
import com.matejdro.pebblenotificationcenter.rules.keys.get
import dev.zacsweers.metro.Inject
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import si.inova.kotlinova.core.outcome.Outcome

@Inject
class RuleResolver(private val rulesRepository: RulesRepository) {
   suspend fun resolveRules(notification: ParsedNotification): ResolvedRules {
      val rules = rulesRepository.getAll().firstSuccessOrThrow()

      val matchingRules = rules.mapNotNull { rule ->
         val preferences = rulesRepository.getRulePreferences(rule.id).first()

         val nameIfNotDefault = rule.name.takeIf { rule.id != RULE_ID_DEFAULT_SETTINGS }

         (nameIfNotDefault to preferences).takeIf { rule.id == RULE_ID_DEFAULT_SETTINGS || preferences.matches(notification) }
      }

      return ResolvedRules(
         involvedRules = matchingRules.mapNotNull { (name, _) -> name },
         preferences = matchingRules.map { (_, preferences) -> preferences }.fold(emptyPreferences(), Preferences::plus),
         hasExplicitShowRule = matchingRules.any { (name, preferences) ->
            name != null && preferences[RuleOption.masterSwitch] == MasterSwitch.SHOW
         },
      )
   }

   private fun Preferences.matches(notification: ParsedNotification): Boolean {
      val conditionPkg = this[RuleOption.conditionAppPackage]
      if (conditionPkg != null && conditionPkg != notification.pkg) {
         return false
      }

      val conditionChannels = this[RuleOption.conditionNotificationChannels]
      if (DIAGNOSTIC_DISABLE_NOTIFICATION_GRANULARITY && conditionChannels.isNotEmpty()) {
         return false
      }
      if (conditionChannels.isNotEmpty() && !conditionChannels.contains(notification.channel)) {
         return false
      }

      val whitelistRegexes = this[RuleOption.conditionWhitelistRegexes].map { Regex(it) }
      if (whitelistRegexes.isNotEmpty() && !whitelistRegexes.all(notification::containsRegex)) {
         return false
      }

      val blacklistRegexes = this[RuleOption.conditionBlacklistRegexes].map { Regex(it) }
      if (blacklistRegexes.any(notification::containsRegex)) {
         return false
      }

      return true
   }
}

private const val DIAGNOSTIC_DISABLE_NOTIFICATION_GRANULARITY = false

data class ResolvedRules(
   val involvedRules: List<String>,
   val preferences: Preferences,
   val hasExplicitShowRule: Boolean,
)

private fun ParsedNotification.containsRegex(regex: Regex): Boolean {
   return regex.containsMatchIn(title) || regex.containsMatchIn(subtitle) || regex.containsMatchIn(body)
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
