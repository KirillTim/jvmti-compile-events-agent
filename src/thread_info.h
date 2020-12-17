#ifndef PERF_MAP_AGENT_THREAD_H
#define PERF_MAP_AGENT_THREAD_H

#include <string>
#include <thread>
#include <unordered_map>

#include <jvmti.h>

using namespace std;

int os_thread_id(const void *vm_thread);

string pthread_name(pthread_t pthread);

uint64_t pthread_id(pthread_t pthread);

jvmtiThreadInfo get_thread_info(jvmtiEnv *jvmti, jthread thread);

string jthread_name(jvmtiEnv *jvmti, jthread thread);

int get_os_tid(JNIEnv *env, jthread thread);

unordered_map<int, string> java_threads_mach_tid_to_name(jvmtiEnv *jvmti, JNIEnv_ *jni_env);

#endif //PERF_MAP_AGENT_THREAD_H
