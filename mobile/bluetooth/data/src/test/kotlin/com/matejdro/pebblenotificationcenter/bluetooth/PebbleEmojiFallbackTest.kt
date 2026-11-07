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

   @Test
   fun `Draw styled letters as plain letters`() {
      "\uD835\uDC01\uD835\uDC28\uD835\uDC25\uD835\uDC1D ｗｉｄｅ \u210D \uD835\uDFCF".replaceUnsupportedPebbleEmoji() shouldBe "Bold wide H 1"
   }
}
