#ifndef PERF_MAP_AGENT_UTILS_H
#define PERF_MAP_AGENT_UTILS_H

#include <string>
#include <vector>

#include <jvmti.h>

using namespace std;

bool starts_with(const string &subject, const string &prefix);

string my_formatter(const char *fmt, ...);

string class_name_from_sig(const string &sig);

vector<string> split_string(const string &subject, char delimiter);

string sig_string(jvmtiEnv *jvmti, jmethodID method);

#endif //PERF_MAP_AGENT_UTILS_H
