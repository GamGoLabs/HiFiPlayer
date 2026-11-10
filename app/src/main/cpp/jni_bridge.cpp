#include <jni.h>
#include <memory>
#include <vector>
#include "AudioEngine.h"
#include "Log.h"

static std::unique_ptr<AudioEngine> gEngine;
#define FN(name) Java_com_hifiplayer_audio_NativeAudioEngine_##name

static std::vector<int> toVec(JNIEnv* env, jintArray a) {
    std::vector<int> v;
    if (!a) return v;
    v.resize(static_cast<size_t>(env->GetArrayLength(a)));
    if (!v.empty()) env->GetIntArrayRegion(a, 0, static_cast<jint>(v.size()), v.data());
    return v;
}

extern "C" {
JNIEXPORT void JNICALL FN(nCreate)(JNIEnv*, jobject) { if (!gEngine) gEngine = std::make_unique<AudioEngine>(); }
JNIEXPORT void JNICALL FN(nDestroy)(JNIEnv*, jobject) { gEngine.reset(); }
JNIEXPORT jboolean JNICALL FN(nLoad)(JNIEnv*, jobject, jint fd) { return gEngine && gEngine->load(fd); }
JNIEXPORT jboolean JNICALL FN(nQueueNext)(JNIEnv*, jobject, jint fd) { return gEngine && gEngine->queueNext(fd); }
JNIEXPORT jboolean JNICALL FN(nPlay)(JNIEnv*, jobject) { return gEngine && gEngine->play(); }
JNIEXPORT void JNICALL FN(nPause)(JNIEnv*, jobject) { if (gEngine) gEngine->pause(); }
JNIEXPORT void JNICALL FN(nStop)(JNIEnv*, jobject) { if (gEngine) gEngine->stop(); }
JNIEXPORT jboolean JNICALL FN(nSeek)(JNIEnv*, jobject, jlong ms) { return gEngine && gEngine->seekMs(ms); }
JNIEXPORT jlong JNICALL FN(nPositionMs)(JNIEnv*, jobject) { return gEngine ? gEngine->positionMs() : 0; }
JNIEXPORT jlong JNICALL FN(nDurationMs)(JNIEnv*, jobject) { return gEngine ? gEngine->durationMs() : 0; }
JNIEXPORT jint JNICALL FN(nState)(JNIEnv*, jobject) { return gEngine ? gEngine->state() : 0; }
JNIEXPORT jint JNICALL FN(nTransitions)(JNIEnv*, jobject) { return gEngine ? gEngine->transitions() : 0; }
JNIEXPORT void JNICALL FN(nSetBitPerfect)(JNIEnv*, jobject, jboolean on) { if (gEngine) gEngine->setBitPerfect(on); }
JNIEXPORT void JNICALL FN(nSetVolume)(JNIEnv*, jobject, jfloat v) { if (gEngine) gEngine->setVolume(v); }

JNIEXPORT void JNICALL FN(nSetOutputPlan)(JNIEnv* env, jobject, jint deviceId, jintArray rates, jintArray formats,
                                          jint forcedRate, jint forcedFmt) {
    if (gEngine) gEngine->setOutputPlan(deviceId, toVec(env, rates), toVec(env, formats), forcedRate, forcedFmt);
}
JNIEXPORT jstring JNICALL FN(nOutputInfo)(JNIEnv* env, jobject) {
    return env->NewStringUTF(gEngine ? gEngine->outputInfo().c_str() : "");
}

JNIEXPORT jintArray JNICALL FN(nFormat)(JNIEnv* env, jobject) {
    StreamInfo i = gEngine ? gEngine->currentInfo() : StreamInfo{};
    jint v[3] = {(jint)i.sampleRate, (jint)i.channels, (jint)i.bitsPerSample};
    jintArray arr = env->NewIntArray(3);
    env->SetIntArrayRegion(arr, 0, 3, v);
    return arr;
}
JNIEXPORT jstring JNICALL FN(nDescribe)(JNIEnv* env, jobject) {
    return env->NewStringUTF(gEngine ? gEngine->describe().c_str() : "");
}
}
