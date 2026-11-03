// Host tests for the weight delivery queue: pio test -e native
#include <unity.h>
#include "weight_delivery.h"

using namespace weightdelivery;

static DeliveryQueue q;

void setUp() { q.clear(); }
void tearDown() {}

static void test_rejects_invalid_input() {
    TEST_ASSERT_EQUAL(EnqueueResult::InvalidWeight, q.enqueue(1, "AA", 0.0f, 0));
    TEST_ASSERT_EQUAL(EnqueueResult::InvalidWeight, q.enqueue(1, "AA", -3.0f, 0));
    TEST_ASSERT_EQUAL(EnqueueResult::InvalidTarget, q.enqueue(0, "", 100.0f, 0));
    TEST_ASSERT_EQUAL(0, q.pendingCount());
}

static void test_success_removes_entry() {
    uint32_t seq = 0;
    TEST_ASSERT_EQUAL(EnqueueResult::Queued, q.enqueue(5, "AA", 812.0f, 0, &seq));
    TEST_ASSERT_EQUAL(DeliveryStatus::Pending, q.status(seq));

    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(0, m));
    TEST_ASSERT_EQUAL(seq, m.seq);
    TEST_ASSERT_EQUAL(5, m.spoolId);
    TEST_ASSERT_EQUAL_FLOAT(812.0f, m.weight);
    TEST_ASSERT_EQUAL(DeliveryStatus::InFlight, q.status(seq));

    Completion c = q.complete(seq, SendOutcome::Success, 10);
    TEST_ASSERT_EQUAL(DeliveryStatus::Confirmed, c.status);
    TEST_ASSERT_EQUAL(DeliveryStatus::Confirmed, q.status(seq));
    TEST_ASSERT_EQUAL(0, q.pendingCount());
}

// Backend down, then back: the same measurement is retried until it lands.
static void test_transient_failure_retries_until_success() {
    uint32_t seq = 0;
    q.enqueue(5, "AA", 812.0f, 0, &seq);
    Measurement m;
    uint32_t now = 0;

    for (uint32_t attempt = 1; attempt <= 10; attempt++) {
        TEST_ASSERT_TRUE(q.beginNext(now, m));
        TEST_ASSERT_EQUAL(seq, m.seq);
        Completion c = q.complete(seq, SendOutcome::Transient, now);
        TEST_ASSERT_EQUAL(DeliveryStatus::Pending, c.status);
        TEST_ASSERT_EQUAL(attempt, c.attempts);
        TEST_ASSERT_EQUAL(DeliveryQueue::backoffMs(attempt), c.retryInMs);
        // Not due before the backoff has elapsed.
        TEST_ASSERT_FALSE(q.beginNext(now + c.retryInMs - 1, m));
        now += c.retryInMs;
    }

    TEST_ASSERT_TRUE(q.beginNext(now, m));
    TEST_ASSERT_EQUAL(DeliveryStatus::Confirmed, q.complete(seq, SendOutcome::Success, now).status);
    TEST_ASSERT_EQUAL(0, q.pendingCount());
}

static void test_backoff_is_capped() {
    TEST_ASSERT_EQUAL(2000, DeliveryQueue::backoffMs(1));
    TEST_ASSERT_EQUAL(4000, DeliveryQueue::backoffMs(2));
    TEST_ASSERT_EQUAL(32000, DeliveryQueue::backoffMs(5));
    TEST_ASSERT_EQUAL(60000, DeliveryQueue::backoffMs(6));
    TEST_ASSERT_EQUAL(60000, DeliveryQueue::backoffMs(1000));
}

static void test_no_network_does_not_count_attempt() {
    uint32_t seq = 0;
    q.enqueue(5, "AA", 812.0f, 0, &seq);
    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(0, m));
    Completion c = q.complete(seq, SendOutcome::NoNetwork, 0);
    TEST_ASSERT_EQUAL(DeliveryStatus::Pending, c.status);
    TEST_ASSERT_EQUAL(0, c.attempts);
    TEST_ASSERT_EQUAL(DeliveryQueue::kNoNetworkWaitMs, c.retryInMs);
}

static void test_permanent_failure_drops_entry() {
    uint32_t seq = 0;
    q.enqueue(5, "AA", 812.0f, 0, &seq);
    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(0, m));
    TEST_ASSERT_EQUAL(DeliveryStatus::Failed, q.complete(seq, SendOutcome::Permanent, 0).status);
    TEST_ASSERT_EQUAL(DeliveryStatus::Failed, q.status(seq));
    TEST_ASSERT_EQUAL(0, q.pendingCount());
}

static void test_newer_measurement_replaces_pending_one() {
    uint32_t oldSeq = 0, newSeq = 0;
    q.enqueue(5, "AA", 812.0f, 0, &oldSeq);
    TEST_ASSERT_EQUAL(EnqueueResult::Replaced, q.enqueue(5, "AA", 790.0f, 1, &newSeq));
    TEST_ASSERT_EQUAL(1, q.pendingCount());
    TEST_ASSERT_EQUAL(DeliveryStatus::Superseded, q.status(oldSeq));

    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(1, m));
    TEST_ASSERT_EQUAL(newSeq, m.seq);
    TEST_ASSERT_EQUAL_FLOAT(790.0f, m.weight);
}

// A newer measurement arrives while the older one is on the wire. The older
// result, whatever it is, must not remove the newer one, and the older one is
// never retried after it.
static void test_in_flight_entry_is_superseded_not_retried() {
    uint32_t oldSeq = 0, newSeq = 0;
    q.enqueue(5, "AA", 812.0f, 0, &oldSeq);
    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(0, m));
    TEST_ASSERT_EQUAL(EnqueueResult::Queued, q.enqueue(5, "AA", 790.0f, 1, &newSeq));

    // Nothing else starts while a request is in flight.
    TEST_ASSERT_FALSE(q.beginNext(1, m));

    Completion c = q.complete(oldSeq, SendOutcome::Transient, 2);
    TEST_ASSERT_EQUAL(DeliveryStatus::Superseded, c.status);
    TEST_ASSERT_EQUAL(1, q.pendingCount());

    TEST_ASSERT_TRUE(q.beginNext(2, m));
    TEST_ASSERT_EQUAL(newSeq, m.seq);
    TEST_ASSERT_EQUAL_FLOAT(790.0f, m.weight);
}

static void test_success_of_older_keeps_newer_pending() {
    uint32_t oldSeq = 0, newSeq = 0;
    q.enqueue(5, "AA", 812.0f, 0, &oldSeq);
    Measurement m;
    q.beginNext(0, m);
    q.enqueue(5, "AA", 790.0f, 1, &newSeq);
    TEST_ASSERT_EQUAL(DeliveryStatus::Confirmed, q.complete(oldSeq, SendOutcome::Success, 2).status);
    TEST_ASSERT_EQUAL(DeliveryStatus::Pending, q.status(newSeq));
    TEST_ASSERT_TRUE(q.beginNext(2, m));
    TEST_ASSERT_EQUAL(newSeq, m.seq);
}

// Switching spools while an update is pending keeps both, each with its own
// identity and weight.
static void test_different_spools_are_independent() {
    uint32_t a = 0, b = 0;
    q.enqueue(5, "AA", 812.0f, 0, &a);
    q.enqueue(7, "BB", 455.0f, 1, &b);
    TEST_ASSERT_EQUAL(2, q.pendingCount());

    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(1, m));
    TEST_ASSERT_EQUAL(a, m.seq);
    TEST_ASSERT_EQUAL(5, m.spoolId);
    q.complete(a, SendOutcome::Success, 1);

    TEST_ASSERT_TRUE(q.beginNext(1, m));
    TEST_ASSERT_EQUAL(b, m.seq);
    TEST_ASSERT_EQUAL(7, m.spoolId);
    TEST_ASSERT_EQUAL_STRING("BB", m.tagUuid.c_str());
    TEST_ASSERT_EQUAL_FLOAT(455.0f, m.weight);
}

// UUID-only tags (no spool id) are keyed by UUID.
static void test_uuid_only_targets() {
    uint32_t a = 0, b = 0;
    q.enqueue(0, "AA", 100.0f, 0, &a);
    TEST_ASSERT_EQUAL(EnqueueResult::Queued, q.enqueue(0, "BB", 200.0f, 0, &b));
    TEST_ASSERT_EQUAL(EnqueueResult::Replaced, q.enqueue(0, "AA", 110.0f, 0));
    TEST_ASSERT_EQUAL(2, q.pendingCount());
}

static void test_queue_saturation() {
    for (int i = 1; i <= (int)DeliveryQueue::kCapacity; i++) {
        TEST_ASSERT_EQUAL(EnqueueResult::Queued, q.enqueue(i, "", 10.0f, 0));
    }
    TEST_ASSERT_EQUAL(EnqueueResult::Full, q.enqueue(99, "", 10.0f, 0));
    // An update for a target already queued still fits.
    TEST_ASSERT_EQUAL(EnqueueResult::Replaced, q.enqueue(1, "", 11.0f, 0));

    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(0, m));
    q.complete(m.seq, SendOutcome::Success, 0);
    TEST_ASSERT_EQUAL(EnqueueResult::Queued, q.enqueue(99, "", 10.0f, 0));
}

static void test_retry_timing_survives_millis_wrap() {
    const uint32_t nearWrap = 0xFFFFFFFFu - 500;
    uint32_t seq = 0;
    q.enqueue(5, "AA", 812.0f, nearWrap, &seq);
    Measurement m;
    TEST_ASSERT_TRUE(q.beginNext(nearWrap, m));
    Completion c = q.complete(seq, SendOutcome::Transient, nearWrap);
    TEST_ASSERT_FALSE(q.beginNext(nearWrap + 1000, m));  // wrapped, still before due
    TEST_ASSERT_TRUE(q.beginNext(nearWrap + c.retryInMs, m));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_rejects_invalid_input);
    RUN_TEST(test_success_removes_entry);
    RUN_TEST(test_transient_failure_retries_until_success);
    RUN_TEST(test_backoff_is_capped);
    RUN_TEST(test_no_network_does_not_count_attempt);
    RUN_TEST(test_permanent_failure_drops_entry);
    RUN_TEST(test_newer_measurement_replaces_pending_one);
    RUN_TEST(test_in_flight_entry_is_superseded_not_retried);
    RUN_TEST(test_success_of_older_keeps_newer_pending);
    RUN_TEST(test_different_spools_are_independent);
    RUN_TEST(test_uuid_only_targets);
    RUN_TEST(test_queue_saturation);
    RUN_TEST(test_retry_timing_survives_millis_wrap);
    return UNITY_END();
}
