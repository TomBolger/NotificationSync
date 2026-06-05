package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.datastore.preferences.core.emptyPreferences
import com.matejdro.bucketsync.FakeBucketSyncRepository
import com.matejdro.pebblenotificationcenter.common.test.InMemoryDataStore
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import dispatch.core.DefaultCoroutineScope
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
}
