//
// FastbootOutput - see the header.
//
#include "fastboot.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

using namespace std;

namespace {

string trimmed(const string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a])))
        a++;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1])))
        b--;
    return s.substr(a, b - a);
}

} // namespace

//*******************************
// FastbootOutput::feed
//*******************************
void FastbootOutput::feed(const string &text) {
    all += text;
    pending_ += text;
    size_t nl;
    while ((nl = pending_.find_first_of("\r\n")) != string::npos) {
        string one = pending_.substr(0, nl);
        pending_.erase(0, nl + 1);
        line(one);
    }
    // the client prints this and then waits, without a newline
    if (pending_.find("waiting for") != string::npos)
        deviceLost = true;
}

//*******************************
// FastbootOutput::finish
//*******************************
void FastbootOutput::finish() {
    if (!pending_.empty()) {
        string rest = pending_;
        pending_.clear();
        line(rest);
    }
}

//*******************************
// FastbootOutput::line
//*******************************
void FastbootOutput::line(const string &raw) {
    const string text = trimmed(raw);
    if (text.empty())
        return;
    if (text.find("waiting for") != string::npos)
        deviceLost = true;
    if (text.find("FAILED") != string::npos)
        failure = text;
    // "Sending [sparse ]'NAME' [i/n ](N KB) ... OKAY [ t ]" - counted once the console said OKAY
    if (text.compare(0, 8, "Sending ") == 0 && text.find("OKAY") != string::npos) {
        size_t open = text.find(" (");
        size_t kb = text.find(" KB)", open == string::npos ? 0 : open);
        if (open != string::npos && kb != string::npos)
            sentBytes += strtoull(text.substr(open + 2, kb - open - 2).c_str(), nullptr, 10) * 1024;
    }
    if (onLine)
        onLine(text);
}

//*******************************
// FastbootOutput::devices
//*******************************
vector<string> FastbootOutput::devices(const string &text) {
    vector<string> serials;
    istringstream in(text);
    string row;
    while (getline(in, row)) {
        istringstream words(row);
        string serial, mode;
        words >> serial >> mode;
        if (!serial.empty() && mode == "fastboot")
            serials.push_back(serial);
    }
    return serials;
}

//*******************************
// FastbootOutput::getvar
//*******************************
bool FastbootOutput::getvar(const string &text, const string &name, string &value) {
    istringstream in(text);
    string row;
    const string prefix = name + ":";
    while (getline(in, row)) {
        string t = trimmed(row);
        if (t.compare(0, prefix.size(), prefix) == 0) {
            value = trimmed(t.substr(prefix.size()));
            return !value.empty();
        }
    }
    return false;
}

//*******************************
// FastbootOutput::parseSize
//*******************************
bool FastbootOutput::parseSize(const string &text, uint64_t &size) {
    const string t = trimmed(text);
    if (t.empty())
        return false;
    char *end = nullptr;
    unsigned long long v = strtoull(t.c_str(), &end, 0); // 0x... hex, else decimal
    if (!end || *end != '\0')
        return false;
    size = v;
    return true;
}
