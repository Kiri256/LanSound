package local.lanaudio

internal object NativeAudio {
    init { System.loadLibrary("lanaudio") }

    external fun nativeStartReceiver(host: String, port: Int): Boolean
    external fun nativeStopReceiver()
    external fun nativeStats(): String
}
