package com.matejdro.pebblenotificationcenter.notification

import android.app.KeyguardManager
import android.content.Context
import android.os.PowerManager
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject

interface PhoneStateDetector {
   fun isPhoneUnlocked(): Boolean
}

@Inject
@ContributesBinding(AppScope::class)
class PhoneStateDetectorImpl(
   private val context: Context,
) : PhoneStateDetector {
   override fun isPhoneUnlocked(): Boolean {
      val powerManager = context.getSystemService(PowerManager::class.java)
      val keyguardManager = context.getSystemService(KeyguardManager::class.java)

      return powerManager?.isInteractive == true && keyguardManager?.isKeyguardLocked == false
   }
}
