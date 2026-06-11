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
import com.matejdro.pebblenotificationcenter.notification.NotificationServiceController
import com.matejdro.pebblenotificationcenter.notification.model.Action
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dispatch.core.DefaultCoroutineScope
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import io.rebble.pebblekit2.common.util.sizeInBytes
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
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
   private val notificationServiceController: NotificationServiceController,
   private val actionOrderRepository: ActionOrderRepository,
   private val drawableExtractor: DrawableExtractor,
   private val scope: DefaultCoroutineScope,
   private val errorReporter: ErrorReporter,
) : NotificationDetailsPusher {
   private val stringEncoder = LimitingStringEncoder()
   private var previousDetailsSendingJob: Job? = null
   private var previousVibrationSendingJob: Job? = null

   override fun pushNotificationDetails(bucketId: Int, maxPacketSize: Int, colorWatch: Boolean) {
      previousDetailsSendingJob?.cancel()

      previousDetailsSendingJob = scope.launch {
         pushNotificationDetailsSafely(bucketId, maxPacketSize, colorWatch, this)
      }
   }

   private suspend fun pushNotificationDetailsSafely(
      bucketId: Int,
      maxPacketSize: Int,
      colorWatch: Boolean,
      detailsScope: CoroutineScope,
   ) {
      try {
         val notification = liveNotificationForDetails(bucketId) ?: return
         notificationRepository.markAsRead(bucketId)

         val detailsPackets = createDetailsPackets(
            bucketId = bucketId,
            bodyText = notification.systemData.body.replaceUnsupportedPebbleEmoji().fixPebbleIndentation(),
            actions = notification.actions,
            iconDrawable = notification.systemData.iconDrawable,
            colorWatch = colorWatch,
            maxPacketSize = maxPacketSize,
         )

         logcat {
            "Sending notification details for $bucketId: ${detailsPackets.packets.size} packet(s), " +
               "${detailsPackets.packets.first().sizeInBytes()} first packet bytes " +
               "(${detailsPackets.encodedActionCount}/${detailsPackets.totalActionCount} actions)"
         }

         detailsScope.launch {
            for (packet in detailsPackets.packets) {
               queue.sendPacket(packet, priority = PRIORITY_WATCH_TEXT)
            }
         }

         pushVibration()
      } catch (e: CancellationException) {
         throw e
      } catch (e: Exception) {
         errorReporter.report(UnknownCauseException("Failed to push notification details", e))
      }
   }

   private suspend fun liveNotificationForDetails(bucketId: Int): ProcessedNotification? {
      var notification = notificationRepository.getNotification(bucketId)
      if (notification == null && notificationServiceController.resyncActiveNotificationsNow()) {
         notification = notificationRepository.getNotification(bucketId)
      }
      if (notification == null) {
         logcat { "Skipping details for $bucketId; no matching live phone notification" }
      }
      return notification
   }

   // Magic numbers are a whole point of this function (protocol constants).
   // Use is not required for memory-only Buffer.
   @Suppress("MagicNumber", "MissingUseCall")
   private suspend fun createDetailsPackets(
      bucketId: Int,
      bodyText: String,
      actions: List<Action>,
      iconDrawable: Any?,
      colorWatch: Boolean,
      maxPacketSize: Int,
   ): NotificationDetailsPackets {
      val buffer = Buffer()
      buffer.writeUByte(bucketId.toUByte())

      val sortedActions = actionOrderRepository.sort(actions)
      val encodedActions = encodedActionsThatFit(bucketId, sortedActions, maxPacketSize)

      buffer.writeUByte(encodedActions.size.toUByte())

      for (action in encodedActions) {
         buffer.writeUByte(action.action.id)
         buffer.write(action.encodedTitle)
         buffer.writeUByte(0u)
      }

      val iconBytes = iconBytesThatFit(
         bucketId = bucketId,
         iconDrawable = iconDrawable,
         colorWatch = colorWatch,
         payloadSizeBeforeIcon = buffer.size.toInt(),
         maxPacketSize = maxPacketSize,
      )

      buffer.writeUShort(iconBytes.size.toUShort())
      buffer.write(iconBytes)

      val packets = bodyPacketsThatFit(
         bucketId = bucketId,
         bodyText = bodyText,
         encodedActions = encodedActions,
         iconBytes = iconBytes,
         buffer = buffer,
         maxPacketSize = maxPacketSize,
      )

      return NotificationDetailsPackets(
         packets = packets,
         encodedActionCount = encodedActions.size,
         totalActionCount = sortedActions.size,
      )
   }

   private fun bodyPacketsThatFit(
      bucketId: Int,
      bodyText: String,
      encodedActions: List<EncodedNotificationAction>,
      iconBytes: ByteArray,
      buffer: Buffer,
      maxPacketSize: Int,
   ): List<Map<UInt, PebbleDictionaryItem>> {
      val packetBeforeText = mapOf(
         0u to PebbleDictionaryItem.UInt8(5u),
         1u to PebbleDictionaryItem.Bytes(ByteArray(buffer.size.toInt())),
      )

      val maxTextSize = maxPacketSize - packetBeforeText.sizeInBytes()
      val encodedText = if (maxTextSize > 0) {
         stringEncoder.encodeSizeLimited(bodyText, maxTextSize)
      } else {
         LimitingStringEncoder.Result(ByteArray(0), bodyText.isNotEmpty())
      }

      if (encodedText.wasTrimmed) {
         return createChunkedDetailsPackets(
            bucketId = bucketId,
            bodyText = bodyText,
            encodedActions = encodedActions,
            iconBytes = iconBytes,
            maxPacketSize = maxPacketSize,
         )
      }

      buffer.write(encodedText.encodedString)
      return listOf(
         packetBeforeText + mapOf(
            1u to PebbleDictionaryItem.Bytes(buffer.readByteArray()),
         ),
      )
   }

   private fun encodedActionsThatFit(
      bucketId: Int,
      sortedActions: List<Action>,
      maxPacketSize: Int,
   ): List<EncodedNotificationAction> {
      val sortedActionsLimited = sortedActions.take(MAX_ACTIONS_TO_SEND)
      val encodedInRequestedOrder = encodeActionsThatFit(bucketId, sortedActionsLimited, maxPacketSize)
      val allCoreActionsIncluded = sortedActions
         .filter { it.isCoreWatchAction() }
         .all { coreAction -> sortedActionsLimited.any { it.id == coreAction.id } }
      if (encodedInRequestedOrder.size == sortedActionsLimited.size && allCoreActionsIncluded) {
         return encodedInRequestedOrder
      }

      val coreFirstActions = sortedActions.stableCoreActionOrder().take(MAX_ACTIONS_TO_SEND)
      if (coreFirstActions == sortedActionsLimited) {
         return encodedInRequestedOrder
      }

      return encodeActionsThatFit(bucketId, coreFirstActions, maxPacketSize)
   }

   private fun encodeActionsThatFit(
      bucketId: Int,
      actionsToEncode: List<Action>,
      maxPacketSize: Int,
   ): List<EncodedNotificationAction> {
      val encodedActions = ArrayList<EncodedNotificationAction>(actionsToEncode.size)
      var payloadSize = 1 + 1 + 1 + 2 // bucket id, v2 chunk count, action count, empty icon length.

      for (action in actionsToEncode) {
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

   private fun List<Action>.stableCoreActionOrder(): List<Action> {
      return filter { it.isCoreWatchAction() } + filterNot { it.isCoreWatchAction() }
   }

   private fun Action.isCoreWatchAction(): Boolean {
      return when (this) {
         is Action.Dismiss,
         is Action.Snooze,
         is Action.PauseApp,
         is Action.PauseConversation,
         is Action.SilenceApp,
         is Action.ShowImage,
         -> true

         is Action.Native,
         is Action.Reply,
         is Action.TaskerTask,
         -> false
      }
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

   private fun createChunkedDetailsPackets(
      bucketId: Int,
      bodyText: String,
      encodedActions: List<EncodedNotificationAction>,
      iconBytes: ByteArray,
      maxPacketSize: Int,
   ): List<Map<UInt, PebbleDictionaryItem>> {
      val bodyBytes = stringEncoder.encodeSizeLimited(bodyText, MAX_DETAIL_BODY_TEXT_BYTES).encodedString
      val initialPayloadWithoutText = Buffer()
      initialPayloadWithoutText.writeUByte(bucketId.toUByte())
      initialPayloadWithoutText.writeUByte(1u) // Replaced with the final chunk count below.
      initialPayloadWithoutText.writeUByte(encodedActions.size.toUByte())
      for (action in encodedActions) {
         initialPayloadWithoutText.writeUByte(action.action.id)
         initialPayloadWithoutText.write(action.encodedTitle)
         initialPayloadWithoutText.writeUByte(0u)
      }
      initialPayloadWithoutText.writeUShort(iconBytes.size.toUShort())
      initialPayloadWithoutText.write(iconBytes)

      val initialHeader = initialPayloadWithoutText.readByteArray()
      val firstChunkSize = (maxPacketSize - packetSizeForPayloadSize(initialHeader.size)).coerceAtLeast(0)
      val continuationChunkSize =
         (maxPacketSize - packetSizeForPayloadSize(CONTINUATION_PAYLOAD_HEADER_BYTES)).coerceAtLeast(0)
      val bodyChunks = splitBodyBytes(bodyBytes, firstChunkSize, continuationChunkSize)
      val totalChunks = bodyChunks.size.coerceAtMost(UByte.MAX_VALUE.toInt()).toUByte()
      initialHeader[1] = totalChunks.toByte()

      val initialPayload = Buffer()
      initialPayload.write(initialHeader)
      initialPayload.write(bodyChunks.first())

      val packets = ArrayList<Map<UInt, PebbleDictionaryItem>>(bodyChunks.size)
      packets += mapOf(
         0u to PebbleDictionaryItem.UInt8(PACKET_NOTIFICATION_DETAILS_V2),
         1u to PebbleDictionaryItem.Bytes(initialPayload.readByteArray())
      )

      bodyChunks.drop(1).forEachIndexed { index, chunk ->
         val payload = Buffer()
         payload.writeUByte(bucketId.toUByte())
         payload.writeUByte((index + 1).toUByte())
         payload.writeUByte(totalChunks)
         payload.write(chunk)
         packets += mapOf(
            0u to PebbleDictionaryItem.UInt8(PACKET_NOTIFICATION_DETAILS_CONTINUATION),
            1u to PebbleDictionaryItem.Bytes(payload.readByteArray())
         )
      }

      return packets
   }

   private fun splitBodyBytes(bodyBytes: ByteArray, firstChunkSize: Int, continuationChunkSize: Int): List<ByteArray> {
      if (bodyBytes.isEmpty()) {
         return listOf(ByteArray(0))
      }

      val chunks = ArrayList<ByteArray>()
      val firstChunkEnd = firstChunkSize.coerceAtMost(bodyBytes.size)
      chunks += bodyBytes.copyOfRange(0, firstChunkEnd)

      if (firstChunkEnd >= bodyBytes.size || continuationChunkSize <= 0) {
         return chunks
      }

      var offset = firstChunkEnd
      while (offset < bodyBytes.size && chunks.size < UByte.MAX_VALUE.toInt()) {
         val end = (offset + continuationChunkSize).coerceAtMost(bodyBytes.size)
         chunks += bodyBytes.copyOfRange(offset, end)
         offset = end
      }

      return chunks
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

private data class NotificationDetailsPackets(
   val packets: List<Map<UInt, PebbleDictionaryItem>>,
   val encodedActionCount: Int,
   val totalActionCount: Int,
)

private fun packetSizeForPayloadSize(payloadSize: Int): Int {
   return mapOf(
      0u to PebbleDictionaryItem.UInt8(5u),
      1u to PebbleDictionaryItem.Bytes(ByteArray(payloadSize))
   ).sizeInBytes()
}

private const val ICON_LENGTH_BYTES = 2
private const val PACKET_NOTIFICATION_DETAILS_V2 = 13
private const val PACKET_NOTIFICATION_DETAILS_CONTINUATION = 14
private const val CONTINUATION_PAYLOAD_HEADER_BYTES = 3
private const val NOTIFICATION_ICON_SIZE_PX = 32
private const val MAX_ACTIONS_TO_SEND = 20
private const val MAX_ACTIONS_TEXT_BYTES = 20
private const val MAX_DETAIL_BODY_TEXT_BYTES = 4000

interface NotificationDetailsPusher {
   fun pushNotificationDetails(bucketId: Int, maxPacketSize: Int, colorWatch: Boolean)
}
