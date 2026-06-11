@file:Suppress("MultipleEmitters")

package com.matejdro.pebblenotificationcenter.rules.ui.list

import android.graphics.drawable.Drawable
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Badge
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.google.accompanist.drawablepainter.rememberDrawablePainter
import com.matejdro.pebblenotificationcenter.navigation.keys.RuleListScreenKey
import com.matejdro.pebblenotificationcenter.rules.MasterSwitch
import com.matejdro.pebblenotificationcenter.rules.ui.R
import com.matejdro.pebblenotificationcenter.ui.components.ProgressErrorSuccessScaffold
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import si.inova.kotlinova.compose.flow.collectAsStateWithLifecycleAndBlinkingPrevention
import si.inova.kotlinova.core.outcome.Outcome
import si.inova.kotlinova.core.time.TimeProvider
import si.inova.kotlinova.navigation.screens.InjectNavigationScreen
import si.inova.kotlinova.navigation.screens.Screen
import java.time.format.FormatStyle

@InjectNavigationScreen
class RuleListScreen(
   private val viewModel: RuleListViewModel,
   private val timeProvider: TimeProvider,
) : Screen<RuleListScreenKey>() {
   @Composable
   override fun Content(key: RuleListScreenKey) {
      val stateOutcome = viewModel.uiState.collectAsStateWithLifecycleAndBlinkingPrevention()
      val appDetailsOutcome by viewModel.appDetailsState.collectAsStateWithLifecycle()

      ProgressErrorSuccessScaffold(
         stateOutcome::value,
         errorProgressModifier = Modifier.safeDrawingPadding()
      ) { state ->
         RuleListScreenContent(
            state = state,
            appDetailsOutcome = appDetailsOutcome,
            timeProvider = timeProvider,
            setAllEnabled = viewModel::setAllEnabled,
            setAppEnabled = viewModel::setAppEnabled,
            openAppDetails = viewModel::openAppDetails,
            closeAppDetails = viewModel::closeAppDetails,
            setAppMasterSwitch = viewModel::setAppMasterSwitch,
            setChannelMasterSwitch = viewModel::setChannelMasterSwitch,
         )
      }
   }
}

@Composable
private fun RuleListScreenContent(
   state: RuleListState,
   appDetailsOutcome: Outcome<NotificationAppDetailsState>?,
   timeProvider: TimeProvider,
   setAllEnabled: (Boolean) -> Unit,
   setAppEnabled: (NotificationAppState, Boolean) -> Unit,
   openAppDetails: (NotificationAppState) -> Unit,
   closeAppDetails: () -> Unit,
   setAppMasterSwitch: (MasterSwitch) -> Unit,
   setChannelMasterSwitch: (NotificationChannelRuleState, MasterSwitch) -> Unit,
) {
   if (appDetailsOutcome != null) {
      NotificationTypeListScreen(
         outcome = appDetailsOutcome,
         onBack = closeAppDetails,
         setAppMasterSwitch = setAppMasterSwitch,
         setChannelMasterSwitch = setChannelMasterSwitch,
      )
      return
   }

   var notifiedOnly by remember { mutableStateOf(true) }
   var filter by rememberSaveable { mutableStateOf(NotificationFilter.All) }
   var sort by rememberSaveable { mutableStateOf(NotificationSort.Recent) }
   val visibleApps = remember(state.apps, notifiedOnly, filter, sort) {
      val notifiedApps = if (notifiedOnly) {
         state.apps.filter { it.notificationCount > 0 || it.ruleId != null }
      } else {
         state.apps
      }
      val filteredApps = when (filter) {
         NotificationFilter.All -> notifiedApps
         NotificationFilter.Enabled -> notifiedApps.filter { it.enabled }
         NotificationFilter.Disabled -> notifiedApps.filter { !it.enabled }
         NotificationFilter.Shown -> notifiedApps.filter { it.masterSwitch == MasterSwitch.SHOW }
         NotificationFilter.Silenced -> notifiedApps.filter { it.masterSwitch == MasterSwitch.MUTE }
         NotificationFilter.Hidden -> notifiedApps.filter { it.masterSwitch == MasterSwitch.HIDE }
      }
      when (sort) {
         NotificationSort.Recent -> filteredApps
         NotificationSort.NameAscending -> filteredApps.sortedBy { it.name.lowercase() }
         NotificationSort.NameDescending -> filteredApps.sortedByDescending { it.name.lowercase() }
      }
   }

   LazyColumn(
      modifier = Modifier.fillMaxSize(),
      contentPadding = PaddingValues(
         top = WindowInsets.safeDrawing.asPaddingValues().calculateTopPadding(),
         bottom = WindowInsets.safeDrawing.asPaddingValues().calculateBottomPadding() + 12.dp,
      ),
   ) {
      item {
         Column(Modifier.padding(horizontal = 16.dp).padding(top = 12.dp, bottom = 8.dp)) {
            Text(
               text = stringResource(R.string.notifications),
               style = MaterialTheme.typography.headlineLarge,
               modifier = Modifier.semantics { heading() },
            )
            Text(
               text = stringResource(R.string.showing_apps_count, visibleApps.size),
               color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
         }
      }

      item {
         Row(
            modifier = Modifier
               .fillMaxWidth()
               .horizontalScroll(rememberScrollState())
               .padding(horizontal = 12.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
         ) {
            FilterChip(
               selected = notifiedOnly,
               onClick = { notifiedOnly = !notifiedOnly },
               label = { Text(stringResource(R.string.notified_only)) },
            )
            SortChip(
               selected = sort == NotificationSort.Recent,
               label = stringResource(R.string.sort_recent),
               onClick = { sort = NotificationSort.Recent },
            )
            SortChip(
               selected = sort == NotificationSort.NameAscending,
               label = stringResource(R.string.sort_name_ascending),
               onClick = { sort = NotificationSort.NameAscending },
            )
            SortChip(
               selected = sort == NotificationSort.NameDescending,
               label = stringResource(R.string.sort_name_descending),
               onClick = { sort = NotificationSort.NameDescending },
            )
            SortChip(
               selected = filter == NotificationFilter.All,
               label = stringResource(R.string.sort_all),
               onClick = { filter = NotificationFilter.All },
            )
            SortChip(
               selected = filter == NotificationFilter.Enabled,
               label = stringResource(R.string.sort_enabled_first),
               onClick = { filter = NotificationFilter.Enabled },
            )
            SortChip(
               selected = filter == NotificationFilter.Disabled,
               label = stringResource(R.string.sort_disabled_first),
               onClick = { filter = NotificationFilter.Disabled },
            )
            SortChip(
               selected = filter == NotificationFilter.Shown,
               label = stringResource(R.string.sort_shown_first),
               onClick = { filter = NotificationFilter.Shown },
            )
            SortChip(
               selected = filter == NotificationFilter.Silenced,
               label = stringResource(R.string.sort_silenced_first),
               onClick = { filter = NotificationFilter.Silenced },
            )
            SortChip(
               selected = filter == NotificationFilter.Hidden,
               label = stringResource(R.string.sort_hidden_first),
               onClick = { filter = NotificationFilter.Hidden },
            )
         }
      }

      item {
         ListItem(
            headlineContent = {
               Text(
                  text = stringResource(R.string.all_apps),
                  fontSize = 17.sp,
                  fontWeight = FontWeight.Medium,
               )
            },
            supportingContent = {
               Text(
                  text = ruleStatusText(state.defaultMasterSwitch),
                  color = MaterialTheme.colorScheme.onSurfaceVariant,
               )
            },
            trailingContent = {
               Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                  TextButton(onClick = { setAllEnabled(false) }) {
                     Text(stringResource(R.string.mute_all))
                  }
                  TextButton(onClick = { setAllEnabled(true) }) {
                     Text(stringResource(R.string.enable_all))
                  }
               }
            },
            shadowElevation = 0.dp,
         )
         HorizontalDivider()
      }

      items(
         items = visibleApps,
         key = { it.packageName ?: "history-${it.name}" },
      ) { app ->
         NotificationAppRow(
            app = app,
            timeProvider = timeProvider,
            onClick = { openAppDetails(app) },
            onEnabledChanged = { setAppEnabled(app, it) },
         )
      }
   }
}

@Composable
private fun NotificationTypeListScreen(
   outcome: Outcome<NotificationAppDetailsState>,
   onBack: () -> Unit,
   setAppMasterSwitch: (MasterSwitch) -> Unit,
   setChannelMasterSwitch: (NotificationChannelRuleState, MasterSwitch) -> Unit,
) {
   BackHandler(onBack = onBack)

   LazyColumn(
      modifier = Modifier.fillMaxSize(),
      contentPadding = PaddingValues(
         top = WindowInsets.safeDrawing.asPaddingValues().calculateTopPadding(),
         bottom = WindowInsets.safeDrawing.asPaddingValues().calculateBottomPadding() + 12.dp,
      ),
   ) {
      item {
         Column(Modifier.padding(horizontal = 16.dp).padding(top = 8.dp, bottom = 8.dp)) {
            TextButton(onClick = onBack) {
               Text(stringResource(R.string.back))
            }
         }
      }

      when (outcome) {
         is Outcome.Progress -> item {
            Box(
               modifier = Modifier
                  .fillMaxWidth()
                  .padding(32.dp),
               contentAlignment = Alignment.Center,
            ) {
               CircularProgressIndicator()
            }
         }

         is Outcome.Error -> item {
            Text(
               text = outcome.exception.localizedMessage ?: stringResource(R.string.generic_error),
               color = MaterialTheme.colorScheme.error,
               modifier = Modifier.padding(16.dp),
            )
         }

         is Outcome.Success -> {
            val details = outcome.data
            item {
               ListItem(
                  leadingContent = {
                     AppIcon(
                        packageName = details.packageName,
                        modifier = Modifier
                           .size(46.dp)
                           .clip(RoundedCornerShape(10.dp))
                           .background(MaterialTheme.colorScheme.surfaceVariant),
                     )
                  },
                  headlineContent = {
                     Text(
                        text = details.appName,
                        style = MaterialTheme.typography.headlineSmall,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                     )
                  },
                  supportingContent = {
                     Text(stringResource(R.string.notification_types))
                  },
                  shadowElevation = 0.dp,
               )
               HorizontalDivider()
            }

            item {
               RuleControlRow(
                  title = stringResource(R.string.all_notifications),
                  subtitle = if (details.appWide.explicit) {
                     stringResource(R.string.direct_setting)
                  } else {
                     stringResource(R.string.using_default_setting)
                  },
                  masterSwitch = details.appWide.masterSwitch,
                  onMasterSwitch = setAppMasterSwitch,
               )
            }

            if (details.channels.isEmpty()) {
               item {
                  Text(
                     text = stringResource(R.string.no_notification_types),
                     color = MaterialTheme.colorScheme.onSurfaceVariant,
                     modifier = Modifier.padding(16.dp),
                  )
               }
            } else {
               items(
                  items = details.channels,
                  key = { it.id },
               ) { channel ->
                  RuleControlRow(
                     title = channel.title,
                     subtitle = if (channel.explicit) {
                        stringResource(R.string.direct_setting)
                     } else {
                        stringResource(R.string.using_app_setting)
                     },
                     masterSwitch = channel.masterSwitch,
                     onMasterSwitch = { setChannelMasterSwitch(channel, it) },
                  )
               }
            }
         }
      }
   }
}

@Composable
private fun RuleControlRow(
   title: String,
   subtitle: String,
   masterSwitch: MasterSwitch,
   onMasterSwitch: (MasterSwitch) -> Unit,
) {
   ListItem(
      headlineContent = {
         Text(
            text = title,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
         )
      },
      supportingContent = {
         Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(
               text = subtitle,
               color = MaterialTheme.colorScheme.onSurfaceVariant,
               maxLines = 1,
               overflow = TextOverflow.Ellipsis,
            )
            Row(
               modifier = Modifier.horizontalScroll(rememberScrollState()),
               horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
               MasterSwitchChip(
                  selected = masterSwitch == MasterSwitch.SHOW,
                  label = stringResource(R.string.shown),
                  onClick = { onMasterSwitch(MasterSwitch.SHOW) },
               )
               MasterSwitchChip(
                  selected = masterSwitch == MasterSwitch.MUTE,
                  label = stringResource(R.string.silenced),
                  onClick = { onMasterSwitch(MasterSwitch.MUTE) },
               )
               MasterSwitchChip(
                  selected = masterSwitch == MasterSwitch.HIDE,
                  label = stringResource(R.string.hidden),
                  onClick = { onMasterSwitch(MasterSwitch.HIDE) },
               )
            }
         }
      },
      shadowElevation = 0.dp,
   )
   HorizontalDivider()
}

@Composable
private fun MasterSwitchChip(
   selected: Boolean,
   label: String,
   onClick: () -> Unit,
) {
   FilterChip(
      selected = selected,
      onClick = onClick,
      label = { Text(label) },
   )
}

@Composable
private fun SortChip(
   selected: Boolean,
   label: String,
   onClick: () -> Unit,
) {
   FilterChip(
      selected = selected,
      onClick = onClick,
      label = { Text(label) },
   )
}

@Composable
private fun NotificationAppRow(
   app: NotificationAppState,
   timeProvider: TimeProvider,
   onClick: () -> Unit,
   onEnabledChanged: (Boolean) -> Unit,
) {
   val lastSeen = app.lastNotification?.let { lastNotification ->
      val localDateTime = lastNotification.atZone(timeProvider.systemDefaultZoneId()).toLocalDateTime()
      java.time.format.DateTimeFormatter.ofLocalizedDateTime(FormatStyle.SHORT).format(localDateTime)
   } ?: stringResource(R.string.never_seen)
   val status = ruleStatusText(app.masterSwitch)

   ListItem(
      leadingContent = {
         AppIcon(
            packageName = app.packageName,
            modifier = Modifier
               .size(42.dp)
               .clip(RoundedCornerShape(10.dp))
               .background(MaterialTheme.colorScheme.surfaceVariant),
         )
      },
      headlineContent = {
         Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
               text = app.name,
               fontSize = 17.sp,
               modifier = Modifier.weight(1f, fill = false),
               maxLines = 1,
               overflow = TextOverflow.Ellipsis,
            )
            if (app.notificationCount > 0) {
               Badge(modifier = Modifier.padding(horizontal = 7.dp)) {
                  Text(app.notificationCount.toString())
               }
            }
         }
      },
      supportingContent = {
         Text(
            text = "$status - $lastSeen",
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            fontSize = 12.sp,
         )
      },
      trailingContent = {
         Switch(
            checked = app.enabled,
            enabled = app.packageName != null,
            onCheckedChange = onEnabledChanged,
         )
      },
      shadowElevation = 0.dp,
      modifier = Modifier.clickable(enabled = app.packageName != null, onClick = onClick),
   )
   HorizontalDivider()
}

@Composable
private fun AppIcon(packageName: String?, modifier: Modifier = Modifier) {
   if (LocalInspectionMode.current) {
      Box(modifier.background(Color.Red))
      return
   }

   if (packageName == null) {
      Box(modifier)
      return
   }

   var icon by remember(packageName) { mutableStateOf<Drawable?>(null) }
   val context = LocalContext.current

   LaunchedEffect(packageName, context) {
      icon = withContext(Dispatchers.Default) {
         runCatching { context.packageManager.getApplicationIcon(packageName) }.getOrNull()
      }
   }

   val drawable = icon
   if (drawable == null) {
      Box(modifier)
   } else {
      Image(
         painter = rememberDrawablePainter(drawable),
         contentDescription = null,
         modifier = modifier.padding(4.dp),
      )
   }
}

@Composable
private fun ruleStatusText(masterSwitch: MasterSwitch): String {
   return when (masterSwitch) {
      MasterSwitch.SHOW -> stringResource(R.string.shown)
      MasterSwitch.MUTE -> stringResource(R.string.silenced)
      MasterSwitch.HIDE -> stringResource(R.string.hidden)
   }
}

private enum class NotificationSort {
   Recent,
   NameAscending,
   NameDescending,
}

private enum class NotificationFilter {
   All,
   Enabled,
   Disabled,
   Shown,
   Silenced,
   Hidden,
}
