# ProGuard rules for LiteRT / TensorFlow Lite Task Vision
-keep class com.google.aiedge.objectdetectordemo.** { *; }
-keep class org.tensorflow.lite.** { *; }
-keep class org.tensorflow.lite.task.vision.** { *; }
-keep class org.tensorflow.lite.task.core.** { *; }
-keep class org.tensorflow.lite.gpu.** { *; }
-keep class org.tensorflow.lite.nnapi.** { *; }

# Keep native JNI methods
-keepclasseswithmembernames class * {
    native <methods>;
}
