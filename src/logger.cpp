#include "logger.h"

ofstream log_file;

bool report_failed(const jvmtiError err, const string &error_msg) {
    if (err == JVMTI_ERROR_NONE) return false;
    log_file << error_msg << " (jvmtiError: " << err << ")" << endl;
    return true;
}
