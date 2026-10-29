# ProGuard rules for AudioAgentDemo

# Keep JNI native bridge classes and methods
-keepclasseswithmembernames class * {
    native <methods>;
}

-keep class com.ai.audioagent.nativebridge.** { *; }
-keep class com.ai.audioagent.tools.** { *; }
