package com.example.proxlock

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.SharedPreferences
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * BLE 协议与连接管理（与固件 Proximity Service 契约一致）：
 *  - 连接后写 Control 特征 0x01 激活距离监测
 *  - 订阅 Status 特征 notify: [0]=int8 rssi, [1]=state, [2]=flags
 *  - 断线后指数退避自动重连 (1s → 30s 封顶)，autoConnect 兜底
 */
@SuppressLint("MissingPermission")
object BleManager {

    // ---- 与固件约定的 128-bit UUID ----
    const val SERVICE_UUID = "a5f5aa00-c263-4a0c-8e8f-9c0b7a5d3e01"
    const val CONTROL_UUID = "a5f5aa01-c263-4a0c-8e8f-9c0b7a5d3e01"
    const val STATUS_UUID  = "a5f5aa02-c263-4a0c-8e8f-9c0b7a5d3e01"

    // 固件广播的完整设备名
    const val DEVICE_NAME_PREFIX = "HID Keyboard"

    // 距离判定状态（与固件 PROX_STATE_* 一致）
    const val PROX_ARMED = 0
    const val PROX_TRIGGERED = 1
    const val PROX_COOLDOWN = 2
    const val PROX_OFF = 3

    enum class Phase { STOPPED, SCANNING, CONNECTING, CONNECTED, RECONNECTING }

    data class UiState(
        val phase: Phase = Phase.STOPPED,
        val deviceName: String = "",
        val rssi: Int? = null,          // EMA 后的 RSSI (dBm)
        val proxState: Int = -1,        // PROX_* ，-1 = 未知
        val retryCount: Int = 0,        // 重连次数
        val needPairing: Boolean = false // 连接被拒 -> 引导系统配对
    )

    private const val TAG = "BleManager"
    private const val DISCOVER_DELAY_MS = 600L
    private val BACKOFF_SECONDS = longArrayOf(1, 2, 4, 8, 16, 30)

    // 已连接过的键盘地址持久化：断线重连优先走 autoConnect 定向重连。
    // 锁屏/后台时 Android 会节流应用主动 BLE 扫描(扫不到广播就永远连不上)，
    // 而 autoConnect 的重连意图由系统蓝牙进程维护，锁屏下依然有效。
    private const val PREFS_NAME = "proxlock_prefs"
    private const val PREFS_KEY_ADDR = "last_device_addr"
    private const val AUTO_WATCHDOG_MS = 25_000L  // autoConnect 无回调时的检查周期

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state

    private val handler = Handler(Looper.getMainLooper())
    private var bluetoothAdapter: BluetoothAdapter? = null
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var running = false
    private var retry = 0
    private var controlRetries = 0   // Control 激活写入重试计数
    private var cacheRefreshTries = 0 // GATT 缓存刷新计数

    private var appContext: Context? = null
    private var prefs: SharedPreferences? = null
    private var lastAddr: String? = null   // 上次成功连接的键盘地址
    private var connEstablished = false    // 链路层已连(激活流程进行中)

    // ---- 对外控制 ----

    fun start(context: Context) {
        if (running) return
        running = true
        retry = 0
        connEstablished = false
        appContext = context.applicationContext
        prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        lastAddr = prefs?.getString(PREFS_KEY_ADDR, null)
        val bm = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothAdapter = bm.adapter
        // 连接过的设备直接 autoConnect 定向重连(锁屏/后台有效)；
        // 没有历史地址才走首次扫描发现
        _state.value = UiState(
            phase = if (lastAddr != null) Phase.CONNECTING else Phase.SCANNING,
            retryCount = 0
        )
        if (lastAddr != null) connectKnown() else startScan()
    }

    fun stop() {
        running = false
        handler.removeCallbacksAndMessages(null)
        stopScan()
        disconnectGatt()
        _state.value = _state.value.copy(phase = Phase.STOPPED, rssi = null, proxState = -1)
    }

    // ---- 扫描 ----

    private fun startScan() {
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: run {
            scheduleReconnect(); return
        }
        if (scanning) return
        // 不加 ScanFilter：部分手机对广播名过滤行为不一致，回调里按名字匹配
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanning = true
        try {
            scanner.startScan(null, settings, scanCallback)
        } catch (e: Exception) {
            Log.w(TAG, "startScan failed: ${e.message}")
            scanning = false
            scheduleReconnect()
        }
        // 扫描超时兜底：30s 没扫到就重试
        handler.postDelayed({ if (scanning) { stopScan(); scheduleReconnect() } }, 30_000)
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        try {
            bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) { }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!running || gatt != null) return
            val name = result.scanRecord?.deviceName ?: result.device.name
            Log.d(TAG, "scan ${result.device.address} name=$name rssi=${result.rssi}")
            // 前缀匹配 "HID"：兼容固件旧广播名 "HID Keybroad" 与 "HID Keyboard"
            if (name != null && name.uppercase().startsWith("HID")) {
                stopScan()
                connectToDevice(result.device)
            }
        }
        override fun onScanFailed(errorCode: Int) {
            scanning = false
            scheduleReconnect()
        }
    }

    // ---- 连接 ----

    private fun connectToDevice(device: BluetoothDevice) {
        _state.value = _state.value.copy(
            phase = Phase.CONNECTING,
            deviceName = device.name ?: DEVICE_NAME_PREFIX,
            needPairing = false
        )
        try {
            // autoConnect=false: 已扫到广播, 直连最快 (30s 连接窗口,
            // 失败/断线由 scheduleReconnect 重新扫描兜底)
            gatt = device.connectGatt(null, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: Exception) {
            Log.w(TAG, "connectGatt failed: ${e.message}")
            gatt = null
            scheduleReconnect()
        }
    }

    private fun disconnectGatt() {
        try {
            gatt?.disconnect()
            gatt?.close()
        } catch (_: Exception) { }
        gatt = null
    }

    /**
     * 反射调用隐藏 API BluetoothGatt.refresh()，强制 Android 丢弃该设备的
     * GATT 服务缓存（固件升级后服务表变化时必须）。部分系统未实现则返回 false。
     */
    private fun refreshGattCache(g: BluetoothGatt): Boolean {
        return try {
            val m = g.javaClass.getMethod("refresh")
            m.invoke(g) as? Boolean ?: false
        } catch (e: Exception) {
            Log.w(TAG, "refresh() not available: ${e.message}")
            false
        }
    }

    private fun scheduleReconnect() {
        if (!running) return
        val delay = BACKOFF_SECONDS[retry.coerceAtMost(BACKOFF_SECONDS.size - 1)] * 1000
        retry++
        controlRetries = 0
        _state.value = _state.value.copy(phase = Phase.RECONNECTING, retryCount = retry)
        handler.postDelayed({
            if (!running) return@postDelayed
            disconnectGatt()
            // 优先 autoConnect 定向重连；没有历史地址才扫描
            if (lastAddr != null) connectKnown() else startScan()
        }, delay)
    }

    /**
     * 定向 autoConnect 重连：已知键盘地址时不再依赖主动扫描。
     * autoConnect=true 把重连意图交给系统蓝牙进程在后台持续维护，
     * 锁屏/Doze 下也能恢复连接，且免去后台低效扫描被系统节流的问题。
     */
    private fun connectKnown() {
        val addr = lastAddr
        if (addr == null) { startScan(); return }
        val dev = try { bluetoothAdapter?.getRemoteDevice(addr) }
        catch (_: IllegalArgumentException) { null }
        if (dev == null) { startScan(); return }
        _state.value = _state.value.copy(
            phase = Phase.CONNECTING,
            deviceName = DEVICE_NAME_PREFIX,
            needPairing = false
        )
        disconnectGatt()
        try {
            gatt = dev.connectGatt(null, true, gattCallback, BluetoothDevice.TRANSPORT_LE)
            armAutoWatchdog()
        } catch (e: Exception) {
            Log.w(TAG, "auto connect failed: ${e.message}")
            gatt = null
            scheduleReconnect()
        }
    }

    /** autoConnect 时设备若关机/远离不会回调，定时检查：
     *  链路已建立 -> 等激活流程跑完即可；屏幕点亮(可扫描) -> 扫描兜底一轮，
     *  防键盘换地址/被遗忘；屏幕熄灭(锁屏) -> 维持 auto 让系统后台重连。 */
    private val autoWatchdog = Runnable {
        if (!running) return@Runnable
        val phase = _state.value.phase
        if (phase == Phase.CONNECTED || phase == Phase.STOPPED) return@Runnable
        if (phase == Phase.SCANNING || phase == Phase.RECONNECTING) return@Runnable // 由各自超时接管
        if (connEstablished) { armAutoWatchdog(); return@Runnable }  // 链路在，等激活
        val interactive = (appContext?.getSystemService(Context.POWER_SERVICE) as? PowerManager)
            ?.isInteractive == true
        if (interactive) {
            disconnectGatt()
            startScan()
        } else {
            armAutoWatchdog()   // 锁屏：系统继续后台重连，过会再看
        }
    }

    private fun armAutoWatchdog() {
        handler.removeCallbacks(autoWatchdog)
        handler.postDelayed(autoWatchdog, AUTO_WATCHDOG_MS)
    }

    // ---- GATT 回调 ----

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.d(TAG, "connState status=0x${Integer.toHexString(status)} newState=$newState")
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    cacheRefreshTries = 0
                    connEstablished = true
                    // 记住地址：之后断线重连走 autoConnect，不依赖后台扫描
                    lastAddr = g.device.address
                    prefs?.edit()?.putString(PREFS_KEY_ADDR, g.device.address)?.apply()
                    handler.postDelayed({
                        try { g.discoverServices() } catch (_: Exception) { }
                    }, DISCOVER_DELAY_MS)
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    if (!running) return
                    connEstablished = false
                    handler.removeCallbacks(autoWatchdog)
                    // status=133 常见于设备拒绝连接/链路错误
                    val rejected = (status == 8 || status == 19 || status == 133)
                    _state.value = _state.value.copy(
                        phase = Phase.RECONNECTING,
                        rssi = null,
                        needPairing = rejected || _state.value.needPairing
                    )
                    scheduleReconnect()
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            Log.d(TAG, "servicesDiscovered status=$status")
            if (status != BluetoothGatt.GATT_SUCCESS) {
                scheduleReconnect(); return
            }
            val svc = g.getService(java.util.UUID.fromString(SERVICE_UUID))
            if (svc == null) {
                // Android 会缓存已配对设备的 GATT 服务表；固件升级后新服务
                // 不在缓存里 -> 调用隐藏 API refresh() 强制刷新缓存后重新发现
                cacheRefreshTries++
                if (cacheRefreshTries <= 5) {
                    Log.w(TAG, "prox service not in cache, refresh #$cacheRefreshTries")
                    refreshGattCache(g)
                    handler.postDelayed({
                        try { g.discoverServices() } catch (_: Exception) { }
                    }, 400)
                } else {
                    Log.e(TAG, "prox service still missing after refresh; " +
                            "请在手机系统蓝牙里取消配对后重试")
                    scheduleReconnect()
                }
                return
            }
            // 1) 订阅 Status notify（CCCD 写完成回调里再写 Control，GATT 单请求串行）
            val chStatus = svc.getCharacteristic(java.util.UUID.fromString(STATUS_UUID))
            if (chStatus != null) {
                g.setCharacteristicNotification(chStatus, true)
                val cccd = chStatus.descriptors.firstOrNull()
                if (cccd != null) {
                    cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(cccd)
                    return
                }
            }
            // 2) 无 CCCD 时直接激活监测
            writeControl(g)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            Log.d(TAG, "descriptorWrite status=$status")
            // CCCD 订阅完成 -> 现在才写 Control 0x01
            if (d.uuid.toString().uppercase().contains("2902")) {
                writeControl(g)
            }
        }

        private fun writeControl(g: BluetoothGatt) {
            val chControl = g.getService(java.util.UUID.fromString(SERVICE_UUID))
                ?.getCharacteristic(java.util.UUID.fromString(CONTROL_UUID))
            if (chControl == null) {
                scheduleReconnect(); return
            }
            chControl.value = byteArrayOf(0x01)
            if (!g.writeCharacteristic(chControl)) {
                Log.w(TAG, "writeCharacteristic returned false, retry later")
                handler.postDelayed({ writeControl(g) }, 1000)
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (characteristic.uuid.toString() != CONTROL_UUID) return
            when {
                status == BluetoothGatt.GATT_SUCCESS -> {
                    retry = 0
                    controlRetries = 0
                    _state.value = _state.value.copy(
                        phase = Phase.CONNECTED,
                        needPairing = false,
                        deviceName = g.device.name ?: DEVICE_NAME_PREFIX
                    )
                }
                // 第一次写会触发链路配对/加密，配对完成后必须重写才能激活
                status == BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION ||
                status == BluetoothGatt.GATT_INSUFFICIENT_ENCRYPTION -> {
                    _state.value = _state.value.copy(needPairing = true)
                    if (controlRetries < 6) {
                        controlRetries++
                        Log.d(TAG, "link not encrypted yet, retry #$controlRetries in 1.5s")
                        handler.postDelayed({ writeControl(g) }, 1500)
                    } else {
                        Log.w(TAG, "control write kept failing, reconnecting")
                        scheduleReconnect()
                    }
                }
                else -> {
                    Log.w(TAG, "control write status=$status")
                    if (controlRetries < 3) {
                        controlRetries++
                        handler.postDelayed({ writeControl(g) }, 1000)
                    } else {
                        scheduleReconnect()
                    }
                }
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid.toString() != STATUS_UUID) return
            val v = characteristic.value ?: return
            if (v.size < 2) return
            val rssi = v[0].toInt()          // int8
            val proxState = v[1].toInt() and 0xFF
            _state.value = _state.value.copy(rssi = rssi, proxState = proxState)
        }
    }
}
