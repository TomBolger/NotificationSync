package com.matejdro.pebblenotificationcenter.tools.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.datastore.preferences.core.emptyPreferences
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.airbnb.android.showkase.annotation.ShowkaseComposable
import com.matejdro.pebblenotificationcenter.navigation.keys.OnboardingKey
import com.matejdro.pebblenotificationcenter.navigation.keys.ToolsScreenKey
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.keys.PreferenceKeyWithDefault
import com.matejdro.pebblenotificationcenter.rules.keys.get
import com.matejdro.pebblenotificationcenter.tools.ui.debug.TestAppLook
import com.matejdro.pebblenotificationcenter.tools.ui.debug.TestNotifications
import com.matejdro.pebblenotificationcenter.tools.ui.debug.TestType
import com.matejdro.pebblenotificationcenter.tools.ui.dialogs.ActionOrderListScreenKey
import com.matejdro.pebblenotificationcenter.ui.components.ErrorAlertDialog
import com.matejdro.pebblenotificationcenter.ui.components.ProgressErrorSuccessScaffold
import com.matejdro.pebblenotificationcenter.ui.debugging.FullScreenPreviews
import com.matejdro.pebblenotificationcenter.ui.debugging.PreviewTheme
import si.inova.kotlinova.compose.flow.collectAsStateWithLifecycleAndBlinkingPrevention
import si.inova.kotlinova.core.outcome.Outcome
import si.inova.kotlinova.navigation.di.ContributesScreenBinding
import si.inova.kotlinova.navigation.instructions.navigateTo
import si.inova.kotlinova.navigation.navigator.Navigator
import si.inova.kotlinova.navigation.screens.InjectNavigationScreen
import si.inova.kotlinova.navigation.screens.Screen
import com.matejdro.pebblenotificationcenter.sharedresources.R as sharedR

@InjectNavigationScreen
@ContributesScreenBinding
class ToolsScreen(
   private val navigator: Navigator,
   private val viewModel: ToolsViewModel,
) : Screen<ToolsScreenKey>() {
   @Composable
   override fun Content(key: ToolsScreenKey) {
      val stateOutcome = viewModel.appVersion.collectAsStateWithLifecycle()
      val logSaveStatus = viewModel.logSave.collectAsStateWithLifecycleAndBlinkingPrevention().value

      ProgressErrorSuccessScaffold(
         stateOutcome::value,
         Modifier
            .fillMaxSize()
            .safeDrawingPadding()
      ) { state ->
         ToolsScreenContent(
            state = state,
            loggingTransmissionState = logSaveStatus,
            openPermissions = { navigator.navigateTo(OnboardingKey) },
            startLogSaving = viewModel::getLogs,
            notifyLogIntentSent = viewModel::resetLog,
            openActionOrderDialog = {
               navigator.navigateTo(ActionOrderListScreenKey)
            },
            updatePreference = { prefKey, value ->
               @Suppress("UNCHECKED_CAST")
               viewModel.updatePreference(prefKey as PreferenceKeyWithDefault<Any?>, value)
            }
         )
      }
   }
}

@Composable
private fun ToolsScreenContent(
   state: ToolsState,
   loggingTransmissionState: Outcome<Uri?>?,
   openPermissions: () -> Unit,
   startLogSaving: () -> Unit,
   notifyLogIntentSent: () -> Unit,
   openActionOrderDialog: () -> Unit,
   updatePreference: (PreferenceKeyWithDefault<*>, Any?) -> Unit,
) {
   val context = LocalContext.current
   LazyColumn(
      modifier = Modifier.fillMaxSize(),
      contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 24.dp),
      verticalArrangement = Arrangement.spacedBy(20.dp),
   ) {
      item {
         Text(
            text = stringResource(R.string.tools_title),
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier
               .padding(horizontal = 4.dp)
               .semantics { heading() },
         )
      }

      item {
         ToolsCard("Quick switches", "Temporary, for when you need quiet") {
            ToolSwitch(
               title = stringResource(sharedR.string.setting_mute_watch),
               description = stringResource(sharedR.string.setting_mute_watch_description),
               checked = state.preferences[GlobalPreferenceKeys.muteWatch],
               onCheckedChange = { updatePreference(GlobalPreferenceKeys.muteWatch, it) },
            )
            ToolSwitch(
               title = stringResource(sharedR.string.setting_mute_phone),
               description = stringResource(sharedR.string.setting_mute_phone_description),
               checked = state.preferences[GlobalPreferenceKeys.mutePhone],
               onCheckedChange = { updatePreference(GlobalPreferenceKeys.mutePhone, it) },
            )
         }
      }

      item {
         ToolsCard("More options", null) {
            ToolSwitch(
               title = stringResource(R.string.setting_notify_on_reconnect),
               description = stringResource(R.string.setting_notify_on_reconnect_description),
               checked = state.preferences[GlobalPreferenceKeys.notifyOnReconnect],
               onCheckedChange = { updatePreference(GlobalPreferenceKeys.notifyOnReconnect, it) },
            )
            ToolSwitch(
               title = stringResource(R.string.setting_messaging_style_chronological),
               description = stringResource(R.string.setting_messaging_style_chronological_description),
               checked = state.preferences[GlobalPreferenceKeys.showMessagingStyleChronologically],
               onCheckedChange = { updatePreference(GlobalPreferenceKeys.showMessagingStyleChronologically, it) },
            )
            ListItem(
               headlineContent = { Text(stringResource(R.string.setting_action_order)) },
               supportingContent = { Text(stringResource(R.string.setting_action_order_description)) },
               trailingContent = { Text("›", style = MaterialTheme.typography.titleLarge) },
               colors = ListItemDefaults.colors(containerColor = Color.Transparent),
               modifier = Modifier.clickable(onClick = openActionOrderDialog),
            )
         }
      }

      item {
         TestNotificationsCard()
      }

      item {
         ToolsCard("Help", "Permissions and problem reports") {
            Row(
               horizontalArrangement = Arrangement.spacedBy(12.dp),
               modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            ) {
               ToolButton(onClick = openPermissions, icon = R.drawable.permissions, text = R.string.permissions)

               ErrorAlertDialog(loggingTransmissionState)
               if (loggingTransmissionState is Outcome.Progress<*>) {
                  CircularProgressIndicator(Modifier.size(40.dp))
               } else {
                  ToolButton(onClick = startLogSaving, icon = R.drawable.logs, text = R.string.save_logs)
                  SideEffect {
                     loggingTransmissionState?.data?.let { targetUri ->
                        val activityIntent = Intent(Intent.ACTION_SEND)
                        activityIntent.putExtra(Intent.EXTRA_STREAM, targetUri)
                        activityIntent.setType("application/zip")

                        activityIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        activityIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)

                        context.startActivity(Intent.createChooser(activityIntent, null))
                        notifyLogIntentSent()
                     }
                  }
               }
            }
            Text(
               stringResource(R.string.save_logs_hint),
               style = MaterialTheme.typography.bodySmall,
               color = MaterialTheme.colorScheme.onSurfaceVariant,
               modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
         }
      }

      item {
         ToolsCard("About", null) {
            Text(
               stringResource(R.string.version, state.versionName),
               style = MaterialTheme.typography.bodyLarge,
               modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
            Text(
               stringResource(R.string.about_credits),
               style = MaterialTheme.typography.bodySmall,
               color = MaterialTheme.colorScheme.onSurfaceVariant,
               modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
            Row(Modifier.padding(horizontal = 8.dp)) {
               TextButton(onClick = { openUrl(context, PRIVACY_URL) }) { Text("Privacy policy") }
               TextButton(onClick = { openUrl(context, SOURCE_URL) }) { Text("Source code") }
            }
         }
      }
   }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TestNotificationsCard() {
   val context = LocalContext.current
   var type by rememberSaveable { mutableStateOf(TestType.CHAT) }
   var look by rememberSaveable { mutableStateOf(TestAppLook.WHATSAPP) }
   var delay by rememberSaveable { mutableIntStateOf(DELAYS[1]) }
   var allowed by remember { mutableStateOf(TestNotifications.canPost(context)) }
   var status by remember { mutableStateOf<String?>(null) }

   LifecycleResumeEffect(Unit) {
      allowed = TestNotifications.canPost(context)
      onPauseOrDispose { }
   }

   ToolsCard("Test notifications", "Send pretend notifications to this phone to try the watch app") {
      Text(
         "They're posted on the phone like any other app's, so the watch gets them the normal way. Pick a delay, " +
            "then leave the app and lock the phone. Buttons on them really work and are written to Save logs.",
         style = MaterialTheme.typography.bodyMedium,
         color = MaterialTheme.colorScheme.onSurfaceVariant,
         modifier = Modifier.padding(horizontal = 16.dp),
      )

      ChipGroup("Kind") {
         TestType.entries.forEach {
            FilterChip(selected = type == it, onClick = { type = it }, label = { Text(it.label) })
         }
      }
      ChipGroup("Looks like") {
         TestAppLook.entries.forEach {
            FilterChip(selected = look == it, onClick = { look = it }, label = { Text(it.label) })
         }
      }
      ChipGroup("Arrives") {
         DELAYS.forEach {
            FilterChip(
               selected = delay == it,
               onClick = { delay = it },
               label = { Text(if (it == 0) "Now" else "In $it s") },
            )
         }
      }

      if (!allowed) {
         Text(
            "Notification Sync isn't allowed to show notifications yet.",
            color = MaterialTheme.colorScheme.error,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
         )
         Button(onClick = { openNotificationSettings(context) }, modifier = Modifier.padding(horizontal = 16.dp)) {
            Text("Allow notifications")
         }
      } else {
         FlowRow(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
         ) {
            Button(
               onClick = {
                  TestNotifications.send(context, type, look, delay)
                  status = if (delay == 0) "Sent." else "Arrives in $delay seconds. Lock your phone now."
               }
            ) { Text("Send") }
            OutlinedButton(
               onClick = {
                  TestNotifications.sendAll(context, look, delay)
                  status = "Sending one of each, a couple of seconds apart" +
                     if (delay == 0) "." else ", starting in $delay seconds."
               }
            ) { Text("Send one of each") }
            TextButton(
               onClick = {
                  TestNotifications.clearAll(context)
                  status = "Cleared."
               }
            ) { Text("Clear") }
         }
         status?.let {
            Text(
               it,
               style = MaterialTheme.typography.bodySmall,
               color = MaterialTheme.colorScheme.primary,
               modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
         }
      }
   }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ChipGroup(label: String, content: @Composable () -> Unit) {
   Column(Modifier.padding(horizontal = 16.dp, vertical = 6.dp)) {
      Text(label, style = MaterialTheme.typography.labelLarge, modifier = Modifier.padding(bottom = 4.dp))
      FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) { content() }
   }
}

@Composable
private fun ToolsCard(title: String, subtitle: String?, content: @Composable ColumnScope.() -> Unit) {
   Card(
      shape = MaterialTheme.shapes.extraLarge,
      colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
   ) {
      Column(Modifier.padding(start = 20.dp, end = 16.dp, top = 18.dp, bottom = 6.dp)) {
         Text(
            title,
            color = MaterialTheme.colorScheme.primary,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.semantics { heading() },
         )
         if (subtitle != null) {
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
         }
      }
      Column(Modifier.padding(bottom = 12.dp), content = content)
   }
}

@Composable
private fun ToolSwitch(title: String, description: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
   ListItem(
      headlineContent = { Text(title) },
      supportingContent = { Text(description) },
      trailingContent = { Switch(checked = checked, onCheckedChange = onCheckedChange) },
      colors = ListItemDefaults.colors(containerColor = Color.Transparent),
      modifier = Modifier.clickable { onCheckedChange(!checked) },
   )
}

@Composable
private fun ToolButton(onClick: () -> Unit, icon: Int, text: Int) {
   FilledTonalButton(onClick = onClick, Modifier.sizeIn(minHeight = 48.dp)) {
      Row(verticalAlignment = Alignment.CenterVertically) {
         Icon(
            painterResource(icon),
            contentDescription = null,
            modifier = Modifier.padding(end = 8.dp)
         )

         Text(stringResource(text))
      }
   }
}

private fun openUrl(context: Context, url: String) {
   runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
}

private fun openNotificationSettings(context: Context) {
   val intent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
   } else {
      Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).setData(Uri.fromParts("package", context.packageName, null))
   }
   runCatching { context.startActivity(intent) }
}

private val DELAYS = listOf(0, 5, 10, 30)
private const val PRIVACY_URL = "https://github.com/TomBolger/NotificationSync/blob/main/PRIVACY.md"
private const val SOURCE_URL = "https://github.com/TomBolger/NotificationSync"

@FullScreenPreviews
@Composable
@ShowkaseComposable(group = "test")
internal fun ToolsScreenPreview() {
   PreviewTheme {
      ToolsScreenContent(
         state = ToolsState("1.0.0-alpha07", emptyPreferences()),
         loggingTransmissionState = Outcome.Success(null),
         openPermissions = {},
         startLogSaving = {},
         notifyLogIntentSent = {},
         openActionOrderDialog = {},
         updatePreference = { _, _ -> },
      )
   }
}
