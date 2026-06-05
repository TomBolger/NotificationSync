@file:Suppress("MultipleEmitters")

package com.matejdro.pebblenotificationcenter.rules.ui.list

import android.graphics.drawable.Drawable
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import com.matejdro.pebblenotificationcenter.rules.ui.R
import com.matejdro.pebblenotificationcenter.ui.components.ProgressErrorSuccessScaffold
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import si.inova.kotlinova.compose.flow.collectAsStateWithLifecycleAndBlinkingPrevention
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

      ProgressErrorSuccessScaffold(
         stateOutcome::value,
         errorProgressModifier = Modifier.safeDrawingPadding()
      ) { state ->
         RuleListScreenContent(
            state = state,
            timeProvider = timeProvider,
            setAllEnabled = viewModel::setAllEnabled,
            setAppEnabled = viewModel::setAppEnabled,
         )
      }
   }
}

@Composable
private fun RuleListScreenContent(
   state: RuleListState,
   timeProvider: TimeProvider,
   setAllEnabled: (Boolean) -> Unit,
   setAppEnabled: (NotificationAppState, Boolean) -> Unit,
) {
   var notifiedOnly by remember { mutableStateOf(true) }
   var sort by rememberSaveable { mutableStateOf(NotificationSort.Recent) }
   val visibleApps = remember(state.apps, notifiedOnly, sort) {
      val filteredApps = if (notifiedOnly) {
         state.apps.filter { it.notificationCount > 0 || it.ruleId != null }
      } else {
         state.apps
      }
      when (sort) {
         NotificationSort.Recent -> filteredApps
         NotificationSort.NameAscending -> filteredApps.sortedBy { it.name.lowercase() }
         NotificationSort.NameDescending -> filteredApps.sortedByDescending { it.name.lowercase() }
         NotificationSort.EnabledFirst -> filteredApps.sortedWith(
            compareByDescending<NotificationAppState> { it.enabled }.thenBy { it.name.lowercase() }
         )
         NotificationSort.DisabledFirst -> filteredApps.sortedWith(
            compareBy<NotificationAppState> { it.enabled }.thenBy { it.name.lowercase() }
         )
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
               selected = sort == NotificationSort.EnabledFirst,
               label = stringResource(R.string.sort_enabled_first),
               onClick = { sort = NotificationSort.EnabledFirst },
            )
            SortChip(
               selected = sort == NotificationSort.DisabledFirst,
               label = stringResource(R.string.sort_disabled_first),
               onClick = { sort = NotificationSort.DisabledFirst },
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
                  text = if (state.defaultEnabled) stringResource(R.string.shown) else stringResource(R.string.muted),
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
            onEnabledChanged = { setAppEnabled(app, it) },
         )
      }
   }
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
   onEnabledChanged: (Boolean) -> Unit,
) {
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
            text = app.lastNotification?.let { lastNotification ->
               val localDateTime = lastNotification.atZone(timeProvider.systemDefaultZoneId()).toLocalDateTime()
               java.time.format.DateTimeFormatter.ofLocalizedDateTime(FormatStyle.SHORT).format(localDateTime)
            } ?: stringResource(R.string.never_seen),
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

private enum class NotificationSort {
   Recent,
   NameAscending,
   NameDescending,
   EnabledFirst,
   DisabledFirst,
}
