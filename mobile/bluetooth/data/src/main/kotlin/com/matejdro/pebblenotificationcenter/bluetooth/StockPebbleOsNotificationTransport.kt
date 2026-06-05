@file:Suppress("MagicNumber", "MaxLineLength", "TrailingCommaOnDeclarationSite")

package com.matejdro.pebblenotificationcenter.bluetooth

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt.GATT_SUCCESS
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.datastore.preferences.core.Preferences
import com.matejdro.pebble.bluetooth.common.util.LimitingStringEncoder
import com.matejdro.pebblenotificationcenter.notification.model.Action
import com.matejdro.pebblenotificationcenter.notification.model.ProcessedNotification
import com.matejdro.pebblenotificationcenter.rules.keys.get
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.ContributesBinding
import dev.zacsweers.metro.Inject
import dev.zacsweers.metro.SingleIn
import dispatch.core.DefaultCoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.launch
import logcat.logcat
import okio.Buffer
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

interface StockNotificationTransport {
   val actions: SharedFlow<StockNotificationAction>

   suspend fun setEnabled(enabled: Boolean)
   suspend fun upsert(notification: ProcessedNotification, preferences: Preferences)
   suspend fun delete(key: String)
   suspend fun deleteAll()
}

object NoOpStockNotificationTransport : StockNotificationTransport {
   override val actions = MutableSharedFlow<StockNotificationAction>()

   override suspend fun setEnabled(enabled: Boolean) = Unit
   override suspend fun upsert(notification: ProcessedNotification, preferences: Preferences) = Unit
   override suspend fun delete(key: String) = Unit
   override suspend fun deleteAll() = Unit
}

@Inject
@SingleIn(AppScope::class)
@ContributesBinding(AppScope::class)
class StockPebbleOsNotificationTransport(
   private val context: Context,
   private val defaultScope: DefaultCoroutineScope,
) : StockNotificationTransport {
   private val bridge = PpogGattBridge(context, defaultScope, ::handleBridgeMessage)
   private val utf8Encoder = LimitingStringEncoder()
   private val keysByUuid = ConcurrentHashMap<UUID, String>()
   private val uuidsByKey = ConcurrentHashMap<String, UUID>()
   private val _actions = MutableSharedFlow<StockNotificationAction>(extraBufferCapacity = 32)

   override val actions: SharedFlow<StockNotificationAction> = _actions

   override suspend fun setEnabled(enabled: Boolean) {
      if (enabled) {
         bridge.start()
      } else {
         bridge.stop()
      }
   }

   override suspend fun upsert(notification: ProcessedNotification, preferences: Preferences) {
      val uuid = uuidForKey(notification.systemData.key)
      keysByUuid[uuid] = notification.systemData.key
      uuidsByKey[notification.systemData.key] = uuid

      val item = buildTimelineNotification(uuid, notification, preferences)
      bridge.send(byteArrayOf(CMD_UPSERT) + item)
   }

   override suspend fun delete(key: String) {
      val uuid = uuidsByKey.remove(key) ?: uuidForKey(key)
      keysByUuid.remove(uuid)
      bridge.send(byteArrayOf(CMD_DELETE) + uuid.toPebbleBytes())
   }

   override suspend fun deleteAll() {
      val uuids = uuidsByKey.values.toList()
      uuidsByKey.clear()
      keysByUuid.clear()
      for (uuid in uuids) {
         bridge.send(byteArrayOf(CMD_DELETE) + uuid.toPebbleBytes())
      }
   }

   private fun handleBridgeMessage(payload: ByteArray) {
      if (payload.isEmpty() || payload[0] != CMD_ACTION) {
         return
      }
      if (payload.size < 19) {
         logcat { "SyncFW action packet too short: ${payload.size}" }
         return
      }

      val uuid = payload.copyOfRange(1, 17).toUuid()
      val actionId = payload[17].toInt() and 0xff
      val actionType = payload[18].toInt() and 0xff
      val key = keysByUuid[uuid]
      logcat { "SyncFW stock action uuid=$uuid key=$key actionId=$actionId type=$actionType" }

      if (key != null && actionType == ACTION_TYPE_DISMISS) {
         _actions.tryEmit(StockNotificationAction.Dismiss(key))
      }
   }

   private fun buildTimelineNotification(
      uuid: UUID,
      notification: ProcessedNotification,
      preferences: Preferences,
   ): ByteArray {
      val parsed = notification.systemData
      val title = parsed.subtitle.ifBlank { parsed.body.lineSequence().firstOrNull().orEmpty() }
      val body = parsed.body.ifBlank { parsed.title }
      val actions = notification.actions.toStockActions()

      val attrs = buildList {
         parsed.title.takeIf { it.isNotBlank() }?.let { add(textAttr(ATTR_APP_NAME, it, 40)) }
         title.takeIf { it.isNotBlank() }?.let { add(textAttr(ATTR_TITLE, it, 64)) }
         body.takeIf { it.isNotBlank() }?.let { add(textAttr(ATTR_BODY, it, 512)) }
         add(uintAttr(ATTR_TINY_ICON, (parsed.pebbleOsIconId() or ICON_RESOURCE_FLAG).toUInt()))
         if (notification.vibrated) {
            add(vibrationAttr(preferences))
         }
      }.filterNotNull()

      val attrBytes = attrs.concat()
      val actionBytes = actions.concat()

      return Buffer().apply {
         write(uuid.toPebbleBytes())
         write(ANDROID_NOTIFICATIONS_UUID.toPebbleBytes())
         writeIntLe(parsed.timestamp.epochSecond.toInt())
         writeShortLe(0)
         writeByte(TIMELINE_TYPE_NOTIFICATION)
         writeShortLe(0)
         writeByte(LAYOUT_GENERIC_NOTIFICATION)
         writeShortLe(attrBytes.size + actionBytes.size)
         writeByte(attrs.size)
         writeByte(actions.size)
         write(attrBytes)
         write(actionBytes)
      }.readByteArray()
   }

   private fun textAttr(id: Int, text: String, maxBytes: Int): ByteArray {
      return attr(id, utf8Encoder.encodeSizeLimited(text.replaceUnsupportedPebbleEmoji(), maxBytes, true).encodedString)
   }

   private fun uintAttr(id: Int, value: UInt): ByteArray {
      return attr(
         id,
         Buffer().apply {
            writeIntLe(value.toInt())
         }.readByteArray()
      )
   }

   private fun vibrationAttr(preferences: Preferences): ByteArray? {
      val rawPattern = preferences[com.matejdro.pebblenotificationcenter.rules.RuleOption.vibrationPattern]
      val pattern = rawPattern.split(",")
         .mapNotNull { it.trim().toUIntOrNull() }
         .takeIf { it.isNotEmpty() }
         ?: return null

      return attr(
         ATTR_VIBRATION_PATTERN,
         Buffer().apply {
            for (item in pattern) {
               writeIntLe(item.toInt())
            }
         }.readByteArray()
      )
   }

   private fun attr(id: Int, content: ByteArray): ByteArray {
      return Buffer().apply {
         writeByte(id)
         writeShortLe(content.size)
         write(content)
      }.readByteArray()
   }

   private fun List<Action>.toStockActions(): List<ByteArray> {
      return mapNotNull { action ->
         val type = when (action) {
            is Action.Dismiss -> ACTION_TYPE_DISMISS
            is Action.Reply -> ACTION_TYPE_RESPONSE
            is Action.Snooze -> ACTION_TYPE_SNOOZE
            is Action.Native,
            is Action.PauseApp,
            is Action.PauseConversation,
            is Action.ShowImage,
            is Action.TaskerTask -> ACTION_TYPE_GENERIC
         }

         Buffer().apply {
            writeByte(action.id.toInt())
            writeByte(type)
            writeByte(1)
            write(textAttr(ATTR_TITLE, action.title, 32))
         }.readByteArray()
      }
   }

   private fun uuidForKey(key: String): UUID {
      val bytes = MessageDigest.getInstance("SHA-256").digest(key.toByteArray()).copyOf(16)
      bytes[0] = 'S'.code.toByte()
      bytes[1] = 'F'.code.toByte()
      bytes[2] = 'N'.code.toByte()
      bytes[3] = '1'.code.toByte()
      return bytes.toUuid()
   }

   companion object {
      private const val CMD_UPSERT: Byte = 0x02
      private const val CMD_DELETE: Byte = 0x04
      private const val CMD_ACTION: Byte = 0x82.toByte()

      private const val ATTR_TITLE = 0x01
      private const val ATTR_BODY = 0x03
      private const val ATTR_TINY_ICON = 0x04
      private const val ATTR_APP_NAME = 30
      private const val ATTR_VIBRATION_PATTERN = 0x31

      private const val TIMELINE_TYPE_NOTIFICATION = 1
      private const val LAYOUT_GENERIC_NOTIFICATION = 4
      private const val ACTION_TYPE_GENERIC = 0x02
      private const val ACTION_TYPE_RESPONSE = 0x03
      private const val ACTION_TYPE_DISMISS = 0x04
      private const val ACTION_TYPE_SNOOZE = 0x06
      private const val ICON_RESOURCE_FLAG = -0x80000000

      private val ANDROID_NOTIFICATIONS_UUID = UUID.fromString("ed429c16-f674-4220-95da-454f303f15e2")
   }
}

private class PpogGattBridge(
   private val context: Context,
   private val defaultScope: DefaultCoroutineScope,
   private val inboundPebblePayload: (ByteArray) -> Unit,
) {
   private var server: BluetoothGattServer? = null
   private var dataCharacteristic: BluetoothGattCharacteristic? = null
   private var connectedDevice: BluetoothDevice? = null
   private var pumpJob: Job? = null
   private val outboundPackets = Channel<ByteArray>(capacity = Channel.UNLIMITED)
   private var outboundSequence = 0

   @SuppressLint("MissingPermission")
   suspend fun start() {
      if (server != null) {
         return
      }
      if (!hasBluetoothPermission()) {
         logcat { "SyncFW stock transport needs Bluetooth permission" }
         return
      }

      val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return
      val callback = Callback()
      try {
         server = manager.openGattServer(context, callback)
         server?.addService(buildService())
         pumpJob = defaultScope.launch { pumpOutbound() }
         logcat { "SyncFW stock PPoGATT transport started" }
      } catch (e: SecurityException) {
         logcat { "SyncFW stock PPoGATT transport could not start: ${e.message}" }
         stop()
      }
   }

   @SuppressLint("MissingPermission")
   suspend fun stop() {
      pumpJob?.cancel()
      pumpJob = null
      outboundSequence = 0
      connectedDevice = null
      dataCharacteristic = null
      try {
         server?.clearServices()
         server?.close()
      } catch (e: SecurityException) {
         logcat { "SyncFW stock PPoGATT transport could not stop cleanly: ${e.message}" }
      }
      server = null
   }

   suspend fun send(payload: ByteArray) {
      outboundPackets.send(pebblePacket(SYNCFW_ENDPOINT, payload))
   }

   private suspend fun pumpOutbound() {
      for (packet in outboundPackets) {
         packet.asList().chunked(PPOG_DATA_CHUNK_SIZE).forEach { chunk ->
            sendPpogPacket(byteArrayOf((outboundSequence shl 3).toByte()) + chunk.toByteArray())
            outboundSequence = (outboundSequence + 1) and PPOG_SEQUENCE_MASK
         }
      }
   }

   @SuppressLint("MissingPermission")
   private fun sendPpogPacket(packet: ByteArray): Boolean {
      val server = server ?: return false
      val device = connectedDevice ?: return false
      val characteristic = dataCharacteristic ?: return false
      return try {
         if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            server.notifyCharacteristicChanged(device, characteristic, false, packet) == android.bluetooth.BluetoothStatusCodes.SUCCESS
         } else {
            @Suppress("DEPRECATION")
            characteristic.value = packet
            @Suppress("DEPRECATION")
            server.notifyCharacteristicChanged(device, characteristic, false)
         }
      } catch (e: SecurityException) {
         logcat { "SyncFW stock PPoGATT notify failed: ${e.message}" }
         false
      }
   }

   private fun buildService(): BluetoothGattService {
      return BluetoothGattService(PPOGATT_SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY).apply {
         addCharacteristic(
            BluetoothGattCharacteristic(
               PPOGATT_META_UUID,
               BluetoothGattCharacteristic.PROPERTY_READ,
               BluetoothGattCharacteristic.PERMISSION_READ_ENCRYPTED,
            )
         )
         dataCharacteristic = BluetoothGattCharacteristic(
            PPOGATT_DATA_UUID,
            BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
            BluetoothGattCharacteristic.PERMISSION_WRITE_ENCRYPTED,
         ).apply {
            addDescriptor(
               BluetoothGattDescriptor(
                  CLIENT_CONFIG_UUID,
                  BluetoothGattDescriptor.PERMISSION_WRITE_ENCRYPTED,
               )
            )
         }
         addCharacteristic(dataCharacteristic)
      }
   }

   private fun handlePpogPacket(value: ByteArray) {
      if (value.isEmpty()) {
         return
      }
      val type = value[0].toInt() and PPOG_TYPE_MASK
      val sequence = (value[0].toInt() and 0xff) ushr 3
      when (type) {
         PPOG_TYPE_DATA -> {
            sendPpogPacket(byteArrayOf((PPOG_TYPE_ACK or (sequence shl 3)).toByte()))
            val data = value.copyOfRange(1, value.size)
            handlePebbleProtocolBytes(data)
         }
         PPOG_TYPE_ACK -> Unit
         PPOG_TYPE_RESET_REQUEST -> sendPpogPacket(byteArrayOf(PPOG_TYPE_RESET_COMPLETE.toByte(), RX_WINDOW, TX_WINDOW))
         PPOG_TYPE_RESET_COMPLETE -> logcat { "SyncFW stock PPoGATT session open" }
      }
   }

   private fun handlePebbleProtocolBytes(bytes: ByteArray) {
      if (bytes.size < 5) {
         return
      }
      val length = ((bytes[0].toInt() and 0xff) shl 8) or (bytes[1].toInt() and 0xff)
      val endpoint = ((bytes[2].toInt() and 0xff) shl 8) or (bytes[3].toInt() and 0xff)
      if (endpoint == SYNCFW_ENDPOINT && length == bytes.size - 4) {
         inboundPebblePayload(bytes.copyOfRange(4, bytes.size))
      }
   }

   private fun metaValue(): ByteArray {
      return byteArrayOf(0, 1) + TRANSPORT_APP_UUID.toPebbleBytes() + byteArrayOf(0)
   }

   private fun hasBluetoothPermission(): Boolean {
      return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
         context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
   }

   private inner class Callback : BluetoothGattServerCallback() {
      override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
         if (newState == BluetoothProfile.STATE_CONNECTED) {
            connectedDevice = device
         } else if (connectedDevice?.address == device.address) {
            connectedDevice = null
         }
      }

      override fun onCharacteristicReadRequest(
         device: BluetoothDevice,
         requestId: Int,
         offset: Int,
         characteristic: BluetoothGattCharacteristic,
      ) {
         if (characteristic.uuid != PPOGATT_META_UUID) {
            return
         }
         val value = metaValue()
         val response = if (offset <= value.size) value.copyOfRange(offset, value.size) else byteArrayOf()
         try {
            server?.sendResponse(device, requestId, GATT_SUCCESS, offset, response)
         } catch (_: SecurityException) {
         }
      }

      override fun onCharacteristicWriteRequest(
         device: BluetoothDevice,
         requestId: Int,
         characteristic: BluetoothGattCharacteristic,
         preparedWrite: Boolean,
         responseNeeded: Boolean,
         offset: Int,
         value: ByteArray,
      ) {
         connectedDevice = device
         if (characteristic.uuid == PPOGATT_DATA_UUID) {
            handlePpogPacket(value)
         }
         if (responseNeeded) {
            try {
               server?.sendResponse(device, requestId, GATT_SUCCESS, offset, value)
            } catch (_: SecurityException) {
            }
         }
      }

      override fun onDescriptorWriteRequest(
         device: BluetoothDevice,
         requestId: Int,
         descriptor: BluetoothGattDescriptor,
         preparedWrite: Boolean,
         responseNeeded: Boolean,
         offset: Int,
         value: ByteArray?,
      ) {
         connectedDevice = device
         if (responseNeeded) {
            try {
               server?.sendResponse(device, requestId, GATT_SUCCESS, offset, value)
            } catch (_: SecurityException) {
            }
         }
      }
   }

   companion object {
      private val PPOGATT_SERVICE_UUID = UUID.fromString("10000000-328e-0fbb-c642-1aa6699bdada")
      private val PPOGATT_DATA_UUID = UUID.fromString("10000001-328e-0fbb-c642-1aa6699bdada")
      private val PPOGATT_META_UUID = UUID.fromString("10000002-328e-0fbb-c642-1aa6699bdada")
      private val CLIENT_CONFIG_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
      private val TRANSPORT_APP_UUID = UUID.fromString("53464e31-0000-4000-8000-000000000001")

      private const val SYNCFW_ENDPOINT = 0x5346
      private const val PPOG_TYPE_DATA = 0
      private const val PPOG_TYPE_ACK = 1
      private const val PPOG_TYPE_RESET_REQUEST = 2
      private const val PPOG_TYPE_RESET_COMPLETE = 3
      private const val PPOG_TYPE_MASK = 0x07
      private const val PPOG_SEQUENCE_MASK = 0x1f
      private const val PPOG_DATA_CHUNK_SIZE = 19
      private const val RX_WINDOW: Byte = 4
      private const val TX_WINDOW: Byte = 4
   }
}

private fun pebblePacket(endpoint: Int, payload: ByteArray): ByteArray {
   return Buffer().apply {
      writeShort(payload.size)
      writeShort(endpoint)
      write(payload)
   }.readByteArray()
}

private fun UUID.toPebbleBytes(): ByteArray {
   return Buffer().apply {
      writeLong(mostSignificantBits)
      writeLong(leastSignificantBits)
   }.readByteArray()
}

private fun ByteArray.toUuid(): UUID {
   val buffer = Buffer().write(this)
   return UUID(buffer.readLong(), buffer.readLong())
}

private fun List<ByteArray>.concat(): ByteArray {
   val size = sumOf { it.size }
   return Buffer().apply {
      for (item in this@concat) {
         write(item)
      }
   }.readByteArray(size.toLong())
}
