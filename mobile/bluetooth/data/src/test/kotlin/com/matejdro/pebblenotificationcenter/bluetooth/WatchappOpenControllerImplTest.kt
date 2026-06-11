package com.matejdro.pebblenotificationcenter.bluetooth

import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import com.matejdro.pebble.bluetooth.common.test.FakePebbleSender
import com.matejdro.pebblenotificationcenter.bluetooth.api.WATCHAPP_UUID
import com.matejdro.pebblenotificationcenter.common.test.InMemoryDataStore
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.keys.set
import dispatch.core.DefaultCoroutineScope
import io.kotest.matchers.collections.shouldBeEmpty
import io.kotest.matchers.collections.shouldContainExactly
import io.kotest.matchers.shouldBe
import io.rebble.pebblekit2.common.model.WatchIdentifier
import io.rebble.pebblekit2.model.Watchapp
import kotlinx.coroutines.delay
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.Test
import si.inova.kotlinova.core.test.time.virtualTimeProvider
import java.util.UUID

class WatchappOpenControllerImplTest {
   private val scope = TestScope()
   private val pebbleSender = FakePebbleSender(scope.virtualTimeProvider())
   private val pebbleInfoRetriever = FakePebbleInfoRetriever()
   private val preferences = InMemoryDataStore(emptyPreferences())
   private val controller: WatchappOpenController = WatchappOpenControllerImpl(
      pebbleSender,
      pebbleInfoRetriever,
      preferences,
      DefaultCoroutineScope(scope.backgroundScope.coroutineContext)
   )

   @BeforeEach
   fun setUp() {
      pebbleInfoRetriever.setConnectedWatchIds(listOf(WatchIdentifier("TheWatch")))
   }

   @Test
   fun `Return false by default`() {
      controller.isNextWatchappOpenForAutoSync() shouldBe false
   }

   @Test
   fun `Return true when set`() {
      controller.setNextWatchappOpenForAutoSync()

      controller.isNextWatchappOpenForAutoSync() shouldBe true
   }

   @Test
   fun `Return false when reset`() {
      controller.setNextWatchappOpenForAutoSync()
      controller.resetNextWatchappOpen()

      controller.isNextWatchappOpenForAutoSync() shouldBe false
   }

   @Test
   fun `Start the app on the watch when requested`() = scope.runTest {
      controller.openWatchapp()

      pebbleSender.startedApps.shouldContainExactly(
         FakePebbleSender.AppLifecycleEvent(WATCHAPP_UUID, listOf(WatchIdentifier("TheWatch")))
      )
   }

   @Test
   fun `Wait to start the app until watch returns to watchface when enabled`() = scope.runTest {
      val otherApp = UUID.fromString("caf5e298-d9e7-44a9-9177-d5ed6acb719a")
      val watchface = UUID.fromString("c11fd5e6-f7cb-43bc-ab32-b043f1358fd8")
      preferences.edit {
         it[GlobalPreferenceKeys.waitForWatchfaceBeforeOpening] = true
      }
      pebbleInfoRetriever.setActiveApp(WatchIdentifier("TheWatch"), Watchapp(otherApp, "Important app", Watchapp.Type.WATCHAPP))

      controller.openWatchapp()
      runCurrent()

      pebbleSender.startedApps.shouldBeEmpty()

      pebbleInfoRetriever.setActiveApp(WatchIdentifier("TheWatch"), Watchapp(watchface, "Watchface", Watchapp.Type.WATCHFACE))
      withTimeout(1000) {
         while (pebbleSender.startedApps.isEmpty()) {
            delay(10)
         }
      }

      pebbleSender.startedApps.size shouldBe 1
      pebbleSender.startedApps.first().watchappUUID shouldBe WATCHAPP_UUID
      pebbleSender.startedApps.first().watches?.map { it.value } shouldBe listOf("TheWatch")
   }

   @Test
   fun `Start the app immediately from watchface when wait is enabled`() = scope.runTest {
      val watchface = UUID.fromString("c11fd5e6-f7cb-43bc-ab32-b043f1358fd8")
      preferences.edit {
         it[GlobalPreferenceKeys.waitForWatchfaceBeforeOpening] = true
      }
      pebbleInfoRetriever.setActiveApp(WatchIdentifier("TheWatch"), Watchapp(watchface, "Watchface", Watchapp.Type.WATCHFACE))

      controller.openWatchapp()

      pebbleSender.startedApps.shouldContainExactly(
         FakePebbleSender.AppLifecycleEvent(WATCHAPP_UUID, listOf(WatchIdentifier("TheWatch")))
      )
   }

   @Test
   fun `Close the watchapp when no open call was made`() = scope.runTest {
      controller.closeWatchappToTheLastApp(WatchIdentifier("TheWatch"))

      pebbleSender.startedApps.shouldBeEmpty()
      pebbleSender.stoppedApps.shouldContainExactly(
         FakePebbleSender.AppLifecycleEvent(WATCHAPP_UUID, listOf(WatchIdentifier("TheWatch")))
      )
   }

   @Test
   fun `Close the watchapp when open call was made with unknown watchapp`() = scope.runTest {
      pebbleInfoRetriever.setActiveApp(WatchIdentifier("TheWatch"), null)

      controller.openWatchapp()
      pebbleSender.startedApps.clear()

      controller.closeWatchappToTheLastApp(WatchIdentifier("TheWatch"))

      pebbleSender.startedApps.shouldBeEmpty()
      pebbleSender.stoppedApps.shouldContainExactly(
         FakePebbleSender.AppLifecycleEvent(WATCHAPP_UUID, listOf(WatchIdentifier("TheWatch")))
      )
   }

   @Test
   fun `Open the previous app when it is known from the previous open call`() = scope.runTest {
      val otherApp = UUID.fromString("caf5e298-d9e7-44a9-9177-d5ed6acb719a")
      pebbleInfoRetriever.setActiveApp(WatchIdentifier("TheWatch"), Watchapp(otherApp, "Important app", Watchapp.Type.WATCHAPP))

      controller.openWatchapp()
      pebbleSender.startedApps.clear()

      controller.closeWatchappToTheLastApp(WatchIdentifier("TheWatch"))

      pebbleSender.startedApps.shouldContainExactly(
         FakePebbleSender.AppLifecycleEvent(otherApp, listOf(WatchIdentifier("TheWatch")))
      )
      pebbleSender.stoppedApps.shouldBeEmpty()
   }

   @Test
   fun `Do not update the value when the previously open app is the notification center`() = scope.runTest {
      val otherApp = UUID.fromString("caf5e298-d9e7-44a9-9177-d5ed6acb719a")
      pebbleInfoRetriever.setActiveApp(WatchIdentifier("TheWatch"), Watchapp(otherApp, "Important app", Watchapp.Type.WATCHAPP))
      controller.openWatchapp()
      pebbleSender.startedApps.clear()

      pebbleInfoRetriever.setActiveApp(
         WatchIdentifier("TheWatch"),
         Watchapp(WATCHAPP_UUID, "Notification Center", Watchapp.Type.WATCHAPP)
      )
      controller.openWatchapp()
      pebbleSender.startedApps.clear()

      controller.closeWatchappToTheLastApp(WatchIdentifier("TheWatch"))

      pebbleSender.startedApps.shouldContainExactly(
         FakePebbleSender.AppLifecycleEvent(otherApp, listOf(WatchIdentifier("TheWatch")))
      )
      pebbleSender.stoppedApps.shouldBeEmpty()
   }
}
