package com.matejdro.pebblenotificationcenter.bluetooth

import io.kotest.matchers.shouldBe
import org.junit.jupiter.api.Test

class PebbleEmojiFallbackTest {
   @Test
   fun `Keep emoji supported by PebbleOS`() {
      "ok 😂 👍 ❤️".replaceUnsupportedPebbleEmoji() shouldBe "ok 😂 👍 ❤"
   }

   @Test
   fun `Replace unsupported emoji with shortcode names`() {
      "dragon 🐉 eggplant 🍆".replaceUnsupportedPebbleEmoji() shouldBe "dragon :dragon: eggplant :eggplant:"
   }

   @Test
   fun `Drop unsupported emoji modifiers`() {
      "thumb 👍🏻".replaceUnsupportedPebbleEmoji() shouldBe "thumb 👍"
   }
}
