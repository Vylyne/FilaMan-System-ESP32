#include "weight_delivery.h"

namespace weightdelivery {

DeliveryQueue::DeliveryQueue() { clear(); }

void DeliveryQueue::clear() {
    for (Slot& s : slots_) s = Slot();
    for (HistoryEntry& h : history_) h = HistoryEntry();
    historyNext_ = 0;
}

bool DeliveryQueue::sameTarget(const Measurement& m, int spoolId, const std::string& tagUuid) {
    if (spoolId > 0 || m.spoolId > 0) return spoolId == m.spoolId;
    return m.tagUuid == tagUuid;
}

// Wrap-safe "now has reached atMs" for a millis()-style counter.
bool DeliveryQueue::isDue(uint32_t nowMs, uint32_t atMs) {
    return static_cast<int32_t>(nowMs - atMs) >= 0;
}

uint32_t DeliveryQueue::backoffMs(uint32_t attempts) {
    if (attempts == 0) return 0;
    uint32_t delay = kBaseBackoffMs;
    for (uint32_t i = 1; i < attempts && delay < kMaxBackoffMs; i++) delay *= 2;
    return delay > kMaxBackoffMs ? kMaxBackoffMs : delay;
}

EnqueueResult DeliveryQueue::enqueue(int spoolId, const std::string& tagUuid, float weight,
                                     uint32_t nowMs, uint32_t* seqOut) {
    if (seqOut) *seqOut = 0;
    if (!(weight > 0.0f)) return EnqueueResult::InvalidWeight;
    if (spoolId <= 0 && tagUuid.empty()) return EnqueueResult::InvalidTarget;

    Measurement m;
    m.seq = nextSeq_++;
    if (nextSeq_ == 0) nextSeq_ = 1;  // 0 means "no ticket"
    m.spoolId = spoolId > 0 ? spoolId : 0;
    m.tagUuid = tagUuid;
    m.weight = weight;

    // Replace a pending (not in-flight) entry for the same target.
    for (Slot& s : slots_) {
        if (s.used && !s.inFlight && sameTarget(s.m, spoolId, tagUuid)) {
            remember(s.m.seq, DeliveryStatus::Superseded);
            s.m = m;
            s.attempts = 0;
            s.nextAttemptMs = nowMs;
            if (seqOut) *seqOut = m.seq;
            return EnqueueResult::Replaced;
        }
    }

    for (Slot& s : slots_) {
        if (!s.used) {
            s.used = true;
            s.inFlight = false;
            s.attempts = 0;
            s.nextAttemptMs = nowMs;
            s.m = m;
            if (seqOut) *seqOut = m.seq;
            return EnqueueResult::Queued;
        }
    }
    return EnqueueResult::Full;
}

bool DeliveryQueue::beginNext(uint32_t nowMs, Measurement& out) {
    Slot* best = nullptr;
    for (Slot& s : slots_) {
        if (!s.used) continue;
        if (s.inFlight) return false;  // one request at a time
        if (!isDue(nowMs, s.nextAttemptMs)) continue;
        if (!best || s.m.seq < best->m.seq) best = &s;
    }
    if (!best) return false;
    best->inFlight = true;
    out = best->m;
    return true;
}

bool DeliveryQueue::hasNewerFor(const Slot& slot) const {
    for (const Slot& s : slots_) {
        if (&s == &slot || !s.used) continue;
        if (sameTarget(s.m, slot.m.spoolId, slot.m.tagUuid) && s.m.seq > slot.m.seq) return true;
    }
    return false;
}

Completion DeliveryQueue::complete(uint32_t seq, SendOutcome outcome, uint32_t nowMs) {
    Completion result;
    Slot* slot = nullptr;
    for (Slot& s : slots_) {
        if (s.used && s.inFlight && s.m.seq == seq) { slot = &s; break; }
    }
    if (!slot) return result;  // unknown or not in flight

    slot->inFlight = false;
    const bool superseded = hasNewerFor(*slot);

    switch (outcome) {
        case SendOutcome::Success:
            result.status = DeliveryStatus::Confirmed;
            break;
        case SendOutcome::Permanent:
            slot->attempts++;
            result.status = DeliveryStatus::Failed;
            break;
        case SendOutcome::Transient:
            slot->attempts++;
            result.status = superseded ? DeliveryStatus::Superseded : DeliveryStatus::Pending;
            break;
        case SendOutcome::NoNetwork:
            result.status = superseded ? DeliveryStatus::Superseded : DeliveryStatus::Pending;
            break;
    }
    result.attempts = slot->attempts;

    if (result.status == DeliveryStatus::Pending) {
        result.retryInMs = (outcome == SendOutcome::NoNetwork) ? kNoNetworkWaitMs
                                                               : backoffMs(slot->attempts);
        slot->nextAttemptMs = nowMs + result.retryInMs;
        return result;
    }

    remember(slot->m.seq, result.status);
    *slot = Slot();
    return result;
}

void DeliveryQueue::remember(uint32_t seq, DeliveryStatus status) {
    history_[historyNext_].seq = seq;
    history_[historyNext_].status = status;
    historyNext_ = (historyNext_ + 1) % kHistory;
}

DeliveryStatus DeliveryQueue::status(uint32_t seq) const {
    if (seq == 0) return DeliveryStatus::Unknown;
    for (const Slot& s : slots_) {
        if (s.used && s.m.seq == seq) {
            return s.inFlight ? DeliveryStatus::InFlight : DeliveryStatus::Pending;
        }
    }
    for (const HistoryEntry& h : history_) {
        if (h.seq == seq) return h.status;
    }
    return DeliveryStatus::Unknown;
}

size_t DeliveryQueue::pendingCount() const {
    size_t n = 0;
    for (const Slot& s : slots_) if (s.used) n++;
    return n;
}

}  // namespace weightdelivery
