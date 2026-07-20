#ifndef SYSTEM_TELEMETRY_H
#define SYSTEM_TELEMETRY_H

#include <atomic>
#include <cstdint>

class SystemTelemetry {
public:
    static void SetMicLevel(int peak, int avg) {
        MicPeakStore().store(peak, std::memory_order_relaxed);
        MicAvgStore().store(avg, std::memory_order_relaxed);
    }

    static int GetMicPeak() {
        return MicPeakStore().load(std::memory_order_relaxed);
    }

    static int GetMicAvg() {
        return MicAvgStore().load(std::memory_order_relaxed);
    }

private:
    static std::atomic<int>& MicPeakStore() {
        static std::atomic<int> value{0};
        return value;
    }

    static std::atomic<int>& MicAvgStore() {
        static std::atomic<int> value{0};
        return value;
    }
};

#endif // SYSTEM_TELEMETRY_H
