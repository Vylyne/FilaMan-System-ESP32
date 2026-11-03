#ifndef WEIGHT_DELIVERY_H
#define WEIGHT_DELIVERY_H

// Bookkeeping for weight measurements on their way to the FilaMan backend.
//
// Pure C++ (no Arduino, no FreeRTOS) so it can be unit-tested on the host.
// The caller owns locking and the clock; every method takes "now" in ms.
//
// Rules:
// - One pending entry per target (spool id, or tag UUID when there is no
//   spool id). A newer measurement replaces an older pending one.
// - An entry is removed only when the backend acknowledged it, failed
//   permanently, or was superseded by a newer measurement for the same target.
// - Transient failures retry with exponential backoff capped at 60 s, with no
//   limit on the number of attempts.

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace weightdelivery {

enum class EnqueueResult : uint8_t {
    Queued,         // new entry created
    Replaced,       // replaced an older pending measurement for the same target
    Full,           // no free slot, try again later
    InvalidWeight,  // weight <= 0, nothing to send
    InvalidTarget   // neither spool id nor tag UUID
};

enum class SendOutcome : uint8_t {
    Success,    // backend acknowledged (2xx)
    Transient,  // timeout, connection error, 408, 429, 5xx: retry with backoff
    NoNetwork,  // network down: wait, does not count as an attempt
    Permanent   // 4xx and similar: retrying will not help
};

enum class DeliveryStatus : uint8_t {
    Unknown,     // never seen, or dropped out of the history
    Pending,     // waiting for its (next) attempt
    InFlight,    // HTTP request running
    Confirmed,   // backend acknowledged
    Failed,      // permanent failure
    Superseded   // replaced by a newer measurement for the same target
};

struct Measurement {
    uint32_t seq = 0;
    int spoolId = 0;
    std::string tagUuid;
    float weight = 0.0f;
};

struct Completion {
    DeliveryStatus status = DeliveryStatus::Unknown;  // Pending when a retry is scheduled
    uint32_t attempts = 0;      // failed attempts counted so far
    uint32_t retryInMs = 0;     // delay until the next attempt, when Pending
};

class DeliveryQueue {
public:
    static constexpr size_t kCapacity = 6;
    static constexpr size_t kHistory = 8;
    static constexpr uint32_t kBaseBackoffMs = 2000;
    static constexpr uint32_t kMaxBackoffMs = 60000;
    static constexpr uint32_t kNoNetworkWaitMs = 5000;

    DeliveryQueue();

    EnqueueResult enqueue(int spoolId, const std::string& tagUuid, float weight,
                          uint32_t nowMs, uint32_t* seqOut = nullptr);

    // Marks the oldest due entry as in flight and copies it to `out`.
    // Returns false when nothing is due or a request is already in flight.
    bool beginNext(uint32_t nowMs, Measurement& out);

    // Reports the result of the in-flight request with sequence `seq`.
    Completion complete(uint32_t seq, SendOutcome outcome, uint32_t nowMs);

    DeliveryStatus status(uint32_t seq) const;
    size_t pendingCount() const;
    void clear();

    static uint32_t backoffMs(uint32_t attempts);

private:
    struct Slot {
        bool used = false;
        bool inFlight = false;
        uint32_t attempts = 0;
        uint32_t nextAttemptMs = 0;
        Measurement m;
    };

    struct HistoryEntry {
        uint32_t seq = 0;
        DeliveryStatus status = DeliveryStatus::Unknown;
    };

    static bool sameTarget(const Measurement& m, int spoolId, const std::string& tagUuid);
    static bool isDue(uint32_t nowMs, uint32_t atMs);
    bool hasNewerFor(const Slot& slot) const;
    void remember(uint32_t seq, DeliveryStatus status);

    Slot slots_[kCapacity];
    HistoryEntry history_[kHistory];
    size_t historyNext_ = 0;
    uint32_t nextSeq_ = 1;
};

}  // namespace weightdelivery

#endif
