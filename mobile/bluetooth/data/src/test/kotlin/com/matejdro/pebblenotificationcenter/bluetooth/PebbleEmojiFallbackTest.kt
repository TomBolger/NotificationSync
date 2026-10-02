package com.matejdro.pebblenotificationcenter.bluetooth

import io.kotest.matchers.shouldBe
import org.junit.jupiter.api.Test

class PebbleEmojiFallbackTest {
   @Test
   fun `Keep emoji supported by PebbleOS`() {
      "ok 😂 👍 ❤️".replaceUnsupportedPebbleEmoji() shouldBe "ok 😂 👍 ❤"
   }

   @Test
   fun `Keep emoji from the expanded PebbleOS emoji fonts`() {
      "sun 🌞 dragon 🐉 eggplant 🍆 melting 🫠".replaceUnsupportedPebbleEmoji() shouldBe
         "sun 🌞 dragon 🐉 eggplant 🍆 melting 🫠"
   }

   @Test
   fun `Keep flags for the watch to draw`() {
      "flag 🇺🇸".replaceUnsupportedPebbleEmoji() shouldBe "flag 🇺🇸"
   }

   @Test
   fun `Replace unsupported emoji with shortcode names`() {
      "white sun 🌣".replaceUnsupportedPebbleEmoji() shouldBe "white sun :white_sun:"
   }

   @Test
   fun `Drop unsupported emoji modifiers`() {
      "thumb 👍🏻".replaceUnsupportedPebbleEmoji() shouldBe "thumb 👍"
   }
}
