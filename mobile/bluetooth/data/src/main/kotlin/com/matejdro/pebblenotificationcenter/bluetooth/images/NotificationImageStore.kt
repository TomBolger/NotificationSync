@file:Suppress("MagicNumber")

package com.matejdro.pebblenotificationcenter.bluetooth.images

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Icon
import android.media.ExifInterface
import android.os.Build
import com.matejdro.pebblenotificationcenter.notification.model.ParsedNotification
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dev.zacsweers.metro.SingleIn
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.withContext
import logcat.logcat
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap

/** What the watch needs to know about a notification's photo before it asks for the pixels. */
data class NotificationImageInfo(
   /** Height/width in sixteenths, clamped to what the watch will reserve. */
   val aspect: Int,
   /** Non-zero; changes when the photo does, so the watch knows to fetch it again. */
   val tag: Int,
)

/**
 * Photos attached to notifications (a BigPictureStyle picture or the image in a MessagingStyle message), kept on
 * disk so they can be served whenever the watch brings the card on screen, as PebbleOS's notification image band
 * expects. The watch pulls them long after the notification was posted, possibly after this process restarted.
 */
interface NotificationImageStore {
   /** Remember the photo of this notification (if any) and describe it for the watch. */
   suspend fun prepare(notification: ParsedNotification): NotificationImageInfo?

   /** The photo of the notification with this key, cropped and encoded to exactly [width] x [height]. */
   suspend fun encode(key: String, width: Int, height: Int): WatchImage?

   fun forget(key: String)

   fun forgetAll()
}

object NoNotificationImages : NotificationImageStore {
   override suspend fun prepare(notification: ParsedNotification): NotificationImageInfo? = null
   override suspend fun encode(key: String, width: Int, height: Int): WatchImage? = null
   override fun forget(key: String) = Unit
   override fun forgetAll() = Unit
}

@Inject
@SingleIn(AppScope::class)
@ContributesBinding(AppScope::class)
class NotificationImageStoreImpl(
   private val context: Context,
) : NotificationImageStore {
   private class Entry(
      val source: String,
      val info: NotificationImageInfo,
      val file: File,
      /** Completes (true when the file was written) once the photo is on disk. */
      val ready: Deferred<Boolean>,
   )

   private val entries = ConcurrentHashMap<String, Entry>()
   private val directory by lazy { File(context.cacheDir, DIRECTORY_NAME) }

   // Writing the photo to disk happens in the background: the watch only needs its shape now (for the card), and
   // the pixels when it brings that card on screen. Keeps the hello answer and new notifications fast.
   private val writerScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

   override suspend fun prepare(notification: ParsedNotification): NotificationImageInfo? {
      val icon = notification.largeImage as? Icon
      if (icon == null) {
         forget(notification.key)
         return null
      }

      val source = icon.sourceIdentity()
      entries[notification.key]?.takeIf { source != null && it.source == source && it.isUsable() }?.let {
         return it.info
      }

      return withContext(Dispatchers.IO) {
         try {
            if (icon.isUri()) prepareUri(notification.key, icon) else prepareBitmap(notification.key, icon)
         } catch (e: Exception) {
            // The listener only has read access to a message's image while it is posted, and the provider can
            // refuse anyway: no photo then, just the text.
            logcat { "Could not read the image of ${notification.key}: ${e.message}" }
            null
         }
      }
   }

   private fun prepareUri(key: String, icon: Icon): NotificationImageInfo? {
      val uri = icon.uri
      val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
      context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
      if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
         return null
      }
      val orientation = context.contentResolver.openInputStream(uri)?.use {
         ExifInterface(it).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
      } ?: ExifInterface.ORIENTATION_NORMAL
      val sideways = orientation in SIDEWAYS_ORIENTATIONS
      val width = if (sideways) bounds.outHeight else bounds.outWidth
      val height = if (sideways) bounds.outWidth else bounds.outHeight

      return remember(key, "uri:$uri", width, height) { decodeUri(icon, bounds, orientation) }
   }

   private fun prepareBitmap(key: String, icon: Icon): NotificationImageInfo? {
      val bitmap = icon.drawToBitmap() ?: return null
      // Bitmaps handed over in the notification itself have no identity of their own; tell them apart by
      // their pixels so a re-post with the same picture isn't sent to the watch again.
      val identity = "bitmap:${bitmap.width}x${bitmap.height}:${bitmap.sampleHash()}"
      entries[key]?.takeIf { it.source == identity && it.isUsable() }?.let { return it.info }
      return remember(key, identity, bitmap.width, bitmap.height) { bitmap }
   }

   private fun remember(
      key: String,
      identity: String,
      width: Int,
      height: Int,
      pixels: () -> Bitmap?,
   ): NotificationImageInfo {
      val info = NotificationImageInfo(
         aspect = aspectSixteenths(width, height),
         tag = (identity.hashCode() and 0xFF).coerceAtLeast(1),
      )
      val file = File(directory, key.sha1() + ".jpg")
      entries.remove(key)?.ready?.cancel()
      val ready = writerScope.async {
         try {
            val bitmap = pixels() ?: return@async false
            directory.mkdirs()
            file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, it) }
            logcat { "Cached ${bitmap.width}x${bitmap.height} image for $key" }
            true
         } catch (e: CancellationException) {
            throw e
         } catch (e: Exception) {
            logcat { "Could not store the image of $key: ${e.message}" }
            false
         }
      }
      entries[key] = Entry(identity, info, file, ready)
      prune()
      return info
   }

   private fun Entry.isUsable(): Boolean = !ready.isCompleted || file.exists()

   override suspend fun encode(key: String, width: Int, height: Int): WatchImage? {
      val entry = entries[key] ?: return null
      if (!entry.ready.await()) {
         return null
      }
      return withContext(Dispatchers.Default) {
         val bitmap = BitmapFactory.decodeFile(entry.file.path) ?: return@withContext null
         try {
            bitmap.encodeForWatchBand(width, height)
         } finally {
            bitmap.recycle()
         }
      }
   }

   override fun forget(key: String) {
      entries.remove(key)?.let {
         it.ready.cancel()
         it.file.delete()
      }
   }

   override fun forgetAll() {
      entries.values.forEach { it.ready.cancel() }
      entries.clear()
      directory.listFiles()?.forEach { it.delete() }
   }

   private fun prune() {
      if (entries.size <= MAX_CACHED) {
         return
      }
      entries.entries
         .sortedBy { it.value.file.lastModified() }
         .take(entries.size - MAX_CACHED)
         .forEach { forget(it.key) }
   }

   private fun Icon.sourceIdentity(): String? {
      return if (isUri()) "uri:$uri" else null
   }

   /** The photo behind a content URI, upright and no larger than [MAX_STORED_DIMENSION] on its longest side. */
   private fun decodeUri(icon: Icon, bounds: BitmapFactory.Options, orientation: Int): Bitmap? {
      var sample = 1
      while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= MAX_STORED_DIMENSION) {
         sample *= 2
      }
      val options = BitmapFactory.Options().apply { inSampleSize = sample }
      val bitmap = context.contentResolver.openInputStream(icon.uri)?.use {
         BitmapFactory.decodeStream(it, null, options)
      } ?: return null
      return bitmap.rotatedFor(orientation).boundedTo(MAX_STORED_DIMENSION)
   }

   /** A bitmap or drawable icon, no larger than [MAX_STORED_DIMENSION] on its longest side. */
   private fun Icon.drawToBitmap(): Bitmap? {
      val drawable = loadDrawable(context) ?: return null
      if (drawable is BitmapDrawable && drawable.bitmap != null) {
         return drawable.bitmap.boundedTo(MAX_STORED_DIMENSION)
      }
      val width = drawable.intrinsicWidth
      val height = drawable.intrinsicHeight
      if (width <= 0 || height <= 0) {
         return null
      }
      val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
      drawable.setBounds(0, 0, width, height)
      drawable.draw(Canvas(bitmap))
      return bitmap.boundedTo(MAX_STORED_DIMENSION)
   }
}

private val SIDEWAYS_ORIENTATIONS = setOf(
   ExifInterface.ORIENTATION_ROTATE_90,
   ExifInterface.ORIENTATION_ROTATE_270,
   ExifInterface.ORIENTATION_TRANSPOSE,
   ExifInterface.ORIENTATION_TRANSVERSE,
)

private fun Icon.isUri(): Boolean {
   return type == Icon.TYPE_URI ||
      (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && type == Icon.TYPE_URI_ADAPTIVE_BITMAP)
}

/** Height/width of a [width] x [height] image in sixteenths, clamped to the band sizes the watch supports. */
internal fun aspectSixteenths(width: Int, height: Int): Int {
   if (width <= 0 || height <= 0) {
      return MIN_ASPECT_SIXTEENTHS
   }
   return ((height * 16 + width / 2) / width).coerceIn(MIN_ASPECT_SIXTEENTHS, MAX_ASPECT_SIXTEENTHS)
}

private fun Bitmap.boundedTo(maxDimension: Int): Bitmap {
   val longest = maxOf(width, height)
   if (longest <= maxDimension) {
      return this
   }
   val scale = maxDimension.toFloat() / longest
   return Bitmap.createScaledBitmap(
      this,
      (width * scale).toInt().coerceAtLeast(1),
      (height * scale).toInt().coerceAtLeast(1),
      true
   )
}

private fun Bitmap.rotatedFor(orientation: Int): Bitmap {
   val matrix = Matrix()
   when (orientation) {
      ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.setScale(-1f, 1f)
      ExifInterface.ORIENTATION_ROTATE_180 -> matrix.setRotate(180f)
      ExifInterface.ORIENTATION_FLIP_VERTICAL -> matrix.setScale(1f, -1f)
      ExifInterface.ORIENTATION_TRANSPOSE -> {
         matrix.setRotate(90f)
         matrix.postScale(-1f, 1f)
      }
      ExifInterface.ORIENTATION_ROTATE_90 -> matrix.setRotate(90f)
      ExifInterface.ORIENTATION_TRANSVERSE -> {
         matrix.setRotate(-90f)
         matrix.postScale(-1f, 1f)
      }
      ExifInterface.ORIENTATION_ROTATE_270 -> matrix.setRotate(-90f)
      else -> return this
   }
   return Bitmap.createBitmap(this, 0, 0, width, height, matrix, true)
}

private fun Bitmap.sampleHash(): Int {
   var hash = 17
   val stepX = (width / 8).coerceAtLeast(1)
   val stepY = (height / 8).coerceAtLeast(1)
   var y = 0
   while (y < height) {
      var x = 0
      while (x < width) {
         hash = hash * 31 + getPixel(x, y)
         x += stepX
      }
      y += stepY
   }
   return hash
}

private fun String.sha1(): String {
   return MessageDigest.getInstance("SHA-1").digest(toByteArray()).joinToString("") { "%02x".format(it) }
}

internal const val MIN_ASPECT_SIXTEENTHS = 4
internal const val MAX_ASPECT_SIXTEENTHS = 24
private const val DIRECTORY_NAME = "notification-images"
private const val JPEG_QUALITY = 85
private const val MAX_CACHED = 32
private const val MAX_STORED_DIMENSION = 600
