@file:Suppress("MagicNumber", "MaxLineLength", "MultipleEmitters")

package com.matejdro.pebblenotificationcenter.rules.ui.settings

import android.os.Build
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
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
            resetWatchMirror = viewModel::resetWatchMirror,
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
   resetWatchMirror: () -> Unit,
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

   val popUp = globalPreferences[GlobalPreferenceKeys.popUpOnWatch]
   val deferEnabled = globalPreferences[GlobalPreferenceKeys.deferNewNotificationsWhileInteracting]
   var advancedOpen by rememberSaveable { mutableStateOf(false) }

   LazyColumn(
      modifier = Modifier.fillMaxSize(),
      contentPadding = PaddingValues(
         start = 16.dp,
         end = 16.dp,
         top = WindowInsets.safeDrawing.asPaddingValues().calculateTopPadding() + 8.dp,
         bottom = WindowInsets.safeDrawing.asPaddingValues().calculateBottomPadding() + 24.dp,
      ),
      verticalArrangement = Arrangement.spacedBy(20.dp),
   ) {
      item {
         Text(
            text = stringResource(R.string.notification_settings_title),
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier
               .padding(horizontal = 4.dp)
               .padding(top = 8.dp)
               .semantics { heading() },
         )
      }

      item {
         SettingsGroup("Alerts", "How the watch tells you about new notifications") {
            SwitchRow(
               title = "Pop up on the watch",
               description = "Buzz and open new notifications on the watch. Turn off to keep a quiet mirror only, " +
                  "for example when the Pebble app's own alerts are on, so you aren't alerted twice.",
               checked = popUp,
               onCheckedChange = { updateGlobalPreference(GlobalPreferenceKeys.popUpOnWatch, it) },
            )
            Nested(enabled = popUp) {
               ValueRow(
                  title = "Vibration",
                  description = "How the watch buzzes, so you can tell notifications from calls and alarms.",
                  value = presetNameFor(preferences[RuleOption.vibrationPattern]),
                  enabled = popUp,
                  onClick = { vibrationDialog.trigger(preferences[RuleOption.vibrationPattern]) },
               )
               SwitchRow(
                  title = "Use the app's vibration",
                  description = "When an app sets its own vibration, use it, so its alerts feel the same as on the phone.",
                  checked = preferences[RuleOption.useNotificationVibrationPattern],
                  enabled = popUp,
                  onCheckedChange = { updatePreference(RuleOption.useNotificationVibrationPattern, it) },
               )
               SwitchRow(
                  title = "Repeat buzz",
                  description = "Keep buzzing now and then until you look, so an important notification isn't missed.",
                  checked = preferences[RuleOption.periodicVibration],
                  enabled = popUp,
                  onCheckedChange = { updatePreference(RuleOption.periodicVibration, it) },
               )
               SwitchRow(
                  title = "Wait until watchface",
                  description = "Hold new notifications until you leave the app you're using on the watch, to avoid " +
                     "interrupting your watch app.",
                  checked = globalPreferences[GlobalPreferenceKeys.waitForWatchfaceBeforeOpening],
                  enabled = popUp,
                  onCheckedChange = { updateGlobalPreference(GlobalPreferenceKeys.waitForWatchfaceBeforeOpening, it) },
               )
               SwitchRow(
                  title = "Don't interrupt reading",
                  description = "While you're pressing buttons, new notifications join the list instead of replacing " +
                     "the one you're reading, so you don't lose your place.",
                  checked = deferEnabled,
                  enabled = popUp,
                  onCheckedChange = {
                     updateGlobalPreference(GlobalPreferenceKeys.deferNewNotificationsWhileInteracting, it)
                  },
               )
               if (deferEnabled) {
                  Nested(enabled = popUp) {
                     NumberRow(
                        title = "Reading time",
                        description = "How long after your last button press you still count as reading.",
                        value = globalPreferences[GlobalPreferenceKeys.newNotificationInteractionTimeoutSeconds],
                        enabled = popUp,
                        onValueChange = {
                           updateGlobalPreference(GlobalPreferenceKeys.newNotificationInteractionTimeoutSeconds, it)
                        },
                     )
                  }
               }
               SwitchRow(
                  title = "Quiet while phone is in use",
                  description = "Mirror without buzzing while your phone is unlocked, since you're already looking at it.",
                  checked = globalPreferences[GlobalPreferenceKeys.skipNotificationsWhenPhoneUnlocked],
                  enabled = popUp,
                  onCheckedChange = {
                     updateGlobalPreference(GlobalPreferenceKeys.skipNotificationsWhenPhoneUnlocked, it)
                  },
               )
            }
            GroupDivider()
            SwitchRow(
               title = "Respect Do Not Disturb",
               description = "Stay quiet whenever the phone's Do Not Disturb would, so the watch doesn't wake you.",
               checked = preferences[RuleOption.muteDndNotifications],
               onCheckedChange = { updatePreference(RuleOption.muteDndNotifications, it) },
            )
            SwitchRow(
               title = "Silent stays silent",
               description = "Don't buzz for notifications that are silent on the phone, so the watch matches it.",
               checked = preferences[RuleOption.muteSilentNotifications],
               onCheckedChange = { updatePreference(RuleOption.muteSilentNotifications, it) },
            )
            SwitchRow(
               title = "Ignore repeats",
               description = "Don't buzz again when an app re-posts the same text, to avoid double buzzes.",
               checked = preferences[RuleOption.muteIdenticalNotifications],
               onCheckedChange = { updatePreference(RuleOption.muteIdenticalNotifications, it) },
            )
         }
      }

      item {
         SettingsGroup("Look", "How notifications appear on the watch") {
            TextSizeRow(
               selected = globalPreferences[GlobalPreferenceKeys.watchTextSize],
               onSelect = { updateGlobalPreference(GlobalPreferenceKeys.watchTextSize, it) },
            )
            WeightRow(
               title = "Sender",
               bold = globalPreferences[GlobalPreferenceKeys.watchSenderBold],
               onSelect = { updateGlobalPreference(GlobalPreferenceKeys.watchSenderBold, it) },
            )
            WeightRow(
               title = "Message",
               bold = globalPreferences[GlobalPreferenceKeys.watchMessageBold],
               onSelect = { updateGlobalPreference(GlobalPreferenceKeys.watchMessageBold, it) },
            )
            GroupDivider()
            SwitchRow(
               title = "Dark mode",
               description = "White text on black, to match your watch if you use PebbleOS dark mode.",
               checked = globalPreferences[GlobalPreferenceKeys.watchDarkMode],
               onCheckedChange = { updateGlobalPreference(GlobalPreferenceKeys.watchDarkMode, it) },
            )
            NumberRow(
               title = "Auto-close",
               description = "Close a popped-up notification after this long, so the watch goes back to your watchface.",
               value = globalPreferences[GlobalPreferenceKeys.autoCloseSeconds],
               onValueChange = { updateGlobalPreference(GlobalPreferenceKeys.autoCloseSeconds, it) },
            )
         }
      }

      item {
         val grouped = globalPreferences[GlobalPreferenceKeys.watchGroupSections]
         SettingsGroup("Organize", "How the list on the watch is laid out") {
            SwitchRow(
               title = "Group like the phone",
               description = "Sections with headers you can open and close, so quiet clutter stays out of the way. " +
                  "Silent starts closed.",
               checked = grouped,
               onCheckedChange = { updateGlobalPreference(GlobalPreferenceKeys.watchGroupSections, it) },
            )
            SwitchRow(
               title = "Stack each app",
               description = "Fold several notifications from one app into one row, so a busy app doesn't fill the list.",
               checked = globalPreferences[GlobalPreferenceKeys.watchStackApps],
               onCheckedChange = { updateGlobalPreference(GlobalPreferenceKeys.watchStackApps, it) },
            )
            GroupDivider()
            CategoryRow(globalPreferences, updateGlobalPreference)
         }
      }

      item {
         SettingsGroup("What's mirrored", "Which phone notifications reach the watch") {
            DefaultStateRow(
               selected = preferences[RuleOption.masterSwitch],
               onSelect = { updatePreference(RuleOption.masterSwitch, it) },
            )
            GroupDivider()
            SwitchRow(
               title = "Hide ongoing",
               description = "Leave out timers, downloads and navigation. They update constantly and would keep the " +
                  "watch busy.",
               checked = preferences[RuleOption.hideOngoingNotifications],
               onCheckedChange = { updatePreference(RuleOption.hideOngoingNotifications, it) },
            )
            SwitchRow(
               title = "Hide group summaries",
               description = "Leave out the \"5 new messages\" summary above a group, since the messages themselves " +
                  "are already there.",
               checked = preferences[RuleOption.hideGroupSummaryNotifications],
               onCheckedChange = { updatePreference(RuleOption.hideGroupSummaryNotifications, it) },
            )
            SwitchRow(
               title = "Hide media",
               description = "Leave out music and video controls. Your watch's music app handles those better.",
               checked = preferences[RuleOption.hideMediaNotifications],
               onCheckedChange = { updatePreference(RuleOption.hideMediaNotifications, it) },
            )
            SwitchRow(
               title = "Include phone-only",
               description = "Some notifications are meant for this phone only, like charging status. Turn on to " +
                  "see them on the watch anyway.",
               checked = !preferences[RuleOption.hideLocalOnlyNotifications],
               onCheckedChange = { updatePreference(RuleOption.hideLocalOnlyNotifications, !it) },
            )
         }
      }

      item {
         SettingsGroup("Actions", "What you can do from the watch") {
            ValueRow(
               title = "Quick replies",
               description = "Ready-made replies, so you don't have to dictate.",
               value = summarizeList(preferences[RuleOption.replyCannedTexts]),
               onClick = { replyDialog.trigger(preferences[RuleOption.replyCannedTexts]) },
            )
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
               ValueRow(
                  title = "Snooze times",
                  description = "Choices offered by Snooze, for a reminder later.",
                  value = summarizeList(preferences[RuleOption.snoozeIntervals].map { "$it min" }),
                  onClick = { snoozeDialog.trigger(preferences[RuleOption.snoozeIntervals]) },
               )
            }
            ValueRow(
               title = "Tasker tasks",
               description = "Run a Tasker task from a notification's actions, to automate things from your wrist.",
               value = summarizeList(preferences[RuleOption.taskerTaskActions].toList()),
               onClick = { taskerDialog.trigger(preferences[RuleOption.taskerTaskActions]) },
            )
         }
      }

      item {
         SettingsGroup(
            title = "Advanced",
            subtitle = "Rarely needed",
            expanded = advancedOpen,
            onToggle = { advancedOpen = !advancedOpen },
         ) {
            ValueRow(
               title = "Text replacements",
               description = "Rewrite parts of notification text, to trim clutter or bring out what matters.",
               value = preferences[RuleOption.regexReplacements].size.let { if (it == 0) "None" else "$it" },
               onClick = { regexDialog.trigger(preferences[RuleOption.regexReplacements]) },
            )
            SwitchRow(
               title = "Stock PebbleOS notifications",
               description = "Experimental. Send alerts to the watch's built-in notification list instead of this " +
                  "app, for watches without the app installed.",
               checked = globalPreferences[GlobalPreferenceKeys.stockPebbleOsNotifications],
               onCheckedChange = { updateGlobalPreference(GlobalPreferenceKeys.stockPebbleOsNotifications, it) },
            )
            ValueRow(
               title = "Rule editor",
               description = "Every option for the default rule, including fonts and lower-level transforms.",
               value = null,
               onClick = {
                  navigator.navigate(OpenScreenOrReplaceExistingType(RuleDetailsScreenKey(RULE_ID_DEFAULT_SETTINGS)))
               },
            )
            ListItem(
               headlineContent = { Text("Reset watch mirror") },
               supportingContent = {
                  Text("Clear the watch's copy and rebuild it from your phone, if the watch ever looks out of date.")
               },
               trailingContent = {
                  OutlinedButton(onClick = resetWatchMirror) {
                     Text(stringResource(R.string.reset))
                  }
               },
               colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            )
         }
      }
   }
}

/** Settings that only matter when the setting above is on: indented, with a guide line, dimmed when it's off. */
@Composable
private fun Nested(enabled: Boolean, content: @Composable ColumnScope.() -> Unit) {
   Row(
      modifier = Modifier
         .padding(start = 24.dp)
         .height(IntrinsicSize.Min)
         .alpha(if (enabled) 1f else DISABLED_ALPHA),
   ) {
      Box(
         Modifier
            .fillMaxHeight()
            .padding(vertical = 8.dp)
            .width(2.dp)
            .background(MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(1.dp))
      )
      Column(content = content)
   }
}

private const val DISABLED_ALPHA = 0.45f

@Composable
private fun SettingsGroup(
   title: String,
   subtitle: String? = null,
   expanded: Boolean = true,
   onToggle: (() -> Unit)? = null,
   content: @Composable ColumnScope.() -> Unit,
) {
   Card(
      shape = MaterialTheme.shapes.extraLarge,
      colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
   ) {
      Row(
         verticalAlignment = Alignment.CenterVertically,
         modifier = Modifier
            .fillMaxWidth()
            .then(if (onToggle != null) Modifier.clickable(onClick = onToggle) else Modifier)
            .padding(start = 20.dp, end = 16.dp, top = 18.dp, bottom = if (expanded) 6.dp else 18.dp),
      ) {
         Column(Modifier.weight(1f)) {
            Text(
               text = title,
               color = MaterialTheme.colorScheme.primary,
               style = MaterialTheme.typography.titleMedium,
               fontWeight = FontWeight.SemiBold,
               modifier = Modifier.semantics { heading() },
            )
            if (subtitle != null) {
               Text(
                  text = subtitle,
                  style = MaterialTheme.typography.bodySmall,
                  color = MaterialTheme.colorScheme.onSurfaceVariant,
               )
            }
         }
         if (onToggle != null) {
            val rotation by animateFloatAsState(if (expanded) 180f else 0f, label = "chevron")
            val chevronColor = MaterialTheme.colorScheme.onSurfaceVariant
            Canvas(
               Modifier
                  .size(24.dp)
                  .rotate(rotation)
            ) {
               val path = Path().apply {
                  moveTo(size.width * 0.25f, size.height * 0.38f)
                  lineTo(size.width * 0.5f, size.height * 0.63f)
                  lineTo(size.width * 0.75f, size.height * 0.38f)
               }
               drawPath(path, chevronColor, style = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round))
            }
         }
      }
      AnimatedVisibility(visible = expanded) {
         Column(Modifier.padding(bottom = 8.dp), content = content)
      }
   }
}

@Composable
private fun GroupDivider() {
   HorizontalDivider(
      modifier = Modifier.padding(horizontal = 20.dp, vertical = 4.dp),
      color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f),
   )
}

@Composable
private fun SwitchRow(
   title: String,
   description: String?,
   checked: Boolean,
   enabled: Boolean = true,
   onCheckedChange: (Boolean) -> Unit,
) {
   ListItem(
      headlineContent = { Text(title) },
      supportingContent = description?.let { { Text(it) } },
      trailingContent = {
         Switch(checked = checked, onCheckedChange = onCheckedChange, enabled = enabled)
      },
      colors = ListItemDefaults.colors(containerColor = Color.Transparent),
      modifier = Modifier.clickable(enabled = enabled) { onCheckedChange(!checked) },
   )
}

/** Android 16 categories as one row of chips instead of four switches. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun CategoryRow(globalPreferences: Preferences, updateGlobalPreference: SetPreference) {
   val categories = listOf(
      "Suggested" to GlobalPreferenceKeys.watchShowSuggested,
      "Promotions" to GlobalPreferenceKeys.watchShowPromotions,
      "News" to GlobalPreferenceKeys.watchShowNews,
      "Social" to GlobalPreferenceKeys.watchShowSocial,
   )
   ListItem(
      headlineContent = { Text("Categories") },
      supportingContent = {
         Column {
            Text(
               "Android 16 sorts some quiet notifications into these. Untick one to keep that kind off the " +
                  "watch entirely."
            )
            FlowRow(
               horizontalArrangement = Arrangement.spacedBy(8.dp),
               modifier = Modifier.padding(top = 8.dp),
            ) {
               categories.forEach { (label, key) ->
                  val shown = globalPreferences[key]
                  FilterChip(
                     selected = shown,
                     onClick = { updateGlobalPreference(key, !shown) },
                     label = { Text(label) },
                  )
               }
            }
         }
      },
      colors = ListItemDefaults.colors(containerColor = Color.Transparent),
   )
}

@Composable
private fun DefaultStateRow(
   selected: MasterSwitch,
   onSelect: (MasterSwitch) -> Unit,
) {
   val description = when (selected) {
      MasterSwitch.SHOW -> "New apps buzz and show on the watch."
      MasterSwitch.MUTE -> "New apps show on the watch without buzzing."
      MasterSwitch.HIDE -> "New apps stay off the watch entirely."
   }
   ListItem(
      headlineContent = { Text(stringResource(R.string.always_send_notifications)) },
      supportingContent = {
         Column {
            Text("$description You can still change each app on the Apps screen.")
            Row(
               horizontalArrangement = Arrangement.spacedBy(8.dp),
               modifier = Modifier.padding(top = 8.dp),
            ) {
               listOf(
                  MasterSwitch.SHOW to R.string.shown,
                  MasterSwitch.MUTE to R.string.silenced,
                  MasterSwitch.HIDE to R.string.hidden,
               ).forEach { (value, label) ->
                  FilterChip(
                     selected = selected == value,
                     onClick = { onSelect(value) },
                     label = { Text(stringResource(label)) },
                  )
               }
            }
         }
      },
      colors = ListItemDefaults.colors(containerColor = Color.Transparent),
   )
}

@Composable
private fun TextSizeRow(
   selected: Int,
   onSelect: (Int) -> Unit,
) {
   val options = listOf("Small" to MaterialTheme.typography.labelMedium, "Default" to MaterialTheme.typography.labelLarge, "Large" to MaterialTheme.typography.titleMedium)
   ListItem(
      headlineContent = { Text("Text size") },
      supportingContent = {
         Column {
            Text("How big notification text is on the watch.")
            Row(
               horizontalArrangement = Arrangement.spacedBy(8.dp),
               modifier = Modifier.padding(top = 8.dp),
            ) {
               options.forEachIndexed { index, (label, style) ->
                  FilterChip(
                     selected = selected == index,
                     onClick = { onSelect(index) },
                     label = { Text(label, style = style) },
                  )
               }
            }
         }
      },
      colors = ListItemDefaults.colors(containerColor = Color.Transparent),
   )
}

@Composable
private fun WeightRow(
   title: String,
   bold: Boolean,
   onSelect: (Boolean) -> Unit,
) {
   ListItem(
      headlineContent = { Text(title) },
      trailingContent = {
         Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(
               selected = !bold,
               onClick = { onSelect(false) },
               label = { Text("Regular") },
            )
            FilterChip(
               selected = bold,
               onClick = { onSelect(true) },
               label = { Text("Bold", fontWeight = FontWeight.Bold) },
            )
         }
      },
      colors = ListItemDefaults.colors(containerColor = Color.Transparent),
   )
}

@Composable
private fun ValueRow(
   title: String,
   description: String?,
   value: String?,
   enabled: Boolean = true,
   onClick: () -> Unit,
) {
   ListItem(
      headlineContent = { Text(title) },
      supportingContent = description?.let { { Text(it) } },
      trailingContent = {
         Text(
            text = if (value.isNullOrBlank()) "›" else "$value  ›",
            color = MaterialTheme.colorScheme.primary,
            style = MaterialTheme.typography.labelLarge,
            maxLines = 1,
            modifier = Modifier.widthIn(max = 140.dp),
            overflow = TextOverflow.Ellipsis,
         )
      },
      colors = ListItemDefaults.colors(containerColor = Color.Transparent),
      modifier = Modifier.clickable(enabled = enabled, onClick = onClick),
   )
}

private fun summarizeList(values: List<String>): String {
   return when {
      values.isEmpty() -> "None"
      values.size <= 2 -> values.joinToString()
      else -> "${values.size} set"
   }
}

@Composable
private fun NumberRow(
   title: String,
   description: String?,
   value: Int,
   enabled: Boolean = true,
   onValueChange: (Int) -> Unit,
) {
   var dialogOpen by rememberSaveable { mutableStateOf(false) }
   var text by rememberSaveable(value) { mutableStateOf(value.toString()) }

   ValueRow(
      title = title,
      description = description,
      value = formatTimeout(value),
      enabled = enabled,
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
               label = { Text("Seconds (0 = off)") },
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
