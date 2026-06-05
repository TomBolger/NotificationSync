@file:Suppress("MagicNumber")

package com.matejdro.pebblenotificationcenter.bluetooth

internal fun String.replaceUnsupportedPebbleEmoji(): String {
   val input = this
   return buildString(input.length) {
      var index = 0
      while (index < input.length) {
         val codePoint = input.codePointAt(index)
         val charCount = Character.charCount(codePoint)

         when {
            codePoint.isPebbleSupportedEmoji() -> appendCodePoint(codePoint)
            codePoint.isEmojiVariationSelector() || codePoint.isEmojiModifier() -> Unit
            codePoint == ZERO_WIDTH_JOINER -> Unit
            codePoint.isLikelyEmoji() -> append(codePoint.unsupportedEmojiShortcode())
            else -> append(input, index, index + charCount)
         }

         index += charCount
      }
   }
}

private fun Int.isPebbleSupportedEmoji(): Boolean {
   return this in PEBBLE_SUPPORTED_EMOJI ||
      this in 0x1F493..0x1F49F ||
      this in 0x1F600..0x1F637
}

private fun Int.isLikelyEmoji(): Boolean {
   return this in 0x1F000..0x1FAFF ||
      this in 0x2600..0x27BF ||
      this in 0x2B00..0x2BFF
}

private fun Int.isEmojiVariationSelector(): Boolean = this == 0xFE0E || this == 0xFE0F

private fun Int.isEmojiModifier(): Boolean = this in 0x1F3FB..0x1F3FF

private fun Int.unsupportedEmojiShortcode(): String {
   val explicitName = UNSUPPORTED_EMOJI_SHORTCODES[this]
   if (explicitName != null) {
      return ":$explicitName:"
   }

   val unicodeName = Character.getName(this)
      ?.lowercase()
      ?.removePrefix("emoji modifier ")
      ?.removePrefix("regional indicator symbol letter ")
      ?.replace(" squared", "")
      ?.replace(" sign", "")
      ?.replace(" button", "")
      ?.replace(" face", "")
      ?.replace(" symbol", "")
      ?.replace(" with ", " ")
      ?.replace(" and ", " ")
      ?.replace(Regex("[^a-z0-9]+"), "_")
      ?.trim('_')
      ?.takeIf { it.isNotBlank() }
      ?: "emoji"

   return ":$unicodeName:"
}

private val PEBBLE_SUPPORTED_EMOJI = setOf(
   0x231A,
   0x2620,
   0x263A,
   0x26A7,
   0x2705,
   0x270B,
   0x270C,
   0x2728,
   0x274E,
   0x2757,
   0x2763,
   0x2764,
   0x2B50,
   0x1F319,
   0x1F31F,
   0x1F337,
   0x1F338,
   0x1F33A,
   0x1F340,
   0x1F37A,
   0x1F37B,
   0x1F389,
   0x1F3B6,
   0x1F3F3,
   0x1F425,
   0x1F440,
   0x1F44D,
   0x1F44E,
   0x1F480,
   0x1F4A1,
   0x1F4A3,
   0x1F4A5,
   0x1F4A9,
   0x1F4AF,
   0x1F5A4,
   0x1F643,
   0x1F644,
   0x1F64F,
   0x1F917,
   0x1F918,
   0x1F91D,
   0x1F923,
   0x1F924,
   0x1F929,
   0x1F92A,
   0x1F92C,
   0x1F92E,
   0x1F970,
   0x1F97A,
)

private val UNSUPPORTED_EMOJI_SHORTCODES = mapOf(
   0x1F409 to "dragon",
   0x1F432 to "dragon_face",
   0x1F346 to "eggplant",
)

private const val ZERO_WIDTH_JOINER = 0x200D
