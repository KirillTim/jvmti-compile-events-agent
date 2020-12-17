#ifndef PERF_MAP_AGENT_LOGGER_H
#define PERF_MAP_AGENT_LOGGER_H

#include <fstream>

#include "jvmti.h"

using namespace std;

extern ofstream log_file;

bool report_failed(jvmtiError err, const string &error_msg);

#endif //PERF_MAP_AGENT_LOGGER_H
