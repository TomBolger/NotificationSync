@file:Suppress("MagicNumber")

package com.matejdro.pebblenotificationcenter.bluetooth

import android.graphics.drawable.Drawable
import com.matejdro.pebble.bluetooth.common.PacketQueue
import com.matejdro.pebble.bluetooth.common.di.WatchappConnectionScope
import com.matejdro.pebble.bluetooth.common.util.LimitingStringEncoder
import com.matejdro.pebble.bluetooth.common.util.fixPebbleIndentation
import com.matejdro.pebble.bluetooth.common.util.writeUByte
import com.matejdro.pebble.bluetooth.common.util.writeUShort
import com.matejdro.pebblenotificationcenter.bluetooth.images.DrawableExtractor
import com.matejdro.pebblenotificationcenter.notification.ActionOrderRepository
import com.matejdro.pebblenotificationcenter.notification.NotificationRepository
import com.matejdro.pebblenotificationcenter.notification.model.Action
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dispatch.core.DefaultCoroutineScope
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import io.rebble.pebblekit2.common.util.sizeInBytes
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import logcat.logcat
import okio.Buffer
import si.inova.kotlinova.core.exceptions.UnknownCauseException
import si.inova.kotlinova.core.reporting.ErrorReporter

@Inject
@ContributesBinding(WatchappConnectionScope::class)
class NotificationDetailsPusherImpl(
   private val queue: PacketQueue,
   private val notificationRepository: NotificationRepository,
   private val actionOrderRepository: ActionOrderRepository,
   private val drawableExtractor: DrawableExtractor,
   private val scope: DefaultCoroutineScope,
   private val errorReporter: ErrorReporter,
) : NotificationDetailsPusher {
   private val stringEncoder = LimitingStringEncoder()
   private var previousDetailsSendingJob: Job? = null
   private var previousVibrationSendingJob: Job? = null

   // Magic numbers are a whole point of this function (protocol constants).
   // Use is not required for memory-only Buffer
   @Suppress("MagicNumber", "MissingUseCall")
   override fun pushNotificationDetails(bucketId: Int, maxPacketSize: Int, colorWatch: Boolean) {
      previousDetailsSendingJob?.cancel()

      val notification = notificationRepository.getNotification(bucketId)

      previousDetailsSendingJob = scope.launch {
         try {
            notificationRepository.markAsRead(bucketId)

            val buffer = Buffer()
            buffer.writeUByte(bucketId.toUByte())

            val sortedActions = actionOrderRepository.sort(notification?.actions.orEmpty().take(MAX_ACTIONS_TO_SEND))
            val encodedActions = encodedActionsThatFit(bucketId, sortedActions, maxPacketSize)

            buffer.writeUByte(encodedActions.size.toUByte())

            for (action in encodedActions) {
               buffer.writeUByte(action.action.id)
               buffer.write(action.encodedTitle)
               buffer.writeUByte(0u)
            }

            val iconBytes = iconBytesThatFit(
               bucketId = bucketId,
               iconDrawable = notification?.systemData?.iconDrawable,
               colorWatch = colorWatch,
               payloadSizeBeforeIcon = buffer.size.toInt(),
               maxPacketSize = maxPacketSize,
            )

            buffer.writeUShort(iconBytes.size.toUShort())
            buffer.write(iconBytes)

            val packetBeforeText = mapOf(
               0u to PebbleDictionaryItem.UInt8(5u),
               1u to PebbleDictionaryItem.Bytes(ByteArray(buffer.size.toInt()))
            )

            val maxTextSize = maxPacketSize - packetBeforeText.sizeInBytes()
            val encodedText = if (maxTextSize > 0) {
               stringEncoder.encodeSizeLimited(
                  notification?.systemData?.body.orEmpty().replaceUnsupportedPebbleEmoji().fixPebbleIndentation(),
                  maxTextSize
               ).encodedString
            } else {
               ByteArray(0)
            }
            buffer.write(encodedText)

            val packet = packetBeforeText + mapOf(
               1u to PebbleDictionaryItem.Bytes(buffer.readByteArray())
            )

            logcat {
               "Sending notification details for $bucketId: ${packet.sizeInBytes()} " +
                  "(${encodedActions.size}/${sortedActions.size} actions)"
            }

            launch {
               queue.sendPacket(packet, priority = PRIORITY_WATCH_TEXT)
            }

            pushVibration()
         } catch (e: CancellationException) {
            throw e
         } catch (e: Exception) {
            errorReporter.report(UnknownCauseException("Failed to push notification details", e))
         }
      }
   }

   private fun encodedActionsThatFit(
      bucketId: Int,
      sortedActions: List<Action>,
      maxPacketSize: Int,
   ): List<EncodedNotificationAction> {
      val encodedActions = ArrayList<EncodedNotificationAction>(sortedActions.size)
      var payloadSize = 1 + 1 + 2 // bucket id, action count, empty icon length.

      for (action in sortedActions) {
         val encodedTitle =
            stringEncoder.encodeSizeLimited(action.title.replaceUnsupportedPebbleEmoji(), MAX_ACTIONS_TEXT_BYTES)
               .encodedString
         val candidatePayloadSize = payloadSize + 1 + encodedTitle.size + 1
         val candidatePacketSize = packetSizeForPayloadSize(candidatePayloadSize)

         if (candidatePacketSize <= maxPacketSize) {
            encodedActions += EncodedNotificationAction(action, encodedTitle)
            payloadSize = candidatePayloadSize
         } else {
            logcat {
               "Skipping action '${action.title}' for $bucketId; " +
                  "details packet would be $candidatePacketSize/$maxPacketSize bytes before body"
            }
         }
      }

      return encodedActions
   }

   private fun iconBytesThatFit(
      bucketId: Int,
      iconDrawable: Any?,
      colorWatch: Boolean,
      payloadSizeBeforeIcon: Int,
      maxPacketSize: Int,
   ): ByteArray {
      val iconBytes = encodeNotificationIcon(iconDrawable, colorWatch)
      if (iconBytes.isEmpty()) {
         return iconBytes
      }

      val candidatePayloadSize = payloadSizeBeforeIcon + ICON_LENGTH_BYTES + iconBytes.size
      val candidatePacketSize = packetSizeForPayloadSize(candidatePayloadSize)
      if (candidatePacketSize <= maxPacketSize) {
         return iconBytes
      }

      logcat {
         "Skipping icon for $bucketId; details packet would be $candidatePacketSize/$maxPacketSize bytes before body"
      }
      return ByteArray(0)
   }

   private fun encodeNotificationIcon(iconDrawable: Any?, colorWatch: Boolean): ByteArray {
      if (iconDrawable !is Drawable) {
         return ByteArray(0)
      }

      return try {
         drawableExtractor.convertIconDrawableToBitmapBytes(
            iconDrawable,
            NOTIFICATION_ICON_SIZE_PX,
            NOTIFICATION_ICON_SIZE_PX,
            colorWatch,
         )
      } catch (e: Exception) {
         logcat { "Failed to encode notification icon: $e" }
         ByteArray(0)
      }
   }

   // Magic numbers are a whole point of this function (protocol constants).
   // Use is not required for memory-only Buffer
   @Suppress("MagicNumber", "MissingUseCall")
   private fun pushVibration() {
      val vibrationPattern = notificationRepository.pollNextVibration()
      logcat { "Next vibration: ${vibrationPattern?.contentToString() ?: "null"}" }
      if (vibrationPattern == null) {
         return
      }

      previousVibrationSendingJob?.cancel()
      previousVibrationSendingJob = scope.launch {
         val buffer = Buffer()
         for (entry in vibrationPattern) {
            buffer.writeUShort(entry.toUShort())
         }

         val packet = mapOf(
            0u to PebbleDictionaryItem.UInt8(7u),
            1u to PebbleDictionaryItem.Bytes(buffer.readByteArray())
         )
         @Suppress("SuspendFunSwallowedCancellation") // Reset vibration before re-throwing
         try {
            queue.sendPacket(packet, priority = PRIORITY_VIBRATION)
         } catch (e: CancellationException) {
            notificationRepository.resetNextVibration(vibrationPattern)
            throw e
         }
      }
   }
}

private data class EncodedNotificationAction(
   val action: Action,
   val encodedTitle: ByteArray,
)

private fun packetSizeForPayloadSize(payloadSize: Int): Int {
   return mapOf(
      0u to PebbleDictionaryItem.UInt8(5u),
      1u to PebbleDictionaryItem.Bytes(ByteArray(payloadSize))
   ).sizeInBytes()
}

private const val ICON_LENGTH_BYTES = 2
private const val NOTIFICATION_ICON_SIZE_PX = 32
private const val MAX_ACTIONS_TO_SEND = 20
private const val MAX_ACTIONS_TEXT_BYTES = 20

interface NotificationDetailsPusher {
   fun pushNotificationDetails(bucketId: Int, maxPacketSize: Int, colorWatch: Boolean)
}
