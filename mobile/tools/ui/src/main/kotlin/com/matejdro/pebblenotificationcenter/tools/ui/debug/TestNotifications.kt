package com.matejdro.pebblenotificationcenter.tools.ui.debug

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.Person
import androidx.core.app.RemoteInput
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import com.matejdro.pebblenotificationcenter.notification.NotificationConstants
import logcat.logcat
import java.util.concurrent.atomic.AtomicInteger

/** Kinds of notification the Tools page can send, each exercising a different part of the watch app. */
enum class TestType(val label: String) {
   CHAT("Chat message"),
   GROUP_CHAT("Group chat"),
   PHOTO("Photo"),
   EMAIL("Email"),
   LONG_TEXT("Long text"),
   CALENDAR("Calendar reminder"),
   SILENT("Silent"),
   STACK("Stack of 3"),
   EDITED("Edited later"),
   PROGRESS("Download progress"),
}

/** Which app the watch should think a test notification is from, for its icon and colour. */
enum class TestAppLook(val label: String, val appName: String?) {
   OWN("Notification Sync", null),
   WHATSAPP("WhatsApp", "WhatsApp"),
   MESSAGES("Messages", "Messages"),
   TELEGRAM("Telegram", "Telegram"),
   SIGNAL("Signal", "Signal"),
   DISCORD("Discord", "Discord"),
   SLACK("Slack", "Slack"),
   GMAIL("Gmail", "Gmail"),
   OUTLOOK("Outlook", "Outlook"),
   CALENDAR("Calendar", "Calendar"),
   INSTAGRAM("Instagram", "Instagram"),
   YOUTUBE("YouTube", "YouTube"),
}

/**
 * Posts realistic notifications from this app straight to the phone, so the watch receives them the normal way.
 * Their actions really work (reply, mark as read, archive, delete...) and every step is written to the log, which
 * Tools → Save logs collects.
 */
object TestNotifications {
   private const val TAG = "TestNotifications"
   private const val CHANNEL_ALERTS = "TEST_ALERTS"
   private const val CHANNEL_SILENT = "TEST_SILENT"
   private const val FIRST_ID = 7000
   private const val STACK_SIZE = 3
   private const val PROGRESS_STEPS = 10
   private const val PROGRESS_STEP_MS = 1500L
   private const val EDIT_DELAY_MS = 6000L
   private const val ALL_SPACING_MS = 2500L

   internal const val ACTION = "com.matejdro.pebblenotificationcenter.TEST_ACTION"
   internal const val EXTRA_ID = "id"
   internal const val EXTRA_ACTION = "action"
   internal const val EXTRA_LOOK = "look"
   internal const val EXTRA_TYPE = "type"
   internal const val REPLY_KEY = "reply"

   private val nextId = AtomicInteger(FIRST_ID)
   private val handler = Handler(Looper.getMainLooper())
   private val posted = mutableSetOf<Int>()

   fun canPost(context: Context): Boolean = NotificationManagerCompat.from(context).areNotificationsEnabled()

   fun send(context: Context, type: TestType, look: TestAppLook, delaySeconds: Int) {
      val appContext = context.applicationContext
      logcat(TAG) { "Scheduled ${type.name} as ${look.label} in ${delaySeconds}s" }
      handler.postDelayed({ post(appContext, type, look) }, delaySeconds * 1000L)
   }

   fun sendAll(context: Context, look: TestAppLook, delaySeconds: Int) {
      val appContext = context.applicationContext
      logcat(TAG) { "Scheduled one of each as ${look.label} in ${delaySeconds}s" }
      TestType.entries.forEachIndexed { index, type ->
         handler.postDelayed({ post(appContext, type, look) }, delaySeconds * 1000L + index * ALL_SPACING_MS)
      }
   }

   fun clearAll(context: Context) {
      handler.removeCallbacksAndMessages(null)
      val manager = NotificationManagerCompat.from(context)
      synchronized(posted) {
         posted.forEach { manager.cancel(it) }
         posted.clear()
      }
      logcat(TAG) { "Cleared all test notifications" }
   }

   @Suppress("CyclomaticComplexMethod")
   private fun post(context: Context, type: TestType, look: TestAppLook) {
      createChannels(context)
      val id = nextId.getAndIncrement()
      logcat(TAG) { "Posting #$id ${type.name} as ${look.label}" }
      when (type) {
         TestType.CHAT -> notify(context, id, chat(context, id, look, group = false))
         TestType.GROUP_CHAT -> notify(context, id, chat(context, id, look, group = true))
         TestType.PHOTO -> notify(context, id, photo(context, id, look))
         TestType.EMAIL -> notify(context, id, email(context, id, look))
         TestType.LONG_TEXT -> notify(context, id, longText(context, id, look))
         TestType.CALENDAR -> notify(context, id, calendar(context, id, look))
         TestType.SILENT -> notify(context, id, silent(context, id, look))
         TestType.STACK -> stack(context, id, look)
         TestType.EDITED -> edited(context, id, look)
         TestType.PROGRESS -> progress(context, id, look, 0)
      }
   }

   private fun createChannels(context: Context) {
      if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
      val manager = context.getSystemService(NotificationManager::class.java)
      manager.createNotificationChannel(
         NotificationChannel(CHANNEL_ALERTS, "Test notifications", NotificationManager.IMPORTANCE_HIGH).apply {
            description = "Sent from Tools to try the watch app"
         }
      )
      manager.createNotificationChannel(
         NotificationChannel(CHANNEL_SILENT, "Silent test notifications", NotificationManager.IMPORTANCE_LOW).apply {
            description = "Quiet notifications sent from Tools"
         }
      )
   }

   @Suppress("MissingPermission") // canPost() is checked by the screen before sending
   private fun notify(context: Context, id: Int, builder: NotificationCompat.Builder) {
      try {
         NotificationManagerCompat.from(context).notify(id, builder.build())
         synchronized(posted) { posted += id }
      } catch (e: SecurityException) {
         logcat(TAG) { "Could not post #$id: ${e.message}" }
      }
   }

   private fun base(
      context: Context,
      id: Int,
      look: TestAppLook,
      type: TestType,
      channel: String = CHANNEL_ALERTS,
   ): NotificationCompat.Builder {
      return NotificationCompat.Builder(context, channel)
         .setSmallIcon(IconCompat.createWithResource(context, android.R.drawable.stat_notify_chat))
         .setAutoCancel(true)
         .setContentIntent(actionIntent(context, id, look, type, "open"))
         .setDeleteIntent(actionIntent(context, id, look, type, "dismissed"))
         .addExtras(
            Bundle().apply {
               look.appName?.let { putString(NotificationConstants.KEY_TEST_APP_NAME, it) }
            }
         )
   }

   private fun chat(context: Context, id: Int, look: TestAppLook, group: Boolean): NotificationCompat.Builder {
      val me = Person.Builder().setName("You").setKey("me").build()
      val alex = Person.Builder().setName("Alex Rivera").setKey("alex").setImportant(true).build()
      val sam = Person.Builder().setName("Sam").setKey("sam").build()
      val now = System.currentTimeMillis()
      val style = NotificationCompat.MessagingStyle(me)
      if (group) {
         style.setConversationTitle("Weekend hike 🥾").setGroupConversation(true)
         style.addMessage("Who's in for Saturday?", now - 120_000, alex)
         style.addMessage("Me! What time?", now - 60_000, sam)
         style.addMessage("8am at the trailhead, bring water 💧", now, alex)
      } else {
         style.addMessage("Hey, are you around?", now - 60_000, alex)
         style.addMessage("Want to grab lunch at 12:30? 🍜", now, alex)
      }
      val shortcutId = publishShortcut(context, look, if (group) "group" else "chat", if (group) "Weekend hike" else "Alex Rivera")
      return base(context, id, look, if (group) TestType.GROUP_CHAT else TestType.CHAT)
         .setStyle(style)
         .setCategory(NotificationCompat.CATEGORY_MESSAGE)
         .setShortcutId(shortcutId)
         .addAction(replyAction(context, id, look, if (group) TestType.GROUP_CHAT else TestType.CHAT))
         .addAction(simpleAction(context, id, look, TestType.CHAT, "Mark as read", "read"))
         .addAction(simpleAction(context, id, look, TestType.CHAT, "👍", "like"))
   }

   /** A conversation shortcut is what makes Android put a chat in the Conversations section. */
   private fun publishShortcut(context: Context, look: TestAppLook, kind: String, name: String): String {
      val id = "test_${kind}_${look.name.lowercase()}"
      val launch = context.packageManager.getLaunchIntentForPackage(context.packageName)
         ?: Intent(Intent.ACTION_MAIN).setPackage(context.packageName)
      try {
         ShortcutManagerCompat.pushDynamicShortcut(
            context,
            ShortcutInfoCompat.Builder(context, id)
               .setShortLabel(name)
               .setLongLived(true)
               .setPerson(Person.Builder().setName(name).setKey(id).build())
               .setIntent(launch.setAction(Intent.ACTION_MAIN))
               .build()
         )
      } catch (e: IllegalArgumentException) {
         logcat(TAG) { "Shortcut not published: ${e.message}" }
      } catch (e: IllegalStateException) {
         logcat(TAG) { "Shortcut not published: ${e.message}" }
      }
      return id
   }

   private fun photo(context: Context, id: Int, look: TestAppLook): NotificationCompat.Builder {
      val picture = samplePicture()
      return base(context, id, look, TestType.PHOTO)
         .setContentTitle("Jordan")
         .setContentText("📷 Sent a photo: sunset from the pier")
         .setLargeIcon(picture)
         .setStyle(NotificationCompat.BigPictureStyle().bigPicture(picture).bigLargeIcon(null as Bitmap?))
         .setCategory(NotificationCompat.CATEGORY_SOCIAL)
         .addAction(replyAction(context, id, look, TestType.PHOTO))
         .addAction(simpleAction(context, id, look, TestType.PHOTO, "❤️ Love", "love"))
   }

   private fun email(context: Context, id: Int, look: TestAppLook): NotificationCompat.Builder {
      return base(context, id, look, TestType.EMAIL)
         .setContentTitle("Taylor from Acme Co.")
         .setContentText("Quarterly report draft")
         .setSubText("work@example.com")
         .setStyle(
            NotificationCompat.BigTextStyle()
               .setBigContentTitle("Taylor from Acme Co.")
               .bigText(
                  "Quarterly report draft\nHi! I've attached the draft of the quarterly report. Could you take a " +
                     "look at the numbers on page 3 before Friday? Thanks!"
               )
         )
         .setCategory(NotificationCompat.CATEGORY_EMAIL)
         .addAction(simpleAction(context, id, look, TestType.EMAIL, "Archive", "archive"))
         .addAction(replyAction(context, id, look, TestType.EMAIL))
         .addAction(simpleAction(context, id, look, TestType.EMAIL, "Delete", "delete"))
   }

   private fun longText(context: Context, id: Int, look: TestAppLook): NotificationCompat.Builder {
      val text = (1..8).joinToString("\n\n") { paragraph ->
         "Paragraph $paragraph. This is a long notification to test scrolling on the watch. " +
            "It keeps going so you can check that the text wraps, scrolls smoothly and loads the rest " +
            "when you reach the end. Emoji too: ☀️🌧️⚡"
      }
      return base(context, id, look, TestType.LONG_TEXT)
         .setContentTitle("Long read")
         .setContentText(text.take(80))
         .setStyle(NotificationCompat.BigTextStyle().bigText(text))
         .addAction(simpleAction(context, id, look, TestType.LONG_TEXT, "Mark as read", "read"))
   }

   private fun calendar(context: Context, id: Int, look: TestAppLook): NotificationCompat.Builder {
      return base(context, id, look, TestType.CALENDAR)
         .setSmallIcon(IconCompat.createWithResource(context, android.R.drawable.ic_menu_my_calendar))
         .setContentTitle("Dentist appointment")
         .setContentText("In 15 minutes · 3:00 – 3:45 PM · 12 Main St")
         .setCategory(NotificationCompat.CATEGORY_EVENT)
         .addAction(simpleAction(context, id, look, TestType.CALENDAR, "Snooze 5 min", "snooze"))
         .addAction(simpleAction(context, id, look, TestType.CALENDAR, "Dismiss", "dismiss"))
   }

   private fun silent(context: Context, id: Int, look: TestAppLook): NotificationCompat.Builder {
      return base(context, id, look, TestType.SILENT, CHANNEL_SILENT)
         .setContentTitle("Weekly summary")
         .setContentText("You walked 42,000 steps this week. A silent notification, so it shouldn't buzz.")
         .setPriority(NotificationCompat.PRIORITY_LOW)
         .addAction(simpleAction(context, id, look, TestType.SILENT, "Mark as read", "read"))
   }

   private fun stack(context: Context, firstId: Int, look: TestAppLook) {
      val group = "test_stack_${look.name}_$firstId"
      val senders = listOf("Mia" to "Running 5 min late", "Noah" to "Got the tickets 🎟️", "Ava" to "Call me when free")
      senders.take(STACK_SIZE).forEachIndexed { index, (name, text) ->
         val id = if (index == 0) firstId else nextId.getAndIncrement()
         notify(
            context,
            id,
            base(context, id, look, TestType.STACK)
               .setContentTitle(name)
               .setContentText(text)
               .setGroup(group)
               .setCategory(NotificationCompat.CATEGORY_MESSAGE)
               .addAction(replyAction(context, id, look, TestType.STACK))
               .addAction(simpleAction(context, id, look, TestType.STACK, "Mark as read", "read"))
         )
      }
      val summaryId = nextId.getAndIncrement()
      val inbox = NotificationCompat.InboxStyle().setSummaryText("$STACK_SIZE new messages")
      senders.forEach { (name, text) -> inbox.addLine("$name: $text") }
      notify(
         context,
         summaryId,
         base(context, summaryId, look, TestType.STACK)
            .setContentTitle("$STACK_SIZE new messages")
            .setContentText(senders.joinToString { it.first })
            .setStyle(inbox)
            .setGroup(group)
            .setGroupSummary(true)
      )
   }

   private fun edited(context: Context, id: Int, look: TestAppLook) {
      val builder = base(context, id, look, TestType.EDITED)
         .setContentTitle("Chris")
         .setContentText("Meeting moved to 3pm")
         .addAction(simpleAction(context, id, look, TestType.EDITED, "Mark as read", "read"))
      notify(context, id, builder)
      handler.postDelayed(
         {
            if (synchronized(posted) { id in posted }) {
               logcat(TAG) { "Editing #$id" }
               notify(context, id, builder.setContentText("Meeting moved to 3:30pm (edited)").setOnlyAlertOnce(true))
            }
         },
         EDIT_DELAY_MS
      )
   }

   private fun progress(context: Context, id: Int, look: TestAppLook, step: Int) {
      if (step > 0 && synchronized(posted) { id !in posted }) {
         return
      }
      val done = step >= PROGRESS_STEPS
      val builder = base(context, id, look, TestType.PROGRESS, CHANNEL_SILENT)
         .setSmallIcon(
            IconCompat.createWithResource(
               context,
               if (done) android.R.drawable.stat_sys_download_done else android.R.drawable.stat_sys_download
            )
         )
         .setContentTitle(if (done) "Download complete" else "Downloading holiday-photos.zip")
         .setContentText(if (done) "holiday-photos.zip · 48 MB" else "${step * 100 / PROGRESS_STEPS}%")
         .setOnlyAlertOnce(true)
         .setOngoing(!done)
      if (!done) {
         builder.setProgress(PROGRESS_STEPS, step, false)
            .addAction(simpleAction(context, id, look, TestType.PROGRESS, "Cancel", "cancel"))
      }
      notify(context, id, builder)
      if (!done) {
         handler.postDelayed({ progress(context, id, look, step + 1) }, PROGRESS_STEP_MS)
      }
   }

   private fun replyAction(context: Context, id: Int, look: TestAppLook, type: TestType): NotificationCompat.Action {
      val remoteInput = RemoteInput.Builder(REPLY_KEY)
         .setLabel("Reply")
         .setChoices(arrayOf("On my way", "Sounds good 👍", "Can't talk now"))
         .build()
      return NotificationCompat.Action.Builder(
         IconCompat.createWithResource(context, android.R.drawable.ic_menu_send),
         "Reply",
         actionIntent(context, id, look, type, "reply", mutable = true)
      )
         .addRemoteInput(remoteInput)
         .setSemanticAction(NotificationCompat.Action.SEMANTIC_ACTION_REPLY)
         .setShowsUserInterface(false)
         .build()
   }

   private fun simpleAction(
      context: Context,
      id: Int,
      look: TestAppLook,
      type: TestType,
      title: String,
      action: String,
   ): NotificationCompat.Action {
      val semantic = when (action) {
         "read" -> NotificationCompat.Action.SEMANTIC_ACTION_MARK_AS_READ
         "delete" -> NotificationCompat.Action.SEMANTIC_ACTION_DELETE
         "archive" -> NotificationCompat.Action.SEMANTIC_ACTION_ARCHIVE
         "like", "love" -> NotificationCompat.Action.SEMANTIC_ACTION_THUMBS_UP
         else -> NotificationCompat.Action.SEMANTIC_ACTION_NONE
      }
      return NotificationCompat.Action.Builder(null as IconCompat?, title, actionIntent(context, id, look, type, action))
         .setSemanticAction(semantic)
         .setShowsUserInterface(false)
         .build()
   }

   private fun actionIntent(
      context: Context,
      id: Int,
      look: TestAppLook,
      type: TestType,
      action: String,
      mutable: Boolean = false,
   ): PendingIntent {
      val intent = Intent(context, TestNotificationReceiver::class.java)
         .setAction("$ACTION.$action.$id")
         .putExtra(EXTRA_ID, id)
         .putExtra(EXTRA_ACTION, action)
         .putExtra(EXTRA_LOOK, look.name)
         .putExtra(EXTRA_TYPE, type.name)
      val flags = PendingIntent.FLAG_UPDATE_CURRENT or
         if (mutable && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            PendingIntent.FLAG_MUTABLE
         } else {
            PendingIntent.FLAG_IMMUTABLE
         }
      return PendingIntent.getBroadcast(context, id, intent, flags)
   }

   /** Behaves like a normal app would: replies are added to the conversation, read/archive/delete remove it. */
   internal fun handleAction(context: Context, intent: Intent) {
      val id = intent.getIntExtra(EXTRA_ID, -1)
      val action = intent.getStringExtra(EXTRA_ACTION).orEmpty()
      val look = intent.getStringExtra(EXTRA_LOOK)?.let { runCatching { TestAppLook.valueOf(it) }.getOrNull() }
         ?: TestAppLook.OWN
      val type = intent.getStringExtra(EXTRA_TYPE).orEmpty()
      val manager = NotificationManagerCompat.from(context)

      when (action) {
         "reply" -> {
            val reply = RemoteInput.getResultsFromIntent(intent)?.getCharSequence(REPLY_KEY)
            logcat(TAG) { "Action on #$id ($type): reply \"$reply\"" }
            val me = Person.Builder().setName("You").setKey("me").build()
            val style = NotificationCompat.MessagingStyle(me)
               .addMessage(reply ?: "(empty reply)", System.currentTimeMillis(), null as Person?)
            notify(
               context,
               id,
               base(context, id, look, TestType.CHAT)
                  .setStyle(style)
                  .setContentText("You: $reply")
                  .setOnlyAlertOnce(true)
                  .setTimeoutAfter(REPLY_SHOWN_MS)
            )
         }
         "open" -> {
            logcat(TAG) { "Action on #$id ($type): opened from the notification" }
            context.packageManager.getLaunchIntentForPackage(context.packageName)
               ?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
               ?.let { runCatching { context.startActivity(it) } }
            synchronized(posted) { posted -= id }
         }
         "dismissed" -> {
            logcat(TAG) { "#$id ($type) was dismissed (on the phone or from the watch)" }
            synchronized(posted) { posted -= id }
         }
         "snooze" -> {
            logcat(TAG) { "Action on #$id ($type): snooze 5 min" }
            manager.cancel(id)
            handler.postDelayed({ post(context.applicationContext, TestType.CALENDAR, look) }, SNOOZE_MS)
         }
         else -> {
            logcat(TAG) { "Action on #$id ($type): $action" }
            manager.cancel(id)
            synchronized(posted) { posted -= id }
         }
      }
   }

   private fun samplePicture(): Bitmap {
      val width = 720
      val height = 480
      val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
      val canvas = Canvas(bitmap)
      val paint = Paint(Paint.ANTI_ALIAS_FLAG)
      paint.shader = LinearGradient(
         0f, 0f, 0f, height.toFloat(),
         intArrayOf(0xFF2B1B5A.toInt(), 0xFFE0577B.toInt(), 0xFFFFB347.toInt()),
         floatArrayOf(0f, 0.55f, 1f),
         Shader.TileMode.CLAMP
      )
      canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
      paint.shader = null
      paint.color = 0xFFFFE08A.toInt()
      canvas.drawCircle(width / 2f, height * 0.62f, 90f, paint)
      paint.color = 0xFF1B2A49.toInt()
      canvas.drawRect(0f, height * 0.7f, width.toFloat(), height.toFloat(), paint)
      paint.color = 0xFF0E1628.toInt()
      canvas.drawRect(width * 0.1f, height * 0.6f, width * 0.14f, height.toFloat(), paint)
      canvas.drawRect(0f, height * 0.78f, width * 0.55f, height * 0.8f, paint)
      return bitmap
   }

   private const val REPLY_SHOWN_MS = 30_000L
   private const val SNOOZE_MS = 5 * 60_000L
}

class TestNotificationReceiver : BroadcastReceiver() {
   override fun onReceive(context: Context, intent: Intent) {
      TestNotifications.handleAction(context, intent)
   }
}
