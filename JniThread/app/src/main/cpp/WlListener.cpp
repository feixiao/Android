//
// Created by ywl5320
//

#include "WlListener.h"
#include "WlAndroidLog.h"

WlListener::WlListener(JavaVM *vm, _JNIEnv *env, jobject obj) {
    jvm = vm;
    jenv = env;
    jobj = obj;
    jclass clz = env->GetObjectClass(jobj);
    if(!clz)
    {
        LOGE("get jclass wrong");
        return;
    }
    jmid = env->GetMethodID(clz, "onError", "(ILjava/lang/String;)V");
    if(!jmid)
    {
        LOGE("get jmethodID wrong");
        return;
    }
}

/**
 *
 * @param type  0：env线程 1：子线程
 * @param code
 * @param msg
 */
void WlListener::onError(int type, int code, const char *msg) {
    if(type == 0)
    {
        jstring jmsg = jenv->NewStringUTF(msg);
        jenv->CallVoidMethod(jobj, jmid, code, jmsg);
        jenv->DeleteLocalRef(jmsg);
    }
    else if(type == 1)
    {
        JNIEnv *env;
        jvm->AttachCurrentThread(&env, 0);

        jclass list_jcs = env->FindClass("java/util/ArrayList");
        if (list_jcs == NULL) {
            LOGE("ArrayList no  find!");
        }else {
            LOGD("Could find class, ArrayList");
        }


        jclass person_cls = env->FindClass("com/ywl5320/jnithread/Person");
        if (person_cls == nullptr) {
            LOGE("Could not find class, Person");
        } else {
            LOGD("Could find class, Person");
        }

        jstring jmsg = env->NewStringUTF(msg);
        env->CallVoidMethod(jobj, jmid, code, jmsg);
        env->DeleteLocalRef(jmsg);

        jvm->DetachCurrentThread();
    }
}
