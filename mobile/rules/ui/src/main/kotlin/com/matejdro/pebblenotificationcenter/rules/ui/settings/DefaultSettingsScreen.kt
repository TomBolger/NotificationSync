@file:Suppress("MagicNumber", "MaxLineLength", "MultipleEmitters")

package com.matejdro.pebblenotificationcenter.rules.ui.settings

import android.os.Build
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.datastore.preferences.core.Preferences
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.matejdro.pebblenotificationcenter.navigation.instructions.OpenScreenOrReplaceExistingType
import com.matejdro.pebblenotificationcenter.navigation.keys.DefaultSettingsScreenKey
import com.matejdro.pebblenotificationcenter.navigation.keys.RuleDetailsScreenKey
import com.matejdro.pebblenotificationcenter.navigation.keys.TaskerTaskSetScreenKey
import com.matejdro.pebblenotificationcenter.navigation.util.rememberNavigationPopup
import com.matejdro.pebblenotificationcenter.navigation.util.trigger
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.MasterSwitch
import com.matejdro.pebblenotificationcenter.rules.RULE_ID_DEFAULT_SETTINGS
import com.matejdro.pebblenotificationcenter.rules.RuleOption
import com.matejdro.pebblenotificationcenter.rules.keys.IntListPreferenceKeyWithDefault
import com.matejdro.pebblenotificationcenter.rules.keys.PreferenceKeyWithDefault
import com.matejdro.pebblenotificationcenter.rules.keys.SetPreference
import com.matejdro.pebblenotificationcenter.rules.keys.StringListPreferenceKeyWithDefault
import com.matejdro.pebblenotificationcenter.rules.keys.get
import com.matejdro.pebblenotificationcenter.rules.ui.R
import com.matejdro.pebblenotificationcenter.rules.ui.dialogs.IntListScreenKey
import com.matejdro.pebblenotificationcenter.rules.ui.dialogs.RegexReplacementSetScreenKey
import com.matejdro.pebblenotificationcenter.rules.ui.dialogs.StringListScreenKey
import com.matejdro.pebblenotificationcenter.rules.ui.dialogs.VibrationPatternScreenKey
import com.matejdro.pebblenotificationcenter.ui.components.ProgressErrorSuccessScaffold
import si.inova.kotlinova.compose.result.ResultKey
import si.inova.kotlinova.navigation.navigator.Navigator
import si.inova.kotlinova.navigation.screens.InjectNavigationScreen
import si.inova.kotlinova.navigation.screens.Screen

@InjectNavigationScreen
class DefaultSettingsScreen(
   private val viewModel: DefaultSettingsViewModel,
   private val navigator: Navigator,
) : Screen<DefaultSettingsScreenKey>() {
   @Composable
   override fun Content(key: DefaultSettingsScreenKey) {
      val state = viewModel.uiState.collectAsStateWithLifecycle()

      ProgressErrorSuccessScaffold(
         state::value,
         errorProgressModifier = Modifier.safeDrawingPadding()
      ) {
         DefaultSettingsContent(
            preferences = it.preferences,
            globalPreferences = it.globalPreferences,
            navigator = navigator,
            updatePreference = SetPreference { prefKey, value ->
               @Suppress("UNCHECKED_CAST")
               viewModel.updatePreference(prefKey as PreferenceKeyWithDefault<Any?>, value)
            },
            updateGlobalPreference = SetPreference { prefKey, value ->
               @Suppress("UNCHECKED_CAST")
               viewModel.updateGlobalPreference(prefKey as PreferenceKeyWithDefault<Any?>, value)
            },
         )
      }
   }
}

@Composable
private fun DefaultSettingsContent(
   preferences: Preferences,
   globalPreferences: Preferences,
   navigator: Navigator,
   updatePreference: SetPreference,
   updateGlobalPreference: SetPreference,
) {
   val replyDialog = stringListDialog(
      navigator = navigator,
      title = stringResource(R.string.quick_replies),
      preference = RuleOption.replyCannedTexts,
      updatePreference = updatePreference,
   )
   val snoozeDialog = intListDialog(
      navigator = navigator,
      title = stringResource(R.string.snooze_intervals),
      preference = RuleOption.snoozeIntervals,
      updatePreference = updatePreference,
   )
   val vibrationDialog = navigator.rememberNavigationPopup(
      navigationKey = { pattern: String, resultKey: ResultKey<String> ->
         VibrationPatternScreenKey(pattern, resultKey)
      },
      onResult = {
         updatePreference(RuleOption.vibrationPattern, it)
      }
   )
   val taskerDialog = navigator.rememberNavigationPopup(
      navigationKey = { initialList: Set<String>, resultKey: ResultKey<Set<String>> ->
         TaskerTaskSetScreenKey("Tasker task actions", initialList, resultKey)
      },
      onResult = {
         updatePreference(RuleOption.taskerTaskActions, it)
      }
   )
   val regexDialog = regexReplacementDialog(navigator, updatePreference)

   LazyColumn(
      modifier = Modifier.fillMaxSize(),
      contentPadding = PaddingValues(
         top = WindowInsets.safeDrawing.asPaddingValues().calculateTopPadding(),
         bottom = WindowInsets.safeDrawing.asPaddingValues().calculateBottomPadding() + 12.dp,
      ),
   ) {
      item {
         Text(
            text = stringResource(R.string.notification_settings_title),
            style = MaterialTheme.typography.headlineLarge,
            modifier = Modifier
               .padding(horizontal = 16.dp)
               .padding(top = 12.dp, bottom = 8.dp)
               .semantics { heading() },
         )
      }

      item { SectionHeader(stringResource(R.string.notifications)) }
      item {
         ActionSetting(
            title = stringResource(R.string.quick_replies),
            description = stringResource(R.string.quick_replies_description),
            value = preferences[RuleOption.replyCannedTexts].joinToString(),
            onClick = { replyDialog.trigger(preferences[RuleOption.replyCannedTexts]) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.always_send_notifications),
            description = "New apps are shown on the watch by default.",
            checked = preferences[RuleOption.masterSwitch] == MasterSwitch.SHOW,
            onCheckedChange = { checked ->
               updatePreference(RuleOption.masterSwitch, if (checked) MasterSwitch.SHOW else MasterSwitch.MUTE)
            },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.respect_phone_dnd),
            description = stringResource(R.string.respect_phone_dnd_description),
            checked = preferences[RuleOption.muteDndNotifications],
            onCheckedChange = { updatePreference(RuleOption.muteDndNotifications, it) },
         )
      }
      item {
         ActionSetting(
            title = stringResource(R.string.setting_vibration_pattern),
            description = "Override the default on the watch.",
            value = presetNameFor(preferences[RuleOption.vibrationPattern]),
            onClick = { vibrationDialog.trigger(preferences[RuleOption.vibrationPattern]) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_use_notification_vibration_pattern),
            description = stringResource(R.string.setting_use_notification_vibration_pattern_description),
            checked = preferences[RuleOption.useNotificationVibrationPattern],
            onCheckedChange = { updatePreference(RuleOption.useNotificationVibrationPattern, it) },
         )
      }

      item { SectionHeader(stringResource(R.string.phone_filters)) }
      item {
         ToggleSetting(
            title = stringResource(R.string.send_local_only_notifications),
            description = stringResource(R.string.send_local_only_notifications_description),
            checked = !preferences[RuleOption.hideLocalOnlyNotifications],
            onCheckedChange = { updatePreference(RuleOption.hideLocalOnlyNotifications, !it) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_mute_silent_notifications),
            description = stringResource(R.string.setting_mute_silent_notifications_description),
            checked = preferences[RuleOption.muteSilentNotifications],
            onCheckedChange = { updatePreference(RuleOption.muteSilentNotifications, it) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_mute_identical_notifications),
            description = stringResource(R.string.setting_mute_identical_notifications_description),
            checked = preferences[RuleOption.muteIdenticalNotifications],
            onCheckedChange = { updatePreference(RuleOption.muteIdenticalNotifications, it) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_hide_ongoing_notifications),
            description = stringResource(R.string.setting_hide_ongoing_notifications_description),
            checked = preferences[RuleOption.hideOngoingNotifications],
            onCheckedChange = { updatePreference(RuleOption.hideOngoingNotifications, it) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_hide_group_summary_notifications),
            description = stringResource(R.string.setting_hide_group_summary_notifications_description),
            checked = preferences[RuleOption.hideGroupSummaryNotifications],
            onCheckedChange = { updatePreference(RuleOption.hideGroupSummaryNotifications, it) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_hide_media_notifications),
            description = stringResource(R.string.setting_hide_media_notifications_description),
            checked = preferences[RuleOption.hideMediaNotifications],
            onCheckedChange = { updatePreference(RuleOption.hideMediaNotifications, it) },
         )
      }

      item { SectionHeader(stringResource(R.string.watch_experience)) }
      item {
         NumberSetting(
            title = stringResource(R.string.notification_timeout),
            description = stringResource(R.string.notification_timeout_description),
            value = globalPreferences[GlobalPreferenceKeys.autoCloseSeconds],
            onValueChange = { updateGlobalPreference(GlobalPreferenceKeys.autoCloseSeconds, it) },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_defer_new_notifications),
            description = stringResource(R.string.setting_defer_new_notifications_description),
            checked = globalPreferences[GlobalPreferenceKeys.deferNewNotificationsWhileInteracting],
            onCheckedChange = {
               updateGlobalPreference(GlobalPreferenceKeys.deferNewNotificationsWhileInteracting, it)
            },
         )
      }
      item {
         NumberSetting(
            title = stringResource(R.string.setting_new_notification_interaction_timeout),
            description = stringResource(R.string.setting_new_notification_interaction_timeout_description),
            value = globalPreferences[GlobalPreferenceKeys.newNotificationInteractionTimeoutSeconds],
            onValueChange = {
               updateGlobalPreference(GlobalPreferenceKeys.newNotificationInteractionTimeoutSeconds, it)
            },
         )
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_skip_when_phone_unlocked),
            description = stringResource(R.string.setting_skip_when_phone_unlocked_description),
            checked = globalPreferences[GlobalPreferenceKeys.skipNotificationsWhenPhoneUnlocked],
            onCheckedChange = {
               updateGlobalPreference(GlobalPreferenceKeys.skipNotificationsWhenPhoneUnlocked, it)
            },
         )
      }
      item {
         if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            ActionSetting(
               title = stringResource(R.string.snooze_intervals),
               description = "Minutes offered by the Snooze action.",
               value = preferences[RuleOption.snoozeIntervals].joinToString(),
               onClick = { snoozeDialog.trigger(preferences[RuleOption.snoozeIntervals]) },
            )
         }
      }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_periodic_vibration),
            description = stringResource(R.string.setting_periodic_vibration_description),
            checked = preferences[RuleOption.periodicVibration],
            onCheckedChange = { updatePreference(RuleOption.periodicVibration, it) },
         )
      }
      item {
         ActionSetting(
            title = stringResource(R.string.setting_tasker_actions),
            description = stringResource(R.string.setting_tasker_actions_description),
            value = preferences[RuleOption.taskerTaskActions].joinToString().ifBlank { "None" },
            onClick = { taskerDialog.trigger(preferences[RuleOption.taskerTaskActions]) },
         )
      }

      item { SectionHeader(stringResource(R.string.advanced)) }
      item {
         ToggleSetting(
            title = stringResource(R.string.setting_stock_pebble_os_notifications),
            description = stringResource(R.string.setting_stock_pebble_os_notifications_description),
            checked = globalPreferences[GlobalPreferenceKeys.stockPebbleOsNotifications],
            onCheckedChange = {
               updateGlobalPreference(GlobalPreferenceKeys.stockPebbleOsNotifications, it)
            },
         )
      }
      item {
         ActionSetting(
            title = stringResource(R.string.preference_regex_replacement),
            description = stringResource(R.string.preference_regex_replacement_description),
            value = "${preferences[RuleOption.regexReplacements].size}",
            onClick = { regexDialog.trigger(preferences[RuleOption.regexReplacements]) },
         )
      }
      item {
         ActionSetting(
            title = "Advanced rule editor",
            description = "Open the original editor for fonts and lower-level notification transforms.",
            value = null,
            onClick = {
               navigator.navigate(OpenScreenOrReplaceExistingType(RuleDetailsScreenKey(RULE_ID_DEFAULT_SETTINGS)))
            },
         )
      }
   }
}

@Composable
private fun SectionHeader(title: String) {
   Text(
      text = title,
      color = MaterialTheme.colorScheme.primary,
      style = MaterialTheme.typography.titleSmall,
      fontWeight = FontWeight.SemiBold,
      modifier = Modifier
         .padding(horizontal = 16.dp)
         .padding(top = 18.dp, bottom = 6.dp)
         .semantics { heading() },
   )
}

@Composable
private fun ToggleSetting(
   title: String,
   description: String?,
   checked: Boolean,
   onCheckedChange: (Boolean) -> Unit,
) {
   ListItem(
      headlineContent = { Text(title) },
      supportingContent = {
         if (description != null) {
            Text(description, fontSize = 12.sp)
         }
      },
      leadingContent = {
         Checkbox(
            checked = checked,
            onCheckedChange = onCheckedChange,
         )
      },
      modifier = Modifier.clickable { onCheckedChange(!checked) },
      shadowElevation = 0.dp,
   )
   HorizontalDivider()
}

@Composable
private fun ActionSetting(
   title: String,
   description: String?,
   value: String?,
   onClick: () -> Unit,
) {
   ListItem(
      headlineContent = { Text(title) },
      supportingContent = {
         Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            if (description != null) {
               Text(description, fontSize = 12.sp)
            }
            if (!value.isNullOrBlank()) {
               Text(value, color = MaterialTheme.colorScheme.primary, fontSize = 12.sp)
            }
         }
      },
      trailingContent = {
         TextButton(onClick = onClick) {
            Text("Edit")
         }
      },
      modifier = Modifier.clickable(onClick = onClick),
      shadowElevation = 0.dp,
   )
   HorizontalDivider()
}

@Composable
private fun NumberSetting(
   title: String,
   description: String?,
   value: Int,
   onValueChange: (Int) -> Unit,
) {
   var dialogOpen by rememberSaveable { mutableStateOf(false) }
   var text by rememberSaveable(value) { mutableStateOf(value.toString()) }

   ActionSetting(
      title = title,
      description = description,
      value = formatTimeout(value),
      onClick = { dialogOpen = true },
   )

   if (dialogOpen) {
      AlertDialog(
         onDismissRequest = { dialogOpen = false },
         title = { Text(title) },
         text = {
            OutlinedTextField(
               value = text,
               onValueChange = { text = it.filter { character -> character.isDigit() } },
               singleLine = true,
               keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
         },
         confirmButton = {
            TextButton(
               onClick = {
                  onValueChange(text.toIntOrNull() ?: 0)
                  dialogOpen = false
               }
            ) {
               Text("OK")
            }
         },
         dismissButton = {
            TextButton(onClick = { dialogOpen = false }) {
               Text("Cancel")
            }
         },
      )
   }
}

@Composable
private fun stringListDialog(
   navigator: Navigator,
   title: String,
   preference: StringListPreferenceKeyWithDefault,
   updatePreference: SetPreference,
) = navigator.rememberNavigationPopup(
   navigationKey = { initialList: List<String>, resultKey: ResultKey<List<String>> ->
      StringListScreenKey(title, initialList, resultKey)
   },
   onResult = {
      updatePreference(preference, it)
   }
)

@Composable
private fun intListDialog(
   navigator: Navigator,
   title: String,
   preference: IntListPreferenceKeyWithDefault,
   updatePreference: SetPreference,
) = navigator.rememberNavigationPopup(
   navigationKey = { initialList: List<Int>, resultKey: ResultKey<List<Int>> ->
      IntListScreenKey(title, initialList, resultKey)
   },
   onResult = {
      updatePreference(preference, it)
   }
)

@Composable
private fun regexReplacementDialog(
   navigator: Navigator,
   updatePreference: SetPreference,
) = navigator.rememberNavigationPopup(
   navigationKey = { initialList: Set<Pair<String, String>>, resultKey: ResultKey<Set<Pair<String, String>>> ->
      RegexReplacementSetScreenKey(initialList, resultKey)
   },
   onResult = {
      updatePreference(RuleOption.regexReplacements, it)
   }
)

private fun presetNameFor(pattern: String): String {
   return coreVibrationPatterns.firstOrNull { it.pattern == pattern }?.name ?: pattern
}

private val coreVibrationPatterns: List<CoreVibrationPattern> = listOf(
   CoreVibrationPattern("Standard", "500"),
   CoreVibrationPattern("Pulses", "50, 50, 50, 50, 50, 50, 50"),
   CoreVibrationPattern("Double", "200, 75, 200"),
   CoreVibrationPattern("Triple", "200, 75, 200, 75, 200"),
   CoreVibrationPattern("Bloom", "35, 61, 47, 53, 50, 40, 81, 171, 189, 236, 47, 70, 38, 44, 39, 62, 79, 171, 181"),
   CoreVibrationPattern("Pips", "40, 960, 40, 960, 40, 960, 40, 960, 40, 960, 500"),
   CoreVibrationPattern("Ole", "61, 194, 272, 153, 47, 77, 47, 78, 46, 89, 54, 78, 47, 70, 388"),
   CoreVibrationPattern("SOS", "100, 75, 100, 75, 100, 220, 300, 75, 300, 75, 300, 150, 100, 75, 100, 75, 100"),
   CoreVibrationPattern("Ohhh, Oh", "459, 522, 144, 171, 173, 162, 72, 135, 555, 386, 514"),
   CoreVibrationPattern("Five", "68, 178, 80, 237, 54, 95, 122, 221, 154, 221, 139, 218, 81, 161, 137, 189, 55, 95, 130, 211, 188, 178, 222"),
   CoreVibrationPattern("Two", "135, 269, 847, 394, 40, 159, 48, 170, 31, 144, 64, 136, 64, 162, 36, 163, 122"),
)

private data class CoreVibrationPattern(
   val name: String,
   val pattern: String,
)

private fun formatTimeout(seconds: Int): String {
   if (seconds <= 0) {
      return "Off"
   }

   if (seconds % 60 == 0) {
      val minutes = seconds / 60
      return "$minutes minute${if (minutes == 1) "" else "s"}"
   }

   return "$seconds seconds"
}
