#pragma once

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

// JNI function prototypes for package com.ai.audioagent.nativebridge.AudioAgentNative

JNIEXPORT jlong JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeCreate(
    JNIEnv* env,
    jobject thiz,
    jstring system_prompt
);

JNIEXPORT jboolean JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeStart(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeStop(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativePauseAudioIO(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

JNIEXPORT jboolean JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeResumeAudioIO(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeTriggerBargeIn(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSubmitUserQuery(
    JNIEnv* env,
    jobject thiz,
    jlong handle,
    jstring query
);

JNIEXPORT jint JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeGetState(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeOnThermalChanged(
    JNIEnv* env,
    jobject thiz,
    jlong handle,
    jint thermal_status
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSetModelPath(
    JNIEnv* env,
    jobject thiz,
    jlong handle,
    jstring model_path
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSetLlmModelPath(
    JNIEnv* env,
    jobject thiz,
    jlong handle,
    jstring model_path
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeSetLlmEngineMode(
    JNIEnv* env,
    jobject thiz,
    jlong handle,
    jint mode
);

JNIEXPORT jstring JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeGetLlmStats(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

JNIEXPORT jintArray JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeGetTopPerformanceCores(
    JNIEnv* env,
    jclass clazz,
    jint top_n
);

JNIEXPORT void JNICALL
Java_com_ai_audioagent_nativebridge_AudioAgentNative_nativeDestroy(
    JNIEnv* env,
    jobject thiz,
    jlong handle
);

#ifdef __cplusplus
}
#endif
