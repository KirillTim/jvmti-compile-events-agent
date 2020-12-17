#include "thread_info.h"
#include "utils.h"
#include "logger.h"

static const int _thread_osthread_offset = 296; //TODO read from libjvm.dylib
static const int _osthread_id_offset = 96;

int os_thread_id(const void *vm_thread) {
    const char *os_thread = *(const char **) (((const char *) vm_thread) + _thread_osthread_offset);
    return *(int *) (os_thread + _osthread_id_offset);
}

string pthread_name(pthread_t pthread) {
    char name[512] = {};
    pthread_getname_np(pthread, name, sizeof(name));
    return string(name);
}

uint64_t pthread_id(pthread_t pthread) {
    uint64_t pthread_id;
    pthread_threadid_np(pthread, &pthread_id);
    return pthread_id;
}

//may only be called during the live phase
jvmtiThreadInfo get_thread_info(jvmtiEnv *jvmti, jthread thread) {
    jvmtiThreadInfo info{};
    report_failed(jvmti->GetThreadInfo(thread, &info), "GetThreadInfo error");
    return info;
}

//may only be called during the live phase
string jthread_name(jvmtiEnv *jvmti, jthread thread) {
    jvmtiThreadInfo info = get_thread_info(jvmti, thread);
    if (info.name == nullptr) return "Unknown";
    string result = string(info.name);
    jvmti->Deallocate((unsigned char *) info.name);
    return result;
}

int get_os_tid(JNIEnv *env, jthread thread) {
    jclass threadClass = env->FindClass("java/lang/Thread");
    if (threadClass == nullptr) {
        log_file << "can't find class java/lang/Thread" << endl;
        return -1;
    }
    jfieldID eetop = env->GetFieldID(threadClass, "eetop", "J");
    if (eetop == nullptr) {
        log_file << "can't find field eetop" << endl;
        return -1;
    }
    const auto *vm_thread = (const void *) (uintptr_t) env->GetLongField(thread, eetop);
    return os_thread_id(vm_thread);
}

unordered_map<int, string> java_threads_mach_tid_to_name(jvmtiEnv *jvmti, JNIEnv_ *jni_env) {
    unordered_map<int, string> result;
    jint count = 0;
    jthread *threads = nullptr;
    jvmti->GetAllThreads(&count, &threads);
    if (count == 0 || threads == nullptr) {
        log_file << "java_threads_mach_tid_to_name: no threads" << endl;
        return result;
    }
    for (int i = 0; i < count; i++) {
        int tid = get_os_tid(jni_env, threads[i]);
        result[tid] = jthread_name(jvmti, threads[i]);
    }
    return result;
};