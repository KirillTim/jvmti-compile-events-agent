#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include <mach/mach_init.h>
#include <mach/task.h>
#include <jvmti.h>
#include <jvmticmlr.h>

#include "logger.h"
#include "thread_info.h"
#include "utils.h"

using namespace std;
using namespace std::chrono;

JavaVM *_vm = nullptr; //one for all threads
jvmtiEnv *_jvmti = nullptr; //one for agent lifetime
mutex jvmti_mutex;

ofstream events_file;
mutex events_file_mutex; //not sure about ofstream buffers and flush atomicity

atomic<int> attach_count(0);

//ms
atomic<long> total_agent_time(0);
atomic<long> total_get_threads_info_time(0);
atomic<long> agent_IO_time(0);

atomic<long> sig_string_time(0);

atomic<long> single_time(0);
atomic<long> unfolded_time(0);
atomic<long> loop_time(0);

atomic<long> cb_compiled_time(0);

atomic<int> unfolded;
atomic<int> single;

template<typename Func>
void measure_total_agent_time(Func block) {
    auto start = system_clock::now();
    block();
    total_agent_time += duration_cast<milliseconds>(system_clock::now() - start).count();
}

JNIEnv *get_JNI(JavaVM *vm) { //one per thread
    JNIEnv *jni;
    jint err = vm->GetEnv((void **) &jni, JNI_VERSION_1_6);
    if (err != JNI_OK) { //jni will be null
        log_file << "get_JNI error: " << err << endl;
    }
    return jni;
}

void write_compiled_method_load_event_entry(const void *code_addr, int code_size, const char *entry) {
    auto start = system_clock::now();
    lock_guard<mutex> guard(events_file_mutex);
    milliseconds ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    events_file << ms.count() << " method_load: "
                << my_formatter("0x%llx %d %s", (unsigned long long) code_addr, code_size, entry) << endl;
    agent_IO_time += duration_cast<milliseconds>(system_clock::now() - start).count();
}

void write_compilede_method_unload_event_entry(const void *code_addr, const char *entry) {
    auto start = system_clock::now();
    lock_guard<mutex> guard(events_file_mutex);
    milliseconds ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    events_file << ms.count() << " method_unload: "
                << my_formatter("0x%llx %s", (unsigned long long) code_addr, entry) << endl;
    agent_IO_time += duration_cast<milliseconds>(system_clock::now() - start).count();
}

void write_thread_entry(const uint64_t native_tid, const int mach_id, const string &thread_name) {
    auto start = system_clock::now();
    lock_guard<mutex> guard(events_file_mutex);
    milliseconds ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    events_file << ms.count() << " thread: "
                << my_formatter("%llu 0x%x %s", native_tid, mach_id, thread_name.c_str()) << endl;
    agent_IO_time += duration_cast<milliseconds>(system_clock::now() - start).count();
}

void generate_single_entry(jvmtiEnv *jvmti, jmethodID method, const void *code_addr, jint code_size) {
    single++;
    auto start = system_clock::now();
    string entry = sig_string(jvmti, method);
    //sig_string will return empty string for events sent BEFORE and processed AFTER jvmti->DisposeEnvironment() in shutdown() is called
    if (entry.empty()) return;
    write_compiled_method_load_event_entry(code_addr, code_size, entry.c_str());
    single_time += duration_cast<milliseconds>(system_clock::now() - start).count();
}

void write_unfolded_entry(
        jvmtiEnv *jvmti,
        PCStackInfo *info,
        const void *start_addr,
        const void *end_addr) {
    unfolded++;
    auto start = system_clock::now();
    string result;
    for (int i = info->numstackframes - 1; i >= 0; i--) {
        //sig_string will return empty string for events sent BEFORE and processed AFTER jvmti->DisposeEnvironment() in shutdown() is called
        string signature = sig_string(jvmti, info->methods[i]);
        if (signature.empty()) return;
        result += signature;
        if (i > 0) result += "->";
    }
    auto code_size = (int) ((unsigned long long) end_addr - (unsigned long long) start_addr);
    write_compiled_method_load_event_entry(start_addr, code_size, result.c_str());
    unfolded_time += duration_cast<milliseconds>(system_clock::now() - start).count();
}

void generate_unfolded_entries(
        jvmtiEnv *jvmti,
        jmethodID root_method,
        const void *code_addr,
        jint code_size,
        const void *compile_info) {
    const auto *header = static_cast<const jvmtiCompiledMethodLoadRecordHeader *>(compile_info);
    //sig_string will return empty string for events sent BEFORE and processed AFTER jvmti->DisposeEnvironment() in shutdown() is called
    string root_name = sig_string(jvmti, root_method);
    if (root_name.empty()) return;

    if (header->kind != JVMTI_CMLR_INLINE_INFO) {
        generate_single_entry(jvmti, root_method, code_addr, code_size);
        return;
    }

    const auto *record = (jvmtiCompiledMethodLoadInlineRecord *) header;
    const void *start_addr = code_addr;
    jmethodID cur_method = root_method;

    // walk through the method meta data per PC to extract address range
    // per inlined method.
    auto start = system_clock::now();
    int i;
    for (i = 0; i < record->numpcs; i++) {
        PCStackInfo *info = &record->pcinfo[i];
        jmethodID top_method = info->methods[0];

        // as long as the top method remains the same we delay recording
        if (cur_method != top_method) {
            // top method has changed, record the range for current method
            void *end_addr = info->pc;

            if (i > 0) {
                write_unfolded_entry(jvmti, &record->pcinfo[i - 1], start_addr, end_addr);
            } else {
                auto single_code_size = (int) ((unsigned long long) end_addr - (unsigned long long) start_addr);
                generate_single_entry(jvmti, root_method, start_addr, single_code_size);
            }

            start_addr = info->pc;
            cur_method = top_method;
        }
    }
    loop_time += duration_cast<milliseconds>(system_clock::now() - start).count();
    // record the last range if there's a gap
    if ((unsigned long long) start_addr != (unsigned long long) code_addr + (unsigned long long) code_size) {
        // end_addr is end of this complete code blob
        unsigned long long end_addr = (unsigned long long) code_addr + code_size;

        if (i > 0)
            write_unfolded_entry(jvmti, &record->pcinfo[i - 1], start_addr,
                                 reinterpret_cast<const void *>(end_addr));
        else {
            auto single_code_size = (int) (end_addr - (unsigned long long) start_addr);
            generate_single_entry(jvmti, root_method, start_addr, single_code_size);
        }
    }
}

//may only be called during the live phase
void print_all_vm_threads(jvmtiEnv *jvmti, JNIEnv_ *jni_env) {
    auto known_java_threads = java_threads_mach_tid_to_name(jvmti, jni_env);
    mach_msg_type_number_t count;
    thread_act_array_t list;
    task_threads(mach_task_self(), &list, &count);
    for (int i = 0; i < count; i++) {
        pthread_t pthread = pthread_from_mach_thread_np(list[i]);
        int mach_tid = list[i];
        uint64_t native_tid = pthread_id(pthread);
        string thread_name;
        auto java_name = known_java_threads.find(mach_tid);
        if (java_name != known_java_threads.end()) {
            thread_name = "java: " + java_name->second;
        } else {
            string native_name = pthread_name(pthread);
            if (starts_with(native_name, "Java: ")) {
                thread_name = "java: " + native_name.substr(string("Java: ").size());
            } else {
                thread_name = "native: " + (!native_name.empty() ? native_name : to_string(native_tid));
            }
        }
        write_thread_entry(native_tid, mach_tid, thread_name);
    }
}

static void JNICALL
cbCompiledMethodLoad(jvmtiEnv *jvmti,
                     jmethodID method,
                     jint code_size,
                     const void *code_addr,
                     jint map_length,
                     const jvmtiAddrLocationMap *map,
                     const void *compile_info) {
    auto start = system_clock::now();
    //measure_total_agent_time([=]() {
    if (compile_info != nullptr)
        generate_unfolded_entries(jvmti, method, code_addr, code_size, compile_info);
    else
        generate_single_entry(jvmti, method, code_addr, code_size);
    //});
    cb_compiled_time += duration_cast<milliseconds>(system_clock::now() - start).count();
}

void JNICALL
cbCompiledMethodUnload(jvmtiEnv *jvmti,
                       jmethodID method,
                       const void *code_addr) {
    //sig_string will return empty string for events sent BEFORE and processed AFTER jvmti->DisposeEnvironment() in shutdown() is called
    string entry = sig_string(jvmti, method);
    if (entry.empty()) return;
    write_compilede_method_unload_event_entry(code_addr, entry.c_str());
}

static void JNICALL
cbDynamicCodeGenerated(jvmtiEnv *jvmti,
                       const char *name,
                       const void *address,
                       jint length) {
    write_compiled_method_load_event_entry(address, length, name);
}

void print_jthread(jvmtiEnv *jvmti, jthread thread, pthread_t pthread, const string &msg_prefix = "") {
    int mach_tid = pthread_mach_thread_np(pthread);
    uint64_t native_tid = pthread_id(pthread);
    string thread_name = "java: " + jthread_name(jvmti, thread);
    write_thread_entry(native_tid, mach_tid, thread_name);
}

static void JNICALL
cbThreadStart(jvmtiEnv *jvmti,
              JNIEnv *jni_env,
              jthread thread) {
    pthread_t pthread = pthread_self(); //callback is called on newly started thread
    print_jthread(jvmti, thread, pthread, "cbThreadStart");
}

void JNICALL
cbThreadEnd(jvmtiEnv *jvmti,
            JNIEnv *jni_env,
            jthread thread) {
    //if thread was renamed report the last name
    pthread_t pthread = pthread_self();
    print_jthread(jvmti, thread, pthread, "cbThreadEnd");
}

vector<jvmtiEvent> EVENTS_LISTEN_TO{
        JVMTI_EVENT_COMPILED_METHOD_LOAD,
        JVMTI_EVENT_COMPILED_METHOD_UNLOAD,
        JVMTI_EVENT_DYNAMIC_CODE_GENERATED,
        JVMTI_EVENT_THREAD_START,
        JVMTI_EVENT_THREAD_END,
        JVMTI_EVENT_VM_DEATH
};

jvmtiError enable_notifications(jvmtiEnv *jvmti) {
    for (auto event: EVENTS_LISTEN_TO) {
        jvmtiError err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, event, nullptr);
        if (err != JVMTI_ERROR_NONE) return err;
    }
    return JVMTI_ERROR_NONE;
}

void disable_notifications(jvmtiEnv *jvmti) {
    for (auto event: EVENTS_LISTEN_TO) {
        report_failed(jvmti->SetEventNotificationMode(JVMTI_DISABLE, event, nullptr),
                      "SetEventNotificationMode(JVMTI_DISABLE)");
    }
}

static bool is_live_phase(jvmtiEnv *jvmti) {
    jvmtiPhase phase;
    if (report_failed(jvmti->GetPhase(&phase), "jvmti->GetPhase")) return false;
    return phase == JVMTI_PHASE_LIVE;
}

static void shutdown(jvmtiEnv *jvmti, JNIEnv_ *jni_env, bool force, bool vm_death) {
    if (jvmti != nullptr) {
        if (is_live_phase(jvmti) && jni_env != nullptr) {
            print_all_vm_threads(jvmti, jni_env);
        }
        if (!force && !vm_death) {

        }
        if (!vm_death) {
            //FIXME: from doc of DisposeEvent: Events enabled by this environment will no longer be sent, however event handlers currently running will continue to run.
            disable_notifications(jvmti);
        }
        report_failed(jvmti->DisposeEnvironment(), "Can not dispose jvmti environment. WHAT THE FUCK?!");
    }
    log_file << "total time in agent code: " << total_agent_time << "ms" << endl;
    log_file << "total time in get all thread info code: " << total_get_threads_info_time << "ms" << endl;
    log_file << "total time in agent IO code: " << agent_IO_time << "ms" << endl;
    total_agent_time = 0;
    total_get_threads_info_time = 0;
    agent_IO_time = 0;
    log_file.close();
    events_file.close();
    lock_guard<mutex> lock(jvmti_mutex);
    _vm = nullptr;
    _jvmti = nullptr;
}

jthread new_thread(JNIEnv *env, const char *threadName) {
    jclass thrClass = env->FindClass("java/lang/Thread");
    if (thrClass == nullptr) {
        log_file << "can't find class java/lang/Thread" << endl;
        return nullptr;
    }
    jmethodID cid = env->GetMethodID(thrClass, "<init>", "()V");
    if (cid == nullptr) {
        log_file << "can't find thread constructor" << endl;
        return nullptr;
    }
    jthread thread = env->NewObject(thrClass, cid);
    if (thread == nullptr) {
        log_file << "can't create new Thread object" << endl;
        return nullptr;
    }
    jmethodID mid = env->GetMethodID(thrClass, "setName", "(Ljava/lang/String;)V");
    env->CallObjectMethod(thread, mid, env->NewStringUTF(threadName));
    return thread;
}

jvmtiError load_previous_events(jvmtiEnv *jvmti) {
    jvmtiError err = jvmti->GenerateEvents(JVMTI_EVENT_COMPILED_METHOD_LOAD);
    if (err != JVMTI_ERROR_NONE) return err;
    return jvmti->GenerateEvents(JVMTI_EVENT_DYNAMIC_CODE_GENERATED);
}

static void events_logger_function(jvmtiEnv *jvmti, JNIEnv *jni_env, void *arg) {
    log_file << "events_logger_function started" << endl;
    //dump all known threads first
    print_all_vm_threads(jvmti, jni_env);
    auto start = system_clock::now();
    report_failed(load_previous_events(jvmti), "load_previous_events error");
    auto total_load_events = duration_cast<milliseconds>(system_clock::now() - start).count();
    log_file << "single = " << single << endl;
    log_file << "unfolded = " << unfolded << endl;
    log_file << "total load events = " << total_load_events << " ms" << endl;
    log_file << "single_time = " << single_time << " ms" << endl;
    log_file << "unfolded_time = " << unfolded_time << " ms" << endl;
    log_file << "sig_string_time = " << sig_string_time << " ms" << endl;
    log_file << "loop_time = " << loop_time << " ms" << endl;
    log_file << "cb_compiled_time = " << cb_compiled_time << " ms" << endl;
}

static jvmtiError request_previous_events(jvmtiEnv *jvmti, JNIEnv *jni_env) {
    jthread logger_thread = new_thread(jni_env, "Profiler Agent Previous Events Writer Thread");
    return jvmti->RunAgentThread(logger_thread, events_logger_function, nullptr, JVMTI_THREAD_NORM_PRIORITY);
}

void JNICALL
cbVMInit(jvmtiEnv *jvmti,
         JNIEnv *jni_env,
         jthread thread) {
    log_file << "VMInit event" << endl;
    if (report_failed(enable_notifications(jvmti), "enable_notifications at VMInit")) {
        shutdown(jvmti, jni_env, true, false);
        return;
    }
    if (report_failed(request_previous_events(jvmti, jni_env), "request_previous_events at VMInit")) {
        shutdown(jvmti, jni_env, true, false);
        return;
    }
}

void JNICALL
cbVMDeath(jvmtiEnv *jvmti,
          JNIEnv *jni_env) {
    log_file << "VMDeath event" << endl;
    shutdown(jvmti, jni_env, true, true);
}

jvmtiError enable_capabilities(jvmtiEnv *jvmti) {
    jvmtiCapabilities capabilities{};
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.can_generate_compiled_method_load_events = 1;
    return jvmti->AddCapabilities(&capabilities);
}

jvmtiError set_callbacks(jvmtiEnv *jvmti) {
    jvmtiEventCallbacks callbacks{};
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.CompiledMethodLoad = &cbCompiledMethodLoad;
    callbacks.CompiledMethodUnload = &cbCompiledMethodUnload;
    callbacks.DynamicCodeGenerated = &cbDynamicCodeGenerated;
    callbacks.ThreadStart = &cbThreadStart;
    callbacks.ThreadEnd = &cbThreadEnd;
    callbacks.VMInit = &cbVMInit;
    callbacks.VMDeath = &cbVMDeath;
    return jvmti->SetEventCallbacks(&callbacks, (jint) sizeof(callbacks));
}

const string ef_prefix("events_file=");
const string lf_prefix("log_file=");

static int agent_main(JavaVM *vm, const char *options, bool already_in_live_phase) {
    bool shutdown_command = false;
    bool force = false;
    string events_file_name;
    string log_file_name;
    auto args = options == nullptr ? vector<string>() : split_string(options, ',');
    for (auto &arg: args) {
        if (starts_with(arg, ef_prefix)) {
            events_file_name = arg.substr(ef_prefix.size());;
        } else if (starts_with(arg, lf_prefix)) {
            log_file_name = arg.substr(lf_prefix.size());
        } else if (arg == "shutdown") {
            shutdown_command = true;
        } else if (arg == "forceshutdown") {
            shutdown_command = true;
            force = true;
        }
    }
    if (shutdown_command) {
        log_file << "shutdown requested by user" << endl;
        shutdown(_jvmti, get_JNI(_vm), force, false);
        return 0;
    }
    {
        lock_guard<mutex> lock(jvmti_mutex);
        if (_vm != nullptr && _jvmti != nullptr) { //can not attach more than one agent at one time
            return 1001;
        }
        events_file.open(events_file_name);
        log_file.open(log_file_name);
        if (vm->GetEnv((void **) &_jvmti, JVMTI_VERSION_1) != JVMTI_ERROR_NONE) {
            log_file << "can't get jvmti env" << endl;
            return 2;
        }
        _vm = vm;
    }
    attach_count += 1;
    log_file << "attach count = " << attach_count << endl;
    if (!events_file.is_open()) {
        events_file.open(my_formatter("/tmp/perf-%d.map", getpid()));
    }
    if (!events_file.is_open()) {
        log_file << "can't open events_file. Will terminate." << endl;
        return 1;
    }
    if (report_failed(enable_capabilities(_jvmti), "enable_capabilities error")) return 2;
    if (report_failed(set_callbacks(_jvmti), "set_callbacks error")) return 2;
    if (already_in_live_phase) {
        if (report_failed(enable_notifications(_jvmti), "enable_notifications at agent_main")) return 2;
        if (report_failed(request_previous_events(_jvmti, get_JNI(vm)), "request_previous_events at agent_main"))
            return 2;
    } else {
        if (report_failed(_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, nullptr),
                          "enable JVMTI_EVENT_VM_INIT"))
            return 2;
    }
    return 0;
}


JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
    return agent_main(vm, options, false);
}

JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    return agent_main(vm, options, true);
}