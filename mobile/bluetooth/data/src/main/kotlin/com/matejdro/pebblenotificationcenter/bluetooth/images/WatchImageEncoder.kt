@file:Suppress("MagicNumber") // Image format constants

package com.matejdro.pebblenotificationcenter.bluetooth.images

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect

/**
 * A notification photo in the format PebbleOS's notification image band uses (ImagingFormat4BitPalette): up to
 * 16 palette entries as GColor8 bytes, and 4-bit indices packed two per byte, rows of (width + 1) / 2 bytes, even x
 * in the high nibble.
 */
class WatchImage(
   val width: Int,
   val height: Int,
   val palette: ByteArray,
   val pixels: ByteArray,
)

/** Centre-crops and scales [this] to exactly [width] x [height], then encodes it for the watch. */
fun Bitmap.encodeForWatchBand(width: Int, height: Int): WatchImage {
   val scaled = centerCrop(width, height)
   val argb = IntArray(width * height)
   scaled.getPixels(argb, 0, width, 0, 0, width, height)
   if (scaled !== this) {
      scaled.recycle()
   }
   return WatchImageEncoder.encode(argb, width, height)
}

private fun Bitmap.centerCrop(width: Int, height: Int): Bitmap {
   val output = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
   val canvas = Canvas(output)
   // Photos with transparency (stickers, screenshots of icons) sit on white, like the card behind them.
   canvas.drawColor(Color.WHITE)

   val source = if (this.width.toLong() * height > this.height.toLong() * width) {
      val croppedWidth = (this.height.toLong() * width / height).toInt()
      val left = (this.width - croppedWidth) / 2
      Rect(left, 0, left + croppedWidth, this.height)
   } else {
      val croppedHeight = (this.width.toLong() * height / width).toInt()
      val top = (this.height - croppedHeight) / 2
      Rect(0, top, this.width, top + croppedHeight)
   }
   canvas.drawBitmap(this, source, Rect(0, 0, width, height), Paint(Paint.FILTER_BITMAP_FLAG))
   return output
}

/**
 * Picks the 16 Pebble colours that best cover the image (median cut over the 64 colours the screen can show, weighted
 * by how many pixels land on each) and Floyd–Steinberg dithers the image to them.
 */
internal object WatchImageEncoder {
   private const val MAX_COLORS = 16
   private const val CHANNEL_STEP = 85

   fun encode(argb: IntArray, width: Int, height: Int): WatchImage {
      val palette = choosePalette(argb)
      val paletteRed = IntArray(palette.size) { component(palette[it], 4) }
      val paletteGreen = IntArray(palette.size) { component(palette[it], 2) }
      val paletteBlue = IntArray(palette.size) { component(palette[it], 0) }

      val stride = (width + 1) / 2
      val pixels = ByteArray(stride * height)
      var errors = IntArray((width + 2) * 3)
      var nextErrors = IntArray((width + 2) * 3)

      for (y in 0 until height) {
         for (x in 0 until width) {
            val pixel = argb[y * width + x]
            // Errors are stored x16 so the 7/3/5/1 sixteenths stay exact.
            val e = (x + 1) * 3
            val red = (Color.red(pixel) + errors[e] / 16).coerceIn(0, 255)
            val green = (Color.green(pixel) + errors[e + 1] / 16).coerceIn(0, 255)
            val blue = (Color.blue(pixel) + errors[e + 2] / 16).coerceIn(0, 255)

            val index = nearest(red, green, blue, paletteRed, paletteGreen, paletteBlue)
            val byteIndex = y * stride + x / 2
            pixels[byteIndex] = if (x % 2 == 0) {
               (pixels[byteIndex].toInt() or (index shl 4)).toByte()
            } else {
               (pixels[byteIndex].toInt() or index).toByte()
            }

            val errorRed = red - paletteRed[index]
            val errorGreen = green - paletteGreen[index]
            val errorBlue = blue - paletteBlue[index]
            spread(errors, x + 2, errorRed, errorGreen, errorBlue, 7)
            spread(nextErrors, x, errorRed, errorGreen, errorBlue, 3)
            spread(nextErrors, x + 1, errorRed, errorGreen, errorBlue, 5)
            spread(nextErrors, x + 2, errorRed, errorGreen, errorBlue, 1)
         }
         val done = errors
         errors = nextErrors
         nextErrors = done
         nextErrors.fill(0)
      }

      val paletteBytes = ByteArray(palette.size) { (0xC0 or palette[it]).toByte() }
      return WatchImage(width, height, paletteBytes, pixels)
   }

   private fun spread(errors: IntArray, slot: Int, red: Int, green: Int, blue: Int, weight: Int) {
      val i = slot * 3
      errors[i] += red * weight
      errors[i + 1] += green * weight
      errors[i + 2] += blue * weight
   }

   /** 6-bit Pebble colour code (rrggbb) nearest to an sRGB colour. */
   private fun code(red: Int, green: Int, blue: Int): Int {
      val r = (red + CHANNEL_STEP / 2) / CHANNEL_STEP
      val g = (green + CHANNEL_STEP / 2) / CHANNEL_STEP
      val b = (blue + CHANNEL_STEP / 2) / CHANNEL_STEP
      return (r shl 4) or (g shl 2) or b
   }

   private fun component(code: Int, shift: Int): Int = ((code shr shift) and 0x03) * CHANNEL_STEP

   private fun nearest(red: Int, green: Int, blue: Int, pr: IntArray, pg: IntArray, pb: IntArray): Int {
      var best = 0
      var bestDistance = Int.MAX_VALUE
      for (i in pr.indices) {
         val dr = red - pr[i]
         val dg = green - pg[i]
         val db = blue - pb[i]
         // Weighted towards green, the way the eye is.
         val distance = 2 * dr * dr + 4 * dg * dg + 3 * db * db
         if (distance < bestDistance) {
            bestDistance = distance
            best = i
         }
      }
      return best
   }

   private class Box(val codes: MutableList<Int>, val counts: IntArray) {
      val population: Long get() = codes.sumOf { counts[it].toLong() }

      fun range(shift: Int): Int {
         val values = codes.map { (it shr shift) and 0x03 }
         return values.max() - values.min()
      }

      fun widestShift(): Int = listOf(4, 2, 0).maxBy { range(it) }
   }

   private fun choosePalette(argb: IntArray): IntArray {
      val counts = IntArray(64)
      for (pixel in argb) {
         counts[code(Color.red(pixel), Color.green(pixel), Color.blue(pixel))]++
      }
      val used = (0 until 64).filter { counts[it] > 0 }.toMutableList()
      if (used.size <= MAX_COLORS) {
         return used.toIntArray()
      }

      val boxes = mutableListOf(Box(used, counts))
      while (boxes.size < MAX_COLORS) {
         val splittable = boxes.filter { it.codes.size > 1 }
         if (splittable.isEmpty()) break
         val box = splittable.maxBy { it.population * (it.range(it.widestShift()) + 1) }
         val shift = box.widestShift()
         box.codes.sortBy { (it shr shift) and 0x03 }

         // Split where half of the box's pixels fall on each side.
         val half = box.population / 2
         var running = 0L
         var split = 1
         for (i in box.codes.indices) {
            running += counts[box.codes[i]]
            if (running >= half) {
               split = (i + 1).coerceIn(1, box.codes.size - 1)
               break
            }
         }
         boxes.remove(box)
         boxes += Box(box.codes.subList(0, split).toMutableList(), counts)
         boxes += Box(box.codes.subList(split, box.codes.size).toMutableList(), counts)
      }

      return boxes.map { box ->
         // The colour of the box is its pixel-weighted average, snapped back onto the screen's colours.
         var red = 0L
         var green = 0L
         var blue = 0L
         val total = box.population.coerceAtLeast(1)
         for (c in box.codes) {
            red += component(c, 4).toLong() * counts[c]
            green += component(c, 2).toLong() * counts[c]
            blue += component(c, 0).toLong() * counts[c]
         }
         code((red / total).toInt(), (green / total).toInt(), (blue / total).toInt())
      }.distinct().toIntArray()
   }
}
