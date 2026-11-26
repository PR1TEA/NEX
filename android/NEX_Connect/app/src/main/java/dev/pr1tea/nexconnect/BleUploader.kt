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

    private var gatt: BluetoothGatt? = null
    private var control: BluetoothGattCharacteristic? = null
    private var data: BluetoothGattCharacteristic? = null
    private var statusCharacteristic: BluetoothGattCharacteristic? = null
    private var ready = false
    private var scanning = false
    private var mtu = 23

    private var payload: ByteArray? = null
    private var offset = 0
    private var phase = Phase.IDLE
    private var uploadCallback: ((Boolean) -> Unit)? = null

    private enum class Phase { IDLE, START, DATA, COMMIT, STATUS }

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
                return
            }
            when {
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
            handleRead(characteristic.value, status)
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            handleRead(value, status)
        }
    }

    private fun handleRead(value: ByteArray?, status: Int) {
        if (status != BluetoothGatt.GATT_SUCCESS) {
            finish(false)
            return
        }
        finish(value?.firstOrNull()?.toInt() == 2)
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
