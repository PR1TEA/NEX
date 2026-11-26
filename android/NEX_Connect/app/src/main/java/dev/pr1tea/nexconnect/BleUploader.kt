package dev.pr1tea.nexconnect

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.io.ByteArrayOutputStream
import java.util.UUID
import kotlin.math.min

@SuppressLint("MissingPermission")
class BleUploader(
    private val context: Context,
    private val onStatus: (String) -> Unit,
    private val onConnection: (Boolean) -> Unit,
) {
    private val main = Handler(Looper.getMainLooper())
    private val serviceUuid: UUID = UUID.fromString("6e657865-0001-4e45-582d-574154434800")
    private val controlUuid: UUID = UUID.fromString("6e657865-0002-4e45-582d-574154434800")
    private val dataUuid: UUID = UUID.fromString("6e657865-0003-4e45-582d-574154434800")
    private val statusUuid: UUID = UUID.fromString("6e657865-0004-4e45-582d-574154434800")
    private val remControlUuid: UUID = UUID.fromString("6e657865-0005-4e45-582d-574154434800")
    private val remDataUuid: UUID = UUID.fromString("6e657865-0006-4e45-582d-574154434800")
    private val remStatusUuid: UUID = UUID.fromString("6e657865-0007-4e45-582d-574154434800")
    private val cccdUuid: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private var gatt: BluetoothGatt? = null
    private var control: BluetoothGattCharacteristic? = null
    private var data: BluetoothGattCharacteristic? = null
    private var statusCharacteristic: BluetoothGattCharacteristic? = null
    private var remControl: BluetoothGattCharacteristic? = null
    private var remData: BluetoothGattCharacteristic? = null
    private var remStatus: BluetoothGattCharacteristic? = null

    private var remGetCallback: ((String?) -> Unit)? = null
    private var remGetBuffer = ByteArrayOutputStream()
    private var remGetExpected = -1
    private var remSetCallback: ((Boolean) -> Unit)? = null
    private var remPayload: ByteArray? = null
    private var remOffset = 0
    private var remPhase = RemPhase.IDLE
    private var ready = false
    private var scanning = false
    private var mtu = 23

    private var payload: ByteArray? = null
    private var offset = 0
    private var phase = Phase.IDLE
    private var uploadCallback: ((Boolean) -> Unit)? = null

    private enum class Phase { IDLE, START, DATA, COMMIT, STATUS }
    private enum class RemPhase { IDLE, START, DATA, COMMIT, STATUS }

    private val adapter get() = context.getSystemService(BluetoothManager::class.java).adapter

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            stopScan()
            onStatus("正在连接…")
            gatt = result.device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            onStatus("蓝牙搜索失败：$errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.discoverServices()
            } else {
                ready = false
                g.close()
                gatt = null
                main.post {
                    onConnection(false)
                    onStatus("连接已断开")
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                onStatus("服务发现失败：$status")
                return
            }
            val service = g.getService(serviceUuid)
            if (service == null) {
                onStatus("未找到手表服务")
                g.disconnect()
                return
            }
            control = service.getCharacteristic(controlUuid)
            data = service.getCharacteristic(dataUuid)
            statusCharacteristic = service.getCharacteristic(statusUuid)
            remControl = service.getCharacteristic(remControlUuid)
            remData = service.getCharacteristic(remDataUuid)
            remStatus = service.getCharacteristic(remStatusUuid)
            val notificationChar = remData
            if (notificationChar != null) {
                g.setCharacteristicNotification(notificationChar, true)
                val descriptor = notificationChar.getDescriptor(cccdUuid)
                if (descriptor != null) {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(descriptor)
                }
            }
            if (control == null || data == null || statusCharacteristic == null) {
                onStatus("手表服务不完整")
                g.disconnect()
                return
            }
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
            g.requestMtu(517)
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            this@BleUploader.mtu = mtu
            ready = true
            main.post {
                onConnection(true)
                onStatus("已连接 NEX-Watch")
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                finish(false)
                finishRemSet(false)
                return
            }
            when {
                characteristic.uuid == remControlUuid && remPhase == RemPhase.START -> {
                    remPhase = RemPhase.DATA
                    remOffset = 0
                    sendReminderChunk()
                }
                characteristic.uuid == remDataUuid && remPhase == RemPhase.DATA -> sendReminderChunk()
                characteristic.uuid == remControlUuid && remPhase == RemPhase.COMMIT -> {
                    remPhase = RemPhase.STATUS
                    main.postDelayed({ g.readCharacteristic(remStatus) }, 350)
                }
                characteristic.uuid == controlUuid && phase == Phase.START -> {
                    phase = Phase.DATA
                    offset = 0
                    sendChunk()
                }
                characteristic.uuid == dataUuid -> sendChunk()
                characteristic.uuid == controlUuid && phase == Phase.COMMIT -> {
                    phase = Phase.STATUS
                    main.postDelayed({ g.readCharacteristic(statusCharacteristic) }, 350)
                }
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicRead(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            handleRead(characteristic, characteristic.value, status)
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            handleRead(characteristic, value, status)
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            handleReminderNotification(characteristic.value)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            handleReminderNotification(value)
        }
    }

    private fun handleRead(characteristic: BluetoothGattCharacteristic, value: ByteArray?, status: Int) {
        if (status != BluetoothGatt.GATT_SUCCESS) {
            finish(false)
            finishRemSet(false)
            return
        }
        if (characteristic.uuid == remStatusUuid) {
            finishRemSet(value?.firstOrNull()?.toInt() == 2)
        } else {
            finish(value?.firstOrNull()?.toInt() == 2)
        }
    }

    private fun handleReminderNotification(value: ByteArray) {
        if (remGetCallback == null) return
        if (remGetExpected < 0) {
            if (value.size < 4) return
            remGetExpected = (value[0].toInt() and 0xFF) or
                ((value[1].toInt() and 0xFF) shl 8) or
                ((value[2].toInt() and 0xFF) shl 16) or
                ((value[3].toInt() and 0xFF) shl 24)
            remGetBuffer = ByteArrayOutputStream()
            if (remGetExpected == 0) finishRemGet("")
            return
        }
        remGetBuffer.write(value)
        if (remGetBuffer.size() >= remGetExpected) {
            finishRemGet(String(remGetBuffer.toByteArray(), 0, remGetExpected, Charsets.UTF_8))
        }
    }

    private fun finishRemGet(json: String?) {
        val callback = remGetCallback
        remGetCallback = null
        remGetExpected = -1
        remGetBuffer = ByteArrayOutputStream()
        main.post { callback?.invoke(json) }
    }

    private fun finishRemSet(ok: Boolean) {
        val callback = remSetCallback
        remSetCallback = null
        remPhase = RemPhase.IDLE
        remPayload = null
        main.post { callback?.invoke(ok) }
    }

    fun scan() {
        val current = adapter
        if (current == null || !current.isEnabled) {
            onStatus("请先打开手机蓝牙")
            return
        }
        if (scanning) return
        scanning = true
        onStatus("正在搜索 NEX-Watch…")
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(serviceUuid)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        val scanner = current.bluetoothLeScanner
        if (scanner == null) {
            scanning = false
            onStatus("蓝牙不可用")
            return
        }
        scanner.startScan(listOf(filter), settings, scanCallback)
        main.postDelayed({
            if (scanning) {
                stopScan()
                onStatus("未找到手表，请确认手表已打开“手机连接”")
            }
        }, 15000)
    }

    fun getReminders(callback: (String?) -> Unit) {
        if (!ready || remControl == null) {
            callback(null)
            return
        }
        remGetCallback = callback
        remGetBuffer = ByteArrayOutputStream()
        remGetExpected = -1
        if (!write(remControl!!, byteArrayOf(0x20))) {
            finishRemGet(null)
        }
    }

    fun setReminders(json: String, callback: (Boolean) -> Unit) {
        if (!ready || remControl == null) {
            callback(false)
            return
        }
        val bytes = json.toByteArray(Charsets.UTF_8)
        remPayload = bytes
        remOffset = 0
        remPhase = RemPhase.START
        remSetCallback = callback
        val start = ByteArray(5)
        start[0] = 0x21
        for (i in 0 until 4) {
            start[i + 1] = ((bytes.size ushr (8 * i)) and 0xFF).toByte()
        }
        if (!write(remControl!!, start)) {
            finishRemSet(false)
        }
    }

    private fun sendReminderChunk() {
        val bytes = remPayload ?: return finishRemSet(false)
        val characteristic = remData ?: return finishRemSet(false)
        if (remOffset >= bytes.size) {
            remPhase = RemPhase.COMMIT
            if (!write(remControl!!, byteArrayOf(0x23))) finishRemSet(false)
            return
        }
        val size = min((mtu - 3).coerceIn(20, 500), bytes.size - remOffset)
        val chunk = bytes.copyOfRange(remOffset, remOffset + size)
        remOffset += size
        if (!write(characteristic, chunk)) {
            finishRemSet(false)
        }
    }

    fun upload(bytes: ByteArray, callback: (Boolean) -> Unit) {
        if (!ready || gatt == null || control == null) {
            callback(false)
            return
        }
        payload = bytes
        offset = 0
        phase = Phase.START
        uploadCallback = callback
        val start = ByteArray(5)
        start[0] = 0x01
        for (i in 0 until 4) {
            start[i + 1] = ((bytes.size ushr (8 * i)) and 0xFF).toByte()
        }
        if (!write(control!!, start)) {
            finish(false)
        }
    }

    fun disconnect() {
        ready = false
        uploadCallback = null
        phase = Phase.IDLE
        remGetCallback = null
        remSetCallback = null
        remPhase = RemPhase.IDLE
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        onConnection(false)
    }

    private fun sendChunk() {
        val g = gatt ?: return finish(false)
        val bytes = payload ?: return finish(false)
        val characteristic = data ?: return finish(false)
        if (offset >= bytes.size) {
            phase = Phase.COMMIT
            if (!write(control!!, byteArrayOf(0x02))) finish(false)
            return
        }
        val size = min((mtu - 3).coerceIn(20, 500), bytes.size - offset)
        val chunk = bytes.copyOfRange(offset, offset + size)
        offset += size
        if (!write(characteristic, chunk)) {
            finish(false)
        }
    }

    private fun write(characteristic: BluetoothGattCharacteristic, value: ByteArray): Boolean {
        val g = gatt ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(characteristic, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            characteristic.value = value
            characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            g.writeCharacteristic(characteristic)
        }
    }

    private fun finish(ok: Boolean) {
        if (phase == Phase.IDLE && uploadCallback == null) return
        val callback = uploadCallback
        uploadCallback = null
        phase = Phase.IDLE
        payload = null
        main.post { callback?.invoke(ok) }
    }

    private fun stopScan() {
        scanning = false
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
    }
}
