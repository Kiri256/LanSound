package local.lanaudio

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.IBinder
import android.os.PowerManager

class AudioService : Service() {
    private var cpuLock: PowerManager.WakeLock? = null
    private var focusRequest: AudioFocusRequest? = null

    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, "LAN Audio ${BuildConfig.VERSION_NAME} 播放", NotificationManager.IMPORTANCE_LOW)
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            NativeAudio.nativeStopReceiver()
            stopSelf(startId)
            return START_NOT_STICKY
        }

        val host = intent?.getStringExtra(EXTRA_HOST) ?: run { stopSelf(startId); return START_NOT_STICKY }
        val port = intent.getIntExtra(EXTRA_PORT, 30010)
        val targetMs = intent.getIntExtra(EXTRA_TARGET_MS, 100)
        startForeground(NOTIFICATION_ID, notification())
        val audioManager = getSystemService(AudioManager::class.java)
        if (focusRequest == null) {
            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setOnAudioFocusChangeListener { change ->
                    if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) stopSelf()
                }.build()
            if (audioManager.requestAudioFocus(request) != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
                stopSelf(startId)
                return START_NOT_STICKY
            }
            focusRequest = request
        }
        acquireCpuLock()
        // Service callbacks are serialized on the main thread. Native start only
        // creates the receiver; opening the output happens on that receiver.
        running = NativeAudio.nativeStartReceiver(host, port, targetMs)
        if (!running) stopSelf(startId)
        return START_NOT_STICKY
    }

    private fun notification(): Notification {
        val stop = PendingIntent.getService(
            this,
            1,
            stopIntent(this),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("LAN Audio ${BuildConfig.VERSION_NAME}")
            .setContentText("正在接收 Windows 音频")
            .setOngoing(true)
            .addAction(android.R.drawable.ic_media_pause, "断开", stop)
            .build()
    }

    private fun acquireCpuLock() {
        if (cpuLock?.isHeld == true) return
        val manager = getSystemService(Context.POWER_SERVICE) as PowerManager
        cpuLock = manager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "LANAudio:audio").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    override fun onDestroy() {
        running = false
        NativeAudio.nativeStopReceiver()
        focusRequest?.let { getSystemService(AudioManager::class.java).abandonAudioFocusRequest(it) }
        focusRequest = null
        cpuLock?.let { if (it.isHeld) it.release() }
        cpuLock = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        @Volatile var running = false
            private set
        private const val CHANNEL = "lan_audio_playback"
        private const val NOTIFICATION_ID = 41
        private const val ACTION_START = "local.lanaudio.START"
        private const val ACTION_STOP = "local.lanaudio.STOP"
        private const val EXTRA_HOST = "host"
        private const val EXTRA_PORT = "port"
        private const val EXTRA_TARGET_MS = "target_ms"

        fun startIntent(context: Context, host: String, port: Int, targetMs: Int) = Intent(context, AudioService::class.java)
            .setAction(ACTION_START)
            .putExtra(EXTRA_HOST, host)
            .putExtra(EXTRA_PORT, port)
            .putExtra(EXTRA_TARGET_MS, targetMs)

        fun stopIntent(context: Context) = Intent(context, AudioService::class.java).setAction(ACTION_STOP)
    }
}
