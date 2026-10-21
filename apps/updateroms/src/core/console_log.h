//
// UpdateRoms's console log: the engine's console appender, but a quiet one shows warnings and errors only.
// The log file (System/Logs/updateroms.log) is a separate appender and keeps the INFO lines either way.
//
#pragma once

#include <ableem/engine/log.h>

//******************
// ConsoleLog
//******************
class ConsoleLog : public plog::ConsoleAppender<ableem::Log::Formatter> {
public:
    explicit ConsoleLog(bool quiet = false) : quiet_(quiet) {}

    void setQuiet(bool quiet) { quiet_ = quiet; }

    void write(const plog::Record &record) override {
        // plog's severities count up from fatal: anything above warning is INFO or DEBUG
        if (quiet_ && record.getSeverity() > plog::warning)
            return;
        plog::ConsoleAppender<ableem::Log::Formatter>::write(record);
    }

private:
    bool quiet_;
};
