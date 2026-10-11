package local.lanaudio

internal object NativeAudio {
    init { System.loadLibrary("lanaudio") }

    external fun nativeStartReceiver(host: String, port: Int, targetMs: Int): Boolean
    external fun nativeStopReceiver()
    external fun nativeSetTargetBuffer(targetMs: Int)
    external fun nativeStats(): String
}
