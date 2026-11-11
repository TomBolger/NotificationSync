package com.matejdro.pebblenotificationcenter.notification.model

import java.time.Instant

data class ParsedNotification(
   val key: String,
   val pkg: String,
   val title: String,
   val subtitle: String,
   val body: String,
   val timestamp: Instant,

   /**
    * When true, the notification would not vibrate and/or make sounds from the phone.
    */
   val isSilent: Boolean = true,
   /**
    * When true, the phone has the do not disturb on and this notification would be filtered by it
    */
   val isFilteredByDoNotDisturb: Boolean = false,
   /**
    * When true, updates of this notification must not alert again (Notification.FLAG_ONLY_ALERT_ONCE).
    */
   val onlyAlertOnce: Boolean = false,
   /**
    * When true, the app's group alert behaviour keeps this notification quiet (its group alerts through another
    * member).
    */
   val groupAlertSuppressed: Boolean = false,
   /** Android's own group key (app + group, including Android's automatic groups) when the phone stacks it. */
   val groupKey: String? = null,
   /** Section of the phone's shade: [SHADE_SECTION_ALERTING], [SHADE_SECTION_CONVERSATION], [SHADE_SECTION_SILENT]. */
   val shadeSection: Int = SHADE_SECTION_ALERTING,
   /** Android 16 notification category (Suggested, Promotions, News, Social), [SHADE_CATEGORY_NONE] otherwise. */
   val shadeCategory: Int = SHADE_CATEGORY_NONE,
   /**
    * When true, the app is paused (Digital Wellbeing, Focus mode) and the phone hides its notifications.
    */
   val isSuspended: Boolean = false,
   val nativeActions: List<NativeAction> = emptyList(),
   val channel: String? = null,
   val isOngoing: Boolean = false,
   val groupSummary: Boolean = false,
   val localOnly: Boolean = false,
   val media: Boolean = false,
   /**
    * When true, notification will always show up and vibrate, regardless of any settings
    */
   val forceVibrate: Boolean = false,
   val overrideVibrationPattern: List<Short>? = null,
   // This is pure kotlin module, so we cannot reference Drawable directly.
   // I don't want to change it, so for now we can just use Any and upcast it
   val iconDrawable: Any? = null,

   // Android type of this is android.graphics.drawable.Icon
   val largeImage: Any? = null,
   val id: Int = 0,
   val tag: String? = null,
)

data class NativeAction(
   val text: String,
   // This is pure kotlin module, so we cannot reference PendingIntent directly.
   // I don't want to change it, so for now we can just use Any and upcast it
   val pendingIntent: Any,
   val remoteInputResultKey: String? = null,
   val cannedTexts: List<String> = emptyList(),
   val allowFreeFormInput: Boolean = true,
) {
   override fun equals(other: Any?): Boolean {
      if (this === other) return true
      if (other !is NativeAction) return false

      if (allowFreeFormInput != other.allowFreeFormInput) return false
      if (text != other.text) return false
      if (pendingIntent !== other.pendingIntent) return false
      if (remoteInputResultKey != other.remoteInputResultKey) return false
      if (cannedTexts != other.cannedTexts) return false

      return true
   }

   override fun hashCode(): Int {
      var result = allowFreeFormInput.hashCode()
      result = 31 * result + text.hashCode()
      result = 31 * result + (remoteInputResultKey?.hashCode() ?: 0)
      result = 31 * result + cannedTexts.hashCode()
      return result
   }

   override fun toString(): String {
      return "NativeAction(text='$text', remoteInputResultKey=${remoteInputResultKey ?: "null"}," +
         " cannedTexts=$cannedTexts, allowFreeFormInput=$allowFreeFormInput)"
   }
}

const val SHADE_SECTION_ALERTING = 0
const val SHADE_SECTION_CONVERSATION = 1
const val SHADE_SECTION_SILENT = 2

const val SHADE_CATEGORY_NONE = 0
const val SHADE_CATEGORY_SUGGESTED = 1
const val SHADE_CATEGORY_PROMOTIONS = 2
const val SHADE_CATEGORY_NEWS = 3
const val SHADE_CATEGORY_SOCIAL = 4
