#include <jni.h>
#include "receiver.h"

namespace {
using lanaudio::Session;
std::mutex globalMutex;
std::unique_ptr<Session> globalSession;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_local_lanaudio_NativeAudio_nativeStartReceiver(JNIEnv* env, jobject, jstring host, jint port) {
    std::lock_guard<std::mutex> lock(globalMutex);
    if (port < 1 || port > 65535) return JNI_FALSE;
    globalSession.reset();
    const char* text = env->GetStringUTFChars(host, nullptr);
    if (!text) return JNI_FALSE;
    auto session = std::make_unique<Session>(text, port);
    env->ReleaseStringUTFChars(host, text);
    if (!session->start()) return JNI_FALSE;
    globalSession = std::move(session); return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL
Java_local_lanaudio_NativeAudio_nativeStopReceiver(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(globalMutex); globalSession.reset();
}
extern "C" JNIEXPORT jstring JNICALL
Java_local_lanaudio_NativeAudio_nativeStats(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(globalMutex);
    const auto s = globalSession ? globalSession->stats() : "未连接";
    return env->NewStringUTF(s.c_str());
}
