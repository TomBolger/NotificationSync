package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.datastore.preferences.core.emptyPreferences
import com.matejdro.bucketsync.BucketSyncRepository
import com.matejdro.bucketsync.FakeBucketSyncRepository
import com.matejdro.pebblenotificationcenter.common.test.InMemoryDataStore
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import dispatch.core.DefaultCoroutineScope
import io.kotest.matchers.shouldBe
import io.kotest.matchers.ints.shouldBeGreaterThan
import io.kotest.matchers.ints.shouldBeLessThanOrEqual
import kotlinx.coroutines.test.runTest
import org.junit.jupiter.api.Test
import si.inova.kotlinova.core.test.TestScopeWithDispatcherProvider
import java.time.Instant

class WatchSyncerPayloadSizeTest {
   private val scope = TestScopeWithDispatcherProvider()

   @Test
   fun `Notification summary bucket fits Basalt sync packets`() = scope.runTest {
      val bucketSyncRepository = FakeBucketSyncRepository(PROTOCOL_VERSION.toInt())
      val watchSyncer = WatchSyncerImpl(
         bucketSyncRepository,
         InMemoryDataStore(emptyPreferences()),
         DefaultCoroutineScope(scope.backgroundScope.coroutineContext),
         notificationImageStore = com.matejdro.pebblenotificationcenter.bluetooth.images.NoNotificationImages,
      )

      watchSyncer.init(enablePreferences = false)
      watchSyncer.syncNotification(
         ProcessedNotification(
            ParsedNotification(
               key = "key",
               pkg = "com.app",
               title = "a".repeat(200),
               subtitle = "b".repeat(200),
               body = "c".repeat(1000),
               timestamp = Instant.ofEpochSecond(1_767_554_305)
            )
         ),
         emptyPreferences()
      )

      val update = bucketSyncRepository.awaitNextUpdate(0u, emptyList())
      update.bucketsToUpdate.single().data.size shouldBeLessThanOrEqual 100
   }

   @Test
   fun `Notification summary bucket uses larger payloads on large-buffer watches`() = scope.runTest {
      val bucketSyncRepository = FakeBucketSyncRepository(PROTOCOL_VERSION.toInt())
      val watchSyncer = WatchSyncerImpl(
         bucketSyncRepository,
         InMemoryDataStore(emptyPreferences()),
         DefaultCoroutineScope(scope.backgroundScope.coroutineContext),
         notificationImageStore = com.matejdro.pebblenotificationcenter.bluetooth.images.NoNotificationImages,
      )

      watchSyncer.init(enablePreferences = false)
      watchSyncer.updateWatchPayloadLimits(4096)
      watchSyncer.syncNotification(
         ProcessedNotification(
            ParsedNotification(
               key = "key",
               pkg = "com.app",
               title = "a".repeat(200),
               subtitle = "b".repeat(200),
               body = "c".repeat(1000),
               timestamp = Instant.ofEpochSecond(1_767_554_305)
            )
         ),
         emptyPreferences()
      )

      val update = bucketSyncRepository.awaitNextUpdate(0u, emptyList())
      val payloadSize = update.bucketsToUpdate.single().data.size
      payloadSize shouldBeGreaterThan 100
      payloadSize shouldBeLessThanOrEqual BucketSyncRepository.MAX_BUCKET_SIZE_BYTES
   }
   @Test
   fun `Long paragraphs remain partial summaries and keep their body text`() = scope.runTest {
      val repository = FakeBucketSyncRepository(PROTOCOL_VERSION.toInt())
      val syncer = WatchSyncerImpl(
         repository,
         InMemoryDataStore(emptyPreferences()),
         DefaultCoroutineScope(scope.backgroundScope.coroutineContext),
         notificationImageStore = com.matejdro.pebblenotificationcenter.bluetooth.images.NoNotificationImages,
      )
      syncer.init(enablePreferences = false)
      val notification = ParsedNotification("key", "com.app", "Messages", "", "a".repeat(7000), Instant.EPOCH)
      notification.watchTitle() shouldBe "Messages"
      notification.watchBody() shouldBe notification.body
      syncer.syncNotification(ProcessedNotification(notification), emptyPreferences())
      val update = repository.awaitNextUpdate(0u, emptyList())
      (update.activeBucketFlags.single().toInt() and 0x08) shouldBe 0
      update.bucketsToUpdate.single().data.takeLast(3) shouldBe listOf<Byte>(46, 46, 46)
   }

}
