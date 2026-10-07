@file:Suppress("MagicNumber") // Protocol constants

package com.matejdro.pebblenotificationcenter.bluetooth.images

import com.matejdro.pebble.bluetooth.WatchMetadata
import com.matejdro.pebble.bluetooth.common.PacketQueue
import com.matejdro.pebble.bluetooth.common.di.WatchappConnectionScope
import com.matejdro.pebblenotificationcenter.notification.NotificationRepository
import dev.zacsweers.metro.Inject
import dev.zacsweers.metro.SingleIn
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import io.rebble.pebblekit2.common.util.sizeInBytes
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import logcat.logcat

/**
 * Answers the watch's requests for notification photos (packet 16), the counterpart of PebbleOS's imaging endpoint
 * for ImagingImageTypeNotification. The watch asks for exactly the size of the band it reserved, only for the card
 * on screen; a request for another card means the user moved on, so the transfer in flight is dropped.
 *
 * Response chunks (packet 16, key 1), big endian:
 *   u8 bucket, u8 flags (1 first, 2 last, 4 no image), u16 pixel offset,
 *   first chunk: u16 width, u16 height, u8 palette count, palette (GColor8),
 *   then 4-bit pixel bytes.
 */
@Inject
@SingleIn(WatchappConnectionScope::class)
class NotificationImageServer(
   private val coroutineScope: CoroutineScope,
   private val packetQueue: PacketQueue,
   private val watchMetadata: WatchMetadata,
   private val notificationRepository: NotificationRepository,
   private val imageStore: NotificationImageStore,
) {
   private var transfer: Job? = null

   fun onImageRequested(bucketId: Int, width: Int, height: Int) {
      transfer?.cancel()
      transfer = coroutineScope.launch {
         try {
            send(bucketId, width, height)
         } catch (e: CancellationException) {
            throw e
         } catch (e: Exception) {
            logcat { "Image for $bucketId failed: ${e.message}" }
            sendNoImage(bucketId)
         }
      }
   }

   private suspend fun send(bucketId: Int, width: Int, height: Int) {
      if (width !in 1..MAX_DIMENSION || height !in 1..MAX_DIMENSION || watchMetadata.watchBufferSize <= 0) {
         sendNoImage(bucketId)
         return
      }
      val key = notificationRepository.getNotification(bucketId)?.systemData?.key
      val image = key?.let { imageStore.encode(it, width, height) }
      if (image == null) {
         sendNoImage(bucketId)
         return
      }

      val maxPayload = watchMetadata.watchBufferSize - packetOverhead()
      val header = byteArrayOf(
         (image.width shr 8).toByte(),
         image.width.toByte(),
         (image.height shr 8).toByte(),
         image.height.toByte(),
         image.palette.size.toByte(),
      ) + image.palette

      var offset = 0
      var first = true
      while (first || offset < image.pixels.size) {
         val room = maxPayload - CHUNK_HEADER_SIZE - (if (first) header.size else 0)
         val end = (offset + room).coerceAtMost(image.pixels.size)
         val last = end >= image.pixels.size
         val flags = (if (first) FLAG_FIRST else 0) or (if (last) FLAG_LAST else 0)
         val chunk = byteArrayOf(
            bucketId.toByte(),
            flags.toByte(),
            (offset shr 8).toByte(),
            offset.toByte(),
         ) + (if (first) header else ByteArray(0)) + image.pixels.copyOfRange(offset, end)

         packetQueue.sendPacket(packet(chunk), priority = PRIORITY_IMAGE)
         offset = end
         first = false
      }
      logcat { "Sent ${image.width}x${image.height} image for $bucketId" }
   }

   private suspend fun sendNoImage(bucketId: Int) {
      packetQueue.sendPacket(
         packet(byteArrayOf(bucketId.toByte(), FLAG_NO_IMAGE.toByte(), 0, 0)),
         priority = PRIORITY_IMAGE
      )
   }

   private fun packet(data: ByteArray) = mapOf(
      0u to PebbleDictionaryItem.UInt8(16u),
      1u to PebbleDictionaryItem.Bytes(data),
   )

   private fun packetOverhead(): Int = packet(ByteArray(0)).sizeInBytes()
}

private const val FLAG_FIRST = 0x01
private const val FLAG_LAST = 0x02
private const val FLAG_NO_IMAGE = 0x04
private const val CHUNK_HEADER_SIZE = 4
private const val MAX_DIMENSION = 300

/** Below the message text (1) so text always comes first, above speculative preloads (-1). */
private const val PRIORITY_IMAGE = 0
