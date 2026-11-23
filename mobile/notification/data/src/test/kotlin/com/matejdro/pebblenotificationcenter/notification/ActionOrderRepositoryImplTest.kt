package com.matejdro.pebblenotificationcenter.notification

import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import app.cash.turbine.test
import com.matejdro.pebblenotificationcenter.common.test.InMemoryDataStore
import com.matejdro.pebblenotificationcenter.notification.model.Action
import com.matejdro.pebblenotificationcenter.rules.GlobalPreferenceKeys
import com.matejdro.pebblenotificationcenter.rules.keys.set
import io.kotest.matchers.shouldBe
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.Test
import si.inova.kotlinova.core.test.fakes.FakeActivity

class ActionOrderRepositoryImplTest {
   private val context = FakeActivity()
   private val preferenceStore = InMemoryDataStore(emptyPreferences())
   private lateinit var repo: ActionOrderRepositoryImpl

   @BeforeEach
   fun setUp() {
      context.resources.putString(R.string.reply, "Reply")
      context.resources.putString(R.string.dismiss, "Dismiss")
      context.resources.putString(R.string.mark_as_read, "Mark as read")
      context.resources.putString(R.string.open_on_phone, "Open on phone")
      context.resources.putString(R.string.other_actions, "Other actions")
      context.resources.putString(R.string.snooze, "Snooze")
      context.resources.putString(R.string.show_image, "Show image")

      repo = ActionOrderRepositoryImpl(context, preferenceStore)
   }

   @Test
   fun `Start with default actions`() = runTest {
      repo.getList().test {
         runCurrent()

         expectMostRecentItem() shouldBe DEFAULT
      }
   }

   @Test
   fun `Add missing default actions at the end`() = runTest {
      preferenceStore.edit { prefs ->
         prefs[GlobalPreferenceKeys.actionOrder] = listOf("Other actions", "Show image")
      }

      repo.getList().test {
         runCurrent()

         expectMostRecentItem() shouldBe listOf(
            "Other actions",
            "Show image",
            "Reply",
            "Dismiss",
            "Mark as read",
            "Open on phone",
            "Snooze",
         )
      }
   }

   @Test
   fun `Do not remove non-default entries after adding default ones`() = runTest {
      preferenceStore.edit { prefs ->
         prefs[GlobalPreferenceKeys.actionOrder] = listOf("Archive", "Other actions")
      }

      repo.getList().test {
         runCurrent()

         expectMostRecentItem() shouldBe listOf(
            "Archive",
            "Other actions",
            "Reply",
            "Dismiss",
            "Mark as read",
            "Open on phone",
            "Snooze",
            "Show image",
         )
      }
   }

   @Test
   fun `Reorder list items`() = runTest {
      repo.getList().test {
         runCurrent()
         repo.moveOrder("Snooze", 0)
         runCurrent()

         expectMostRecentItem() shouldBe listOf("Snooze") + (DEFAULT - "Snooze")
      }
   }

   @Test
   fun `Sort provided list according to the default order`() = runTest {
      val inputList = listOf(
         Action.Dismiss("Snooze", 0u),
         Action.Dismiss("Open on phone", 1u),
         Action.Dismiss("Archive", 2u),
         Action.Dismiss("Mark as read", 3u),
         Action.Dismiss("Dismiss", 4u),
         Action.Dismiss("Reply", 5u),
      )

      repo.sort(inputList) shouldBe listOf(
         Action.Dismiss("Reply", 5u),
         Action.Dismiss("Dismiss", 4u),
         Action.Dismiss("Mark as read", 3u),
         Action.Dismiss("Open on phone", 1u),
         Action.Dismiss("Archive", 2u),
         Action.Dismiss("Snooze", 0u),
      )
   }

   @Test
   fun `Reply actions go first whatever the app calls them`() = runTest {
      val reply = Action.Reply("Respond", Any(), "key", emptyList(), true, 2u)
      val inputList = listOf(
         Action.Dismiss("Dismiss", 0u),
         Action.Dismiss("Snooze", 1u),
         reply,
      )

      repo.sort(inputList) shouldBe listOf(
         reply,
         Action.Dismiss("Dismiss", 0u),
         Action.Dismiss("Snooze", 1u),
      )
   }

   @Test
   fun `Sort provided list according to the stored action order`() = runTest {
      repo.getList().test {
         runCurrent()
         cancelAndIgnoreRemainingEvents()
      }

      repo.moveOrder("Dismiss", 6)

      val inputList = listOf(
         Action.Dismiss("Snooze", 0u),
         Action.Dismiss("Dismiss", 1u),
         Action.Dismiss("Reply", 2u),
      )

      repo.sort(inputList) shouldBe listOf(
         Action.Dismiss("Reply", 2u),
         Action.Dismiss("Snooze", 0u),
         Action.Dismiss("Dismiss", 1u),
      )
   }

   @Test
   fun `Unknown actions should be added to the list after the 'Other Actions' entry`() = runTest {
      repo.sort(listOf(Action.Dismiss("Archive", 0u)))

      repo.getList().test {
         runCurrent()

         expectMostRecentItem() shouldBe listOf(
            "Reply",
            "Dismiss",
            "Mark as read",
            "Open on phone",
            "Other actions",
            "Archive",
            "Snooze",
            "Show image",
         )
      }
   }

   companion object {
      private val DEFAULT = listOf(
         "Reply",
         "Dismiss",
         "Mark as read",
         "Open on phone",
         "Other actions",
         "Snooze",
         "Show image",
      )
   }
}
