package com.matejdro.pebblenotificationcenter.bluetooth

import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.drawable.Drawable
import com.matejdro.pebble.bluetooth.common.PacketQueue
import com.matejdro.pebble.bluetooth.common.test.FakePebbleSender
import com.matejdro.pebble.bluetooth.common.test.sentData
import com.matejdro.pebble.bluetooth.common.util.requireBytes
import com.matejdro.pebblenotificationcenter.FakeNotificationServiceController
import com.matejdro.pebblenotificationcenter.bluetooth.api.WATCHAPP_UUID
import com.matejdro.pebblenotificationcenter.bluetooth.images.FakeDrawableExtractor
import com.matejdro.pebblenotificationcenter.notification.FakeActionOrderRepository
import com.matejdro.pebblenotificationcenter.notification.FakeNotificationRepository
import com.matejdro.pebblenotificationcenter.notification.model.Action
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import dispatch.core.DefaultCoroutineScope
import io.kotest.matchers.collections.shouldContain
import io.kotest.matchers.collections.shouldContainExactly
import io.kotest.matchers.collections.shouldHaveSize
import io.kotest.matchers.shouldBe
import io.rebble.pebblekit2.common.model.PebbleDictionaryItem
import io.rebble.pebblekit2.common.model.WatchIdentifier
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.jupiter.api.Test
import si.inova.kotlinova.core.test.TestScopeWithDispatcherProvider
import si.inova.kotlinova.core.test.time.virtualTimeProvider
import java.time.Instant

class NotificationDetailsPusherImplTest {
   private val scope = TestScopeWithDispatcherProvider()
   private val sender = FakePebbleSender(scope.virtualTimeProvider())
   private val packetQueue = PacketQueue(sender, WatchIdentifier("watch"), WATCHAPP_UUID)
   private val notificationRepository = FakeNotificationRepository()
   private val notificationServiceController = FakeNotificationServiceController()

   private val actionOrderRepository = FakeActionOrderRepository()

   private val drawableExtractor = FakeDrawableExtractor()

   private val notificationDetailsPusher = NotificationDetailsPusherImpl(
      packetQueue,
      notificationRepository,
      notificationServiceController,
      actionOrderRepository,
      drawableExtractor,
      DefaultCoroutineScope(scope.backgroundScope.coroutineContext),
      {},
   )

   @Test
   fun `Send text of the notification`() = scope.runTest {
      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  0, // No actions in this test

                  0, 0, // No image

                  // Hello in UTf-8
                  72,
                  101,
                  108,
                  108,
                  111
               )
            )
         )
      )
   }

   @Test
   fun `Limit the text of the notification to the max packet size`() = scope.runTest {
      setup()

      val body = "a".repeat(100)
      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               body,
               Instant.MIN,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      val details = parseSentDetails(bucketId = 12)
      details.actionCount shouldBe 0
      details.body.isNotEmpty() shouldBe true
      details.body.startsWith("aaa") shouldBe true
   }

   @Test
   fun `Keep previous packets when new request is made`() = scope.runTest {
      setup()

      repeat(3) { inex ->
         notificationRepository.putNotification(
            inex,
            ProcessedNotification(
               ParsedNotification(
                  "",
                  "",
                  "",
                  "",
                  "Hello",
                  Instant.MIN,
               )
            )
         )
      }

      sender.pauseSending = true

      notificationDetailsPusher.pushNotificationDetails(bucketId = 0, maxPacketSize = 100, colorWatch = false)
      runCurrent()

      notificationDetailsPusher.pushNotificationDetails(bucketId = 1, maxPacketSize = 100, colorWatch = false)
      runCurrent()

      notificationDetailsPusher.pushNotificationDetails(bucketId = 2, maxPacketSize = 100, colorWatch = false)
      runCurrent()

      sender.pauseSending = false
      runCurrent()

      sender.sentData
         .map { (it.getValue(1u) as PebbleDictionaryItem.Bytes).value[0] }
         .toSet() shouldBe setOf(0.toByte(), 1.toByte(), 2.toByte())
   }

   @Test
   fun `Send a list of actions of the notification`() = scope.runTest {
      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            ),
            actions = listOf(
               Action.Dismiss("A1", 0u),
               Action.Dismiss("A2", 1u),
               Action.Dismiss("A3", 2u),
            )
         ),
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 1000, colorWatch = false)

      runCurrent()

      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  3, // 3 actions
                  0, // Action ID 0
                  65, 49, 0, // A1 & null
                  1, // Action ID 1
                  65, 50, 0, // A2 & null
                  2, // Action ID 2
                  65, 51, 0, // A2 & null

                  0, 0, // No image

                  // Hello in UTf-8
                  72,
                  101,
                  108,
                  108,
                  111
               )
            )
         )
      )
   }

   @Test
   fun `Keep core notification actions when optional app actions exceed packet budget`() = scope.runTest {
      setup()
      repeat(12) {
         actionOrderRepository.moveOrder("Native action $it", -20 + it)
      }

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            ),
            actions = listOf(
               Action.Dismiss("Dismiss", 0u),
               Action.Snooze("Snooze", 1u),
               Action.PauseApp("Pause app", 2u),
               Action.PauseConversation("Pause convo", 3u),
               Action.SilenceApp("Silence app", 4u),
            ) + List(12) {
               Action.Native("Native action $it", Any(), (it + 5).toUByte())
            }
         ),
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      payloadActionIds(sender.sentData.single().requireBytes(1u)).shouldContain(4)
   }

   @Test
   fun `Keep core notification actions when optional tasker actions exceed action count limit`() = scope.runTest {
      setup()
      repeat(25) {
         actionOrderRepository.moveOrder("Tasker $it", -30 + it)
      }

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            ),
            actions = List(25) {
               Action.TaskerTask("Tasker $it", it.toUByte())
            } + listOf(
               Action.Dismiss("Dismiss", 25u),
               Action.Snooze("Snooze", 26u),
               Action.PauseApp("Pause app", 27u),
               Action.PauseConversation("Pause convo", 28u),
               Action.SilenceApp("Silence app", 29u),
            )
         ),
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      payloadActionIds(sender.sentData.single().requireBytes(1u)).shouldContain(29)
   }

   @Test
   fun `Trim action text length`() = scope.runTest {
      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            ),
            actions = listOf(
               Action.Dismiss("a".repeat(100), 0u),
            )
         ),
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  1, // 1 action
                  0, // Action ID
               ) +
                  // 17 'a' characters, followed by the ...
                  ByteArray(17) { 'a'.code.toByte() } +

                  byteArrayOf(
                     // ...
                     46, 46, 46,
                     0, // Null terminator

                     0, 0, // No image

                     // Notification body, Hello in UTf-8
                     72,
                     101,
                     108,
                     108,
                     111
                  )
            )
         )
      )
   }

   @Test
   fun `Allow maximum of 20 notification actions`() = scope.runTest {
      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            ),
            actions = List(30) { Action.Dismiss(it.toString(), it.toUByte()) }
         ),
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 1000, colorWatch = false)

      runCurrent()

      sender.sentData.shouldHaveSize(1).elementAt(0).requireBytes(1u).get(1) shouldBe 20
   }

   @Test
   fun `Prioritize long detail body over optional action labels`() = scope.runTest {
      setup()

      val body = "b".repeat(200)
      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               body,
               Instant.MIN,
            ),
            actions = List(20) { Action.Native("Very long action title $it", Any(), it.toUByte()) }
         ),
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      val details = parseSentDetails(bucketId = 12)
      details.body.isNotEmpty() shouldBe true
      details.body.startsWith("bbb") shouldBe true
      (details.actionCount < 20) shouldBe true
   }

   @Test
   fun `Do not send details when the notification does not exist`() = scope.runTest {
      setup()
      notificationServiceController.returnValue = false

      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      notificationServiceController.resyncActiveNotificationsNowCalled shouldBe true
      sender.sentData.shouldContainExactly()
      notificationRepository.notificationsMarkedAsRead.shouldContainExactly()
   }

   @Test
   fun `Resync live notifications before sending details for a missing bucket`() = scope.runTest {
      setup()
      notificationServiceController.onResyncActiveNotificationsNow = {
         notificationRepository.putNotification(
            12,
            ProcessedNotification(
               ParsedNotification(
                  "",
                  "",
                  "",
                  "",
                  "Hello",
                  Instant.MIN,
               )
            )
         )
      }

      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      notificationServiceController.resyncActiveNotificationsNowCalled shouldBe true
      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  0, // No actions in this test

                  0, 0, // No image

                  // Hello in UTF-8
                  72,
                  101,
                  108,
                  108,
                  111
               )
            )
         )
      )
   }

   @Test
   fun `Send existing live notification details without resync delay`() = scope.runTest {
      setup()
      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               key = "live-key",
               pkg = "com.app",
               title = "Title",
               subtitle = "",
               body = "Short",
               timestamp = Instant.MIN,
            )
         )
      )

      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 180, colorWatch = false)

      runCurrent()

      notificationServiceController.resyncActiveNotificationsNowCalled shouldBe false
      val payload = sender.sentData.single().requireBytes(1u)
      (payload[1].toInt() and 0xff) shouldBe 0
      payload.copyOfRange(payload.size - "Short".length, payload.size)
         .decodeToString() shouldBe "Short"
   }

   @Test
   fun `Send vibration after successful details push`() = scope.runTest {
      setup()

      notificationRepository.nextVibration = intArrayOf(10, 10, 10, 10)

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      sender.sentData.shouldHaveSize(2).elementAt(1) shouldBe mapOf(
         0u to PebbleDictionaryItem.UInt8(7),
         1u to PebbleDictionaryItem.Bytes(
            byteArrayOf(
               0, 10,
               0, 10,
               0, 10,
               0, 10,
            )
         )
      )
   }

   @Test
   fun `Send vibration even if details are superseded by another notification`() = scope.runTest {
      setup()

      sender.pauseSending = true

      notificationRepository.nextVibration = intArrayOf(10, 10, 10, 10)
      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)
      runCurrent()

      notificationRepository.nextVibration = null
      notificationRepository.putNotification(
         13,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 13, maxPacketSize = 100, colorWatch = false)
      runCurrent()

      sender.pauseSending = false
      runCurrent()

      sender.sentData.map { it.getValue(0u) }.shouldContain(PebbleDictionaryItem.UInt8(7))
   }

   @Test
   fun `Do not send vibration of the previous notification when superseded by another notification with vibration`() =
      scope.runTest {
         setup()

         sender.pauseSending = true

         notificationRepository.nextVibration = intArrayOf(10, 10, 10, 10)
         notificationRepository.putNotification(
            12,
            ProcessedNotification(
               ParsedNotification(
                  "",
                  "",
                  "",
                  "",
                  "Hello",
                  Instant.MIN,
               )
            )
         )
         notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)
         runCurrent()

         notificationRepository.nextVibration = intArrayOf(20, 20, 20, 20)
         notificationRepository.putNotification(
            13,
            ProcessedNotification(
               ParsedNotification(
                  "",
                  "",
                  "",
                  "",
                  "Hello",
                  Instant.MIN,
               )
            )
         )
         notificationDetailsPusher.pushNotificationDetails(bucketId = 13, maxPacketSize = 100, colorWatch = false)
         runCurrent()

         sender.pauseSending = false
         runCurrent()

         sender.sentData.map { it.getValue(0u) }.shouldContainExactly(
            PebbleDictionaryItem.UInt8(5),
            PebbleDictionaryItem.UInt8(5),
            PebbleDictionaryItem.UInt8(7)
         )

         sender.sentData.last() shouldBe mapOf(
            0u to PebbleDictionaryItem.UInt8(7),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  0, 20,
                  0, 20,
                  0, 20,
                  0, 20,
               )
            )
         )
      }

   @Test
   fun `Mark notification as read on sending`() = scope.runTest {
      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            )
         )
      )

      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)
      runCurrent()

      notificationRepository.notificationsMarkedAsRead.shouldContainExactly(12)
   }

   @Test
   fun `Fix indentation of the text`() = scope.runTest {
      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               " c",
               Instant.MIN,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  0, // No actions in this test
                  0, 0, // No image

                  // UTF8 Bytes for the text
                  194.toByte(), // UTF8 marker
                  160.toByte(), // Non-breaking space, not the input regular space
                  99, // c
               )
            )
         )
      )
   }

   @Test
   fun `It should respect action order from the action order repository`() = scope.runTest {
      actionOrderRepository.moveOrder("A1", 2)

      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            ),
            actions = listOf(
               Action.Dismiss("A1", 0u),
               Action.Dismiss("A2", 1u),
               Action.Dismiss("A3", 2u),
            )
         ),
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  3, // 3 actions
                  1, 65, 50, 0, // ID, A2, null
                  2, 65, 51, 0, // ID, A3, null
                  0, 65, 49, 0, // ID, A1, null

                  0, 0, // No image

                  // Hello in UTf-8
                  72,
                  101,
                  108,
                  108,
                  111
               )
            )
         )
      )
   }

   @Test
   fun `Do not send duplicate notification icon in detail packet`() = scope.runTest {
      val fakeDrawable = object : Drawable() {
         override fun draw(canvas: Canvas) {
            throw UnsupportedOperationException()
         }

         @Deprecated("Deprecated in Java")
         override fun getOpacity(): Int {
            throw UnsupportedOperationException()
         }

         override fun setAlpha(alpha: Int) {
            throw UnsupportedOperationException()
         }

         override fun setColorFilter(colorFilter: ColorFilter?) {
            throw UnsupportedOperationException()
         }
      }

      drawableExtractor.registerOutput(
         drawable = fakeDrawable,
         width = 32,
         height = 32,
         colorWatch = false,
         output = byteArrayOf(1, 2, 3)
      )

      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
               iconDrawable = fakeDrawable,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()

      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  0, // No actions in this test

                  0, 0, // No image; bucket metadata already carries the icon

                  // Hello in UTf-8
                  72,
                  101,
                  108,
                  108,
                  111
               )
            )
         )
      )
   }

   @Test
   fun `Do not send duplicate colorful notification icon in detail packet`() = scope.runTest {
      val fakeDrawable = object : Drawable() {
         override fun draw(canvas: Canvas) {
            throw UnsupportedOperationException()
         }

         @Deprecated("Deprecated in Java")
         override fun getOpacity(): Int {
            throw UnsupportedOperationException()
         }

         override fun setAlpha(alpha: Int) {
            throw UnsupportedOperationException()
         }

         override fun setColorFilter(colorFilter: ColorFilter?) {
            throw UnsupportedOperationException()
         }
      }

      drawableExtractor.registerOutput(
         drawable = fakeDrawable,
         width = 32,
         height = 32,
         colorWatch = true,
         output = byteArrayOf(1, 2, 3)
      )

      setup()

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
               iconDrawable = fakeDrawable,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = true)

      runCurrent()

      sender.sentData.shouldContainExactly(
         mapOf(
            0u to PebbleDictionaryItem.UInt8(5),
            1u to PebbleDictionaryItem.Bytes(
               byteArrayOf(
                  12, // Notification id

                  0, // No actions in this test

                  0, 0, // No image; bucket metadata already carries the icon

                  // Hello in UTf-8
                  72,
                  101,
                  108,
                  108,
                  111
               )
            )
         )
      )
   }

   @Test
   fun `Reset vibration pattern when sending fails`() = scope.runTest {
      val watchSenderJob = setup()
      sender.pauseSending = true

      notificationRepository.nextVibration = intArrayOf(10, 10, 10, 10)

      notificationRepository.putNotification(
         12,
         ProcessedNotification(
            ParsedNotification(
               "",
               "",
               "",
               "",
               "Hello",
               Instant.MIN,
            )
         )
      )
      notificationDetailsPusher.pushNotificationDetails(bucketId = 12, maxPacketSize = 100, colorWatch = false)

      runCurrent()
      watchSenderJob.cancel()
      runCurrent()

      notificationRepository.nextVibration shouldBe intArrayOf(10, 10, 10, 10)
   }

   private fun parseDetailsPayload(payload: ByteArray, bucketId: Int): ParsedDetails {
      (payload[0].toInt() and 0xff) shouldBe bucketId
      val actionCount = payload[1].toInt() and 0xff
      var position = 2

      repeat(actionCount) {
         position++ // Action id
         while (position < payload.size && payload[position] != 0.toByte()) {
            position++
         }
         position++ // Null terminator
      }

      val iconSize = ((payload[position].toInt() and 0xff) shl 8) or
         (payload[position + 1].toInt() and 0xff)
      position += 2 + iconSize

      return ParsedDetails(
         actionCount = actionCount,
         body = payload.copyOfRange(position, payload.size).decodeToString(),
      )
   }

   private fun parseSentDetails(bucketId: Int): ParsedDetails {
      val firstPacketId = sender.sentData.first().getValue(0u)
      val firstPayload = sender.sentData.first().requireBytes(1u)
      if (firstPacketId == PebbleDictionaryItem.UInt8(5)) {
         return parseDetailsPayload(firstPayload, bucketId)
      }

      firstPacketId shouldBe PebbleDictionaryItem.UInt8(13)
      (firstPayload[0].toInt() and 0xff) shouldBe bucketId
      val actionCount = firstPayload[2].toInt() and 0xff
      var position = 3

      repeat(actionCount) {
         position++ // Action id
         while (position < firstPayload.size && firstPayload[position] != 0.toByte()) {
            position++
         }
         position++ // Null terminator
      }

      val iconSize = ((firstPayload[position].toInt() and 0xff) shl 8) or
         (firstPayload[position + 1].toInt() and 0xff)
      position += 2 + iconSize

      val body = StringBuilder(firstPayload.copyOfRange(position, firstPayload.size).decodeToString())
      sender.sentData.drop(1).forEach { packet ->
         packet.getValue(0u) shouldBe PebbleDictionaryItem.UInt8(14)
         val continuationPayload = packet.requireBytes(1u)
         (continuationPayload[0].toInt() and 0xff) shouldBe bucketId
         body.append(continuationPayload.copyOfRange(3, continuationPayload.size).decodeToString())
      }

      return ParsedDetails(
         actionCount = actionCount,
         body = body.toString(),
      )
   }

   private fun payloadActionIds(payload: ByteArray): List<Int> {
      val actionCount = payload[1].toInt() and 0xff
      var position = 2
      return buildList {
         repeat(actionCount) {
            add(payload[position++].toInt() and 0xff)
            while (position < payload.size && payload[position] != 0.toByte()) {
               position++
            }
            position++
         }
      }
   }

   private fun TestScope.setup(): Job {
      return backgroundScope.launch {
         packetQueue.runQueue()
      }
   }

   private data class ParsedDetails(
      val actionCount: Int,
      val body: String,
   )
}
