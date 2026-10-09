package local.lanaudio

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.inputmethod.InputMethodManager
import android.content.Context
import android.content.Intent
import android.content.ActivityNotFoundException
import android.provider.Settings
import android.widget.Toast
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.ArrayAdapter
import android.widget.AdapterView
import android.widget.ImageButton
import android.view.View
import android.text.InputType

class MainActivity : Activity() {
    private lateinit var address: EditText
    private lateinit var port: EditText
    private lateinit var connect: Button
    private lateinit var status: TextView
    private lateinit var metrics: TextView
    private lateinit var servers: Spinner
    private lateinit var refresh: ImageButton
    private lateinit var scanStatus: TextView
    private lateinit var serverLabels: ArrayAdapter<String>
    private val foundServers = mutableListOf<DiscoveredServer>()
    private var discovery: ServerDiscovery? = null
    private val handler = Handler(Looper.getMainLooper())
    private var active = false
    private val preferences by lazy { getSharedPreferences(PREFERENCES, MODE_PRIVATE) }

    private val poll = object : Runnable {
        override fun run() {
            updateConnection(AudioService.running)
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = Color.rgb(17, 23, 25)
        window.navigationBarColor = Color.rgb(17, 23, 25)
        window.decorView.systemUiVisibility = 0

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(34), dp(24), dp(24))
            setBackgroundColor(Color.rgb(17, 23, 25))
        }
        root.addView(TextView(this).apply {
            text = "LAN Audio ${BuildConfig.VERSION_NAME}"
            textSize = 24f
            setTextColor(Color.rgb(239, 243, 245))
            typeface = Typeface.DEFAULT_BOLD
        }, match())
        root.addView(TextView(this).apply {
            text = "WINDOWS SYSTEM AUDIO  |  UNCOMPRESSED PCM"
            textSize = 12f
            setTextColor(Color.rgb(126, 214, 189))
            setPadding(0, dp(6), 0, dp(20))
        }, match())

        root.addView(label("USB 网络服务器"))
        val scanRow = LinearLayout(this)
        serverLabels = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, mutableListOf("选择服务器"))
        servers = Spinner(this).apply {
            adapter = serverLabels
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    val server = foundServers.getOrNull(position - 1) ?: return
                    setSelection(0)
                    if (active) return
                    if (!server.compatible) {
                        scanStatus.text = "音频协议或包大小不兼容"
                        return
                    }
                    address.setText(server.host)
                    port.setText(server.port.toString())
                    toggle()
                }
            }
        }
        scanRow.addView(servers, LinearLayout.LayoutParams(0, dp(48), 1f))
        refresh = ImageButton(this).apply {
            setImageResource(android.R.drawable.ic_popup_sync)
            contentDescription = "刷新服务器"
            tooltipText = "刷新服务器"
            setOnClickListener { scanServers() }
        }
        scanRow.addView(refresh, LinearLayout.LayoutParams(dp(48), dp(48)))
        root.addView(scanRow, match())
        scanStatus = label("未扫描")
        root.addView(scanStatus, match())
        root.addView(label("服务器 IPv4 地址"))
        address = EditText(this).apply {
            hint = "192.168.1.10"
            setText(preferences.getString(KEY_HOST, "") ?: "")
            setSingleLine(true)
            textSize = 16f
            inputType = InputType.TYPE_CLASS_PHONE
            setTextColor(Color.WHITE)
            setHintTextColor(Color.rgb(130, 145, 147))
            setPadding(dp(12), 0, dp(12), 0)
            setBackgroundColor(Color.rgb(26, 34, 36))
        }
        root.addView(address, LinearLayout.LayoutParams(-1, dp(48)))

        root.addView(label("UDP 端口").apply { setPadding(0, dp(20), 0, dp(8)) })
        port = EditText(this).apply {
            setText(preferences.getString(KEY_PORT, DEFAULT_PORT.toString()) ?: DEFAULT_PORT.toString())
            setSingleLine(true)
            textSize = 16f
            inputType = InputType.TYPE_CLASS_NUMBER
            setTextColor(Color.WHITE)
            setPadding(dp(12), 0, dp(12), 0)
            setBackgroundColor(Color.rgb(26, 34, 36))
        }
        root.addView(port, LinearLayout.LayoutParams(-1, dp(48)))

        connect = Button(this).apply {
            text = "连接"
            isAllCaps = false
            textSize = 15f
            setTextColor(Color.rgb(16, 32, 28))
            setBackgroundColor(Color.rgb(126, 214, 189))
            setOnClickListener { toggle() }
        }
        root.addView(connect, LinearLayout.LayoutParams(-1, dp(48)).apply { topMargin = dp(24) })

        status = TextView(this).apply {
            text = "未连接"
            textSize = 14f
            setTextColor(Color.rgb(189, 199, 200))
            setPadding(0, dp(24), 0, dp(6))
        }
        root.addView(status, match())
        metrics = TextView(this).apply {
            text = ""
            textSize = 12f
            setTextColor(Color.rgb(155, 168, 170))
        }
        root.addView(ScrollView(this).apply { addView(metrics) }, LinearLayout.LayoutParams(-1, 0, 1f))
        root.addView(Button(this).apply {
            text = "USB 网络共享设置"
            isAllCaps = false
            textSize = 14f
            setOnClickListener { openTetherSettings() }
        }, LinearLayout.LayoutParams(-1, dp(48)))
        setContentView(root)
    }

    private fun openTetherSettings() {
        for (action in listOf("android.settings.TETHER_SETTINGS", Settings.ACTION_WIRELESS_SETTINGS, Settings.ACTION_SETTINGS)) {
            try {
                startActivity(Intent(action))
                return
            } catch (_: ActivityNotFoundException) {
                // Some vendors omit the dedicated tethering settings activity.
            } catch (_: SecurityException) {
                // A vendor may expose the activity only to system applications.
            }
        }
        Toast.makeText(this, "无法打开系统设置", Toast.LENGTH_SHORT).show()
    }

    private fun toggle() {
        if (active) {
            stopAudio()
            return
        }
        val host = address.text.toString().trim()
        val portNumber = port.text.toString().toIntOrNull()
        if (host.isBlank() || portNumber == null || portNumber !in 1..65535) {
            status.text = "请输入有效的服务器地址和端口"
            return
        }
        preferences.edit()
            .putString(KEY_HOST, host)
            .putString(KEY_PORT, portNumber.toString())
            .apply()
        (getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager)
            .hideSoftInputFromWindow(address.windowToken, 0)
        stopDiscovery()
        startForegroundService(AudioService.startIntent(this, host, portNumber))
        updateConnection(true, "正在连接并预热音频缓冲")
    }

    private fun stopAudio() {
        stopService(AudioService.stopIntent(this))
        updateConnection(false, "已断开")
        scanServers()
    }

    private fun updateConnection(connected: Boolean, message: String? = null) {
        active = connected
        address.isEnabled = !connected
        port.isEnabled = !connected
        servers.isEnabled = !connected
        refresh.isEnabled = !connected
        if (connected) stopDiscovery()
        connect.text = if (connected) "断开" else "连接"
        metrics.text = if (connected && message == null) NativeAudio.nativeStats() else ""
        status.text = message ?: when {
            !connected -> "未连接"
            metrics.text.contains("output_hz=") -> "正在接收音频"
            else -> "等待服务器音频 / 正在恢复连接"
        }
    }

    override fun onDestroy() {
        stopDiscovery()
        handler.removeCallbacks(poll)
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        handler.removeCallbacks(poll)
        handler.post(poll)
        updateConnection(AudioService.running)
        if (!active) scanServers()
    }

    override fun onPause() {
        stopDiscovery()
        handler.removeCallbacks(poll)
        super.onPause()
    }

    private fun scanServers() {
        if (active) return
        stopDiscovery()
        foundServers.clear()
        serverLabels.clear()
        serverLabels.add("选择服务器")
        servers.setSelection(0)
        scanStatus.text = "正在扫描…"
        lateinit var scan: ServerDiscovery
        scan = ServerDiscovery(address.text.toString().trim(), { server ->
            handler.post {
                if (discovery !== scan || active) return@post
                if (foundServers.size < 64 && foundServers.none { it.host == server.host && it.port == server.port }) {
                    foundServers.add(server)
                    serverLabels.add(server.label)
                    scanStatus.text = "已发现 ${foundServers.size} 台服务器 · 正在扫描"
                }
            }
        }, { error ->
            handler.post {
                if (discovery !== scan) return@post
                discovery = null
                scanStatus.text = error?.let { "扫描失败：$it" }
                    ?: if (foundServers.isEmpty()) "未发现服务器" else "已发现 ${foundServers.size} 台服务器"
            }
        })
        discovery = scan
        scan.start()
    }

    private fun stopDiscovery() {
        discovery?.close()
        if (discovery != null) scanStatus.text = "扫描已停止"
        discovery = null
    }

    private fun label(value: String) = TextView(this).apply {
        text = value
        textSize = 13f
        setTextColor(Color.rgb(189, 199, 200))
        setPadding(0, 0, 0, dp(8))
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun match() = LinearLayout.LayoutParams(-1, -2)

    companion object {
        private const val PREFERENCES = "lan_audio_preferences"
        private const val KEY_HOST = "server_host"
        private const val KEY_PORT = "server_port"
        private const val DEFAULT_PORT = 30010
    }

}
