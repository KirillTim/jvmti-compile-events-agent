#include "utils.h"

#include <sstream>

static const int STRING_BUFFER_SIZE = 2000;

bool starts_with(const string &subject, const string &prefix) {
    return subject.substr(0, prefix.size()) == prefix;
}

string my_formatter(const char *fmt, ...) {
    char buf[STRING_BUFFER_SIZE];
    va_list argptr;
    va_start(argptr, fmt);
    vsprintf(buf, fmt, argptr);
    va_end(argptr);
    return string(buf);
}

string class_name_from_sig(const string &sig) {
    string result = string(sig);
    if (result[0] == 'L') result = result.substr(1);
    result = result.substr(0, result.find(';'));
    for (char &i : result) {
        if (i == '/') i = '.';
    }
    return result;
}

vector<string> split_string(const string &subject, const char delimiter) {
    stringstream ss(subject);
    vector<string> result;
    string item;
    while (getline(ss, item, delimiter)) {
        result.push_back(move(item));
    }
    return result;
}

string sig_string(jvmtiEnv *jvmti, jmethodID method) {
    //auto start = system_clock::now();
    char *generic_method_sig = nullptr;
    char *generic_class_sig = nullptr;
    char *method_name = nullptr;
    char *msig = nullptr;
    char *csig = nullptr;
    jclass jcls;
    string result;
    if (!jvmti->GetMethodName(method, &method_name, &msig, &generic_method_sig)) {
        if (!jvmti->GetMethodDeclaringClass(method, &jcls)
            && !jvmti->GetClassSignature(jcls, &csig, &generic_class_sig)) {
            result = string(class_name_from_sig(csig)) + "." + string(method_name);
        }
    }
    if (generic_method_sig != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char *>(generic_method_sig));
    if (generic_class_sig != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char *>(generic_class_sig));
    if (method_name != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char *>(method_name));
    if (csig != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char *>(csig));
    if (msig != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char *>(msig));
    //sig_string_time += duration_cast<milliseconds>(system_clock::now() - start).count();
    return result;
}