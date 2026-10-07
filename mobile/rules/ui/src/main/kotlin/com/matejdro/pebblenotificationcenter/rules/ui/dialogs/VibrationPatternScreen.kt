package com.matejdro.pebblenotificationcenter.rules.ui.dialogs

import android.annotation.SuppressLint
import android.app.NotificationManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.input.InputTransformation
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.clearText
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationCompat
import androidx.core.content.getSystemService
import com.airbnb.android.showkase.annotation.ShowkaseComposable
import com.matejdro.pebblenotificationcenter.common.NotificationsKeys.CHANNEL_ID_TESTS
import com.matejdro.pebblenotificationcenter.common.NotificationsKeys.NOTIFICATION_ID_PATTERN_TEST
import com.matejdro.pebblenotificationcenter.notification.NotificationConstants
import com.matejdro.pebblenotificationcenter.notification.utils.parseVibrationPattern
import com.matejdro.pebblenotificationcenter.rules.ui.R
import com.matejdro.pebblenotificationcenter.ui.components.AlertDialogInnerContent
import com.matejdro.pebblenotificationcenter.ui.debugging.FullScreenPreviews
import com.matejdro.pebblenotificationcenter.ui.debugging.PreviewTheme
import kotlinx.serialization.Serializable
import si.inova.kotlinova.compose.result.LocalResultPassingStore
import si.inova.kotlinova.compose.result.ResultKey
import si.inova.kotlinova.navigation.instructions.goBack
import si.inova.kotlinova.navigation.navigator.Navigator
import si.inova.kotlinova.navigation.screenkeys.DialogKey
import si.inova.kotlinova.navigation.screenkeys.ScreenKey
import si.inova.kotlinova.navigation.screens.InjectNavigationScreen
import si.inova.kotlinova.navigation.screens.Screen
import com.matejdro.pebblenotificationcenter.sharedresources.R as sharedR

@InjectNavigationScreen
class VibrationPatternScreen(private val navigator: Navigator) : Screen<VibrationPatternScreenKey>() {
   @Composable
   override fun Content(key: VibrationPatternScreenKey) {
      val resultPassingStore = LocalResultPassingStore.current
      val context = LocalContext.current
      val resources = LocalResources.current

      VibrationPatternScreenContent(
         key,
         accept = { pattern ->
            navigator.goBack()
            resultPassingStore.sendResult(key.result, pattern)
         },
         dismiss = { navigator.goBack() },
         test = { pattern ->
            val notification = NotificationCompat.Builder(context, CHANNEL_ID_TESTS)
               .setContentTitle(resources.getString(R.string.vibration_pattern_test))
               .setContentText(resources.getString(R.string.feel_the_new_pattern))
               .setSmallIcon(R.drawable.ic_watch_vibrate)
               .addExtras(
                  Bundle().apply {
                     putBoolean(NotificationConstants.KEY_FORCE_VIBRATE, true)
                     putShortArray(NotificationConstants.KEY_VIBRATION_PATTERN, pattern.toShortArray())
                  }
               )

            val notificationManager = context.getSystemService<NotificationManager>()!!
            notificationManager.notify(NOTIFICATION_ID_PATTERN_TEST, notification.build())
         }
      )
   }
}

@Composable
private fun VibrationPatternScreenContent(
   key: VibrationPatternScreenKey,
   accept: (String) -> Unit,
   dismiss: () -> Unit,
   test: (List<Short>) -> Unit,
) {
   val textFieldState = rememberTextFieldState(key.existingPattern)
   var parsedPattern by remember { mutableStateOf<List<Short>?>(parseVibrationPattern(key.existingPattern)) }

   @Suppress("ComplexCondition") // Understandable in this case
   val limitToNumbersAndCommas = InputTransformation {
      for (i in 0 until length) {
         val char = this.charAt(i)
         if (
            !char.isDigit() &&
            char != '.' &&
            char != ',' &&
            char != ' '
         ) {
            revertAllChanges()
            return@InputTransformation
         }
      }

      parsedPattern = parseVibrationPattern(toString())
   }

   AlertDialogInnerContent(
      title = {
         Text(text = "Vibration pattern")
      },
      dismissButton = {
         TextButton(
            onClick = {
               dismiss()
            }
         ) {
            Text(stringResource(sharedR.string.cancel))
         }
      },
      confirmButton = {
         TextButton(
            onClick = {
               accept(textFieldState.text.toString())
            },
            enabled = parsedPattern != null
         ) {
            Text(stringResource(sharedR.string.ok))
         }
      },
      content = {
         val vibrator = LocalContext.current.getSystemService<Vibrator>()!!
         val tapperInteractionSource = remember { MutableInteractionSource() }
         val lastTransition = remember { mutableLongStateOf(-1) }
         var recording by remember { mutableStateOf(false) }
         var showNumbers by remember { mutableStateOf(false) }

         fun setPattern(pattern: String) {
            textFieldState.edit { replace(0, length, pattern) }
            parsedPattern = parseVibrationPattern(pattern)
         }

         // Scrolls so everything stays reachable on short screens or with large display/font sizes.
         val scrollState = rememberScrollState()
         LaunchedEffect(showNumbers) {
            if (showNumbers) {
               withFrameNanos { } // wait for the number box to be laid out
               scrollState.animateScrollTo(scrollState.maxValue)
            }
         }

         Column(
            verticalArrangement = Arrangement.spacedBy(16.dp),
            modifier = Modifier.verticalScroll(scrollState),
         ) {
            // What the current pattern looks like: filled blocks buzz, gaps are pauses.
            PatternPreview(parsedPattern.orEmpty())

            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
               Text(
                  text = stringResource(R.string.vibration_patterns_core),
                  style = MaterialTheme.typography.titleSmall,
               )
               FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                  val current = textFieldState.text.toString()
                  coreVibrationPatterns.forEach { preset ->
                     FilterChip(
                        selected = current == preset.pattern,
                        onClick = {
                           recording = false
                           setPattern(preset.pattern)
                           // Picking a preset lets you feel it right away.
                           parseVibrationPattern(preset.pattern)?.let { vibrator.playPattern(it) }
                        },
                        label = { Text(preset.name) },
                     )
                  }
               }
            }

            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
               Text(text = "Or make your own", style = MaterialTheme.typography.titleSmall)
               Text(
                  text = if (recording) {
                     "Recording. Press and hold to buzz, let go to pause. Tap Done when finished."
                  } else {
                     "Press and hold the pad in the rhythm you want. Recording starts with your first press."
                  },
                  style = MaterialTheme.typography.bodySmall,
               )
            }

            Button(
               onClick = {},
               modifier = Modifier
                  .fillMaxWidth()
                  .height(96.dp),
               shape = MaterialTheme.shapes.large,
               colors = ButtonDefaults.buttonColors(
                  containerColor = if (recording) {
                     MaterialTheme.colorScheme.tertiary
                  } else {
                     MaterialTheme.colorScheme.tertiaryContainer
                  },
                  contentColor = if (recording) {
                     MaterialTheme.colorScheme.onTertiary
                  } else {
                     MaterialTheme.colorScheme.onTertiaryContainer
                  },
               ),
               interactionSource = tapperInteractionSource,
            ) {
               Text(if (recording) "Recording… hold to buzz" else "Hold here to record")
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
               OutlinedButton(
                  onClick = { parsedPattern?.let { vibrator.playPattern(it) } },
                  enabled = parsedPattern != null,
                  modifier = Modifier.weight(1f),
               ) { Text("Feel on phone") }
               OutlinedButton(
                  onClick = { parsedPattern?.let { test(it) } },
                  enabled = parsedPattern != null,
                  modifier = Modifier.weight(1f),
               ) { Text("Send to watch") }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
               if (recording) {
                  TextButton(onClick = { recording = false }) { Text("Done recording") }
               }
               TextButton(onClick = { showNumbers = !showNumbers }) {
                  Text(if (showNumbers) "Hide numbers" else "Edit as numbers")
               }
            }

            if (showNumbers) {
               TextField(
                  textFieldState,
                  Modifier.fillMaxWidth(),
                  label = { Text("Buzz, pause, buzz… in milliseconds") },
                  onKeyboardAction = { accept(textFieldState.text.toString()) },
                  keyboardOptions = KeyboardOptions(
                     imeAction = ImeAction.Done,
                     keyboardType = KeyboardType.Number,
                  ),
                  lineLimits = TextFieldLineLimits.SingleLine,
                  inputTransformation = limitToNumbersAndCommas
               )
            }
         }

         LaunchedEffect(Unit) {
            var pressed: Boolean = false

            tapperInteractionSource.interactions.collect { interaction ->
               val nowPressed = interaction is PressInteraction.Press
               if (nowPressed && !recording) {
                  // First press starts a fresh recording.
                  recording = true
                  lastTransition.longValue = -1
                  textFieldState.clearText()
                  parsedPattern = null
               }
               if (nowPressed != pressed) {
                  if (nowPressed) {
                     if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                        vibrator.vibrate(VibrationEffect.createOneShot(LONG_VIBRATION, VibrationEffect.DEFAULT_AMPLITUDE))
                     } else {
                        @Suppress("DEPRECATION")
                        vibrator.vibrate(LONG_VIBRATION)
                     }
                  } else {
                     vibrator.cancel()
                  }

                  if (lastTransition.longValue >= 0) {
                     val msDiff = SystemClock.uptimeMillis() - lastTransition.longValue
                     textFieldState.edit {
                        if (length != 0) {
                           append(", ")
                        }
                        append(msDiff.toString())
                     }
                     parsedPattern = parseVibrationPattern(textFieldState.text.toString())
                  }
                  lastTransition.longValue = SystemClock.uptimeMillis()
                  pressed = nowPressed
               }
            }
         }

         DisposableEffect(Unit) {
            onDispose {
               vibrator.cancel()
            }
         }
      },
   )
}

@Composable
private fun PatternPreview(pattern: List<Short>) {
   val buzzColor = MaterialTheme.colorScheme.primary
   val trackColor = MaterialTheme.colorScheme.surfaceVariant
   val total = pattern.sumOf { it.toInt().coerceAtLeast(0) }.coerceAtLeast(1)
   val seconds = total / 1000f

   Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
      Canvas(
         Modifier
            .fillMaxWidth()
            .height(28.dp)
      ) {
         drawRoundRect(trackColor, cornerRadius = CornerRadius(8.dp.toPx()))
         var x = 0f
         pattern.forEachIndexed { index, value ->
            val width = size.width * value.toInt().coerceAtLeast(0) / total
            if (index % 2 == 0) {
               drawRoundRect(
                  buzzColor,
                  topLeft = Offset(x, 0f),
                  size = Size(width.coerceAtLeast(2.dp.toPx()), size.height),
                  cornerRadius = CornerRadius(4.dp.toPx()),
               )
            }
            x += width
         }
      }
      Text(
         text = if (pattern.isEmpty()) "No pattern yet" else "%.1f seconds".format(seconds),
         style = MaterialTheme.typography.labelSmall,
      )
   }
}

/** Plays a "buzz, pause, buzz, ..." pattern on the phone so it can be felt before saving. */
private fun Vibrator.playPattern(pattern: List<Short>) {
   val timings = (listOf(0L) + pattern.map { it.toLong().coerceAtLeast(0) }).toLongArray()
   cancel()
   if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      vibrate(VibrationEffect.createWaveform(timings, -1))
   } else {
      @Suppress("DEPRECATION")
      vibrate(timings, -1)
   }
}

@ShowkaseComposable(group = "test")
@Composable
@FullScreenPreviews
@SuppressLint("VisibleForTests") // Previews are sort of tests
internal fun VibrationPatternScreenPreview() {
   PreviewTheme {
      Box {
         VibrationPatternScreenContent(
            VibrationPatternScreenKey("100, 200", ResultKey(0, 0)),
            {},
            {},
            {},
         )
      }
   }
}

@ShowkaseComposable(group = "test")
@Composable
@Preview
internal fun VibrationPatternScreenBlankPreview() {
   PreviewTheme {
      Box {
         VibrationPatternScreenContent(
            VibrationPatternScreenKey("", ResultKey(0, 0)),
            {},
            {},
            {},
         )
      }
   }
}

@Serializable
data class VibrationPatternScreenKey(
   val existingPattern: String,
   val result: ResultKey<String>,
) : ScreenKey(), DialogKey

private const val LONG_VIBRATION = 10_000L

private val coreVibrationPatterns = listOf(
   CoreVibrationPattern("Standard", "500"),
   CoreVibrationPattern("Pulses", "50, 50, 50, 50, 50, 50, 50"),
   CoreVibrationPattern("Double", "200, 75, 200"),
   CoreVibrationPattern("Triple", "200, 75, 200, 75, 200"),
   CoreVibrationPattern("Bloom", "35, 61, 47, 53, 50, 40, 81, 171, 189, 236, 47, 70, 38, 44, 39, 62, 79, 171, 181"),
   CoreVibrationPattern("Pips", "40, 960, 40, 960, 40, 960, 40, 960, 40, 960, 500"),
   CoreVibrationPattern("Ole", "61, 194, 272, 153, 47, 77, 47, 78, 46, 89, 54, 78, 47, 70, 388"),
   CoreVibrationPattern("SOS", "100, 75, 100, 75, 100, 220, 300, 75, 300, 75, 300, 150, 100, 75, 100, 75, 100"),
   CoreVibrationPattern("Ohhh, Oh", "459, 522, 144, 171, 173, 162, 72, 135, 555, 386, 514"),
   CoreVibrationPattern(
      "Five",
      "68, 178, 80, 237, 54, 95, 122, 221, 154, 221, 139, 218, 81, 161, 137, 189, 55, 95, 130, 211, 188, 178, 222",
   ),
   CoreVibrationPattern("Two", "135, 269, 847, 394, 40, 159, 48, 170, 31, 144, 64, 136, 64, 162, 36, 163, 122"),
)

private data class CoreVibrationPattern(
   val name: String,
   val pattern: String,
)
