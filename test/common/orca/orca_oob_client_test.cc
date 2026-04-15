#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/backoff_strategy.h"
#include "envoy/grpc/status.h"
#include "envoy/http/async_client.h"

#include "source/common/common/backoff_strategy.h"
#include "source/common/orca/orca_oob_client.h"
#include "source/common/stats/isolated_store_impl.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/test_common/logging.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"
#include "xds/service/orca/v3/orca.pb.h"

using testing::_;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Orca {
namespace {

// Mock subscriber interface.
class MockOrcaOobReportCallbacks : public OrcaOobReportCallbacks {
public:
  MOCK_METHOD(void, onOrcaReport, (const xds::data::orca::v3::OrcaLoadReport& report), (override));
  MOCK_METHOD(void, onStreamClosed, (Grpc::Status::GrpcStatus status, absl::string_view message),
              (override));
};

class OrcaOobClientTest : public testing::Test {
protected:
  OrcaOobClientTest() : stats_(generateOrcaOobClientStats(*scope_.rootScope(), "test_orca_oob.")) {}

  // Shared mocks.
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<Grpc::MockAsyncStream> mock_stream_;
  std::shared_ptr<NiceMock<Grpc::MockAsyncClient>> mock_async_client_{
      std::make_shared<NiceMock<Grpc::MockAsyncClient>>()};
  Stats::IsolatedStoreImpl scope_;
  OrcaOobClientStats stats_;
  NiceMock<MockOrcaOobReportCallbacks> callbacks_;

  // Captures the most recent retry delay passed to the retry timer's
  // enableTimer() - used by TransientFailuresRampBackoff to assert monotonic
  // ramping across successive failures.
  std::chrono::milliseconds last_retry_delay_{0};
};

// Pin test for the JitteredExponentialBackOffStrategy::reset() semantics the
// client's state machine depends on. See the design spec's
// "Backoff reset semantics" section.
TEST(OrcaOobBackoffAssumption, ResetRestoresBaseDelay) {
  NiceMock<Random::MockRandomGenerator> random;
  // JitteredExponentialBackOffStrategy::nextBackOffMs() returns
  // `random() % next_interval_`, so the pinned random value must be large
  // enough that it differentiates base_interval (500) from 2*base (1000).
  // 999 gives `999 % 500 = 499` and `999 % 1000 = 999`, producing distinct
  // results in each bucket.
  ON_CALL(random, random()).WillByDefault(Return(999));

  JitteredExponentialBackOffStrategy strategy(/*base_interval_ms=*/500,
                                              /*max_interval_ms=*/30000, random);

  // Advance the strategy via a couple of nextBackOffMs() calls.
  const uint64_t d1 = strategy.nextBackOffMs();
  const uint64_t d2 = strategy.nextBackOffMs();
  EXPECT_GT(d2, d1) << "backoff should ramp on successive calls";

  // Reset, then the next delay should match the first-call behavior.
  strategy.reset();
  const uint64_t d1_after_reset = strategy.nextBackOffMs();
  EXPECT_EQ(d1_after_reset, d1)
      << "reset() must restore the state such that the next call returns the base delay";

  // And the call after that should ramp again, matching d2.
  const uint64_t d2_after_reset = strategy.nextBackOffMs();
  EXPECT_EQ(d2_after_reset, d2) << "subsequent calls after reset ramp from base";
}

TEST_F(OrcaOobClientTest, HappyPathLifecycleAndWireFormat) {
  // Capture the StreamOptions and the serialized initial request.
  Http::AsyncClient::StreamOptions captured_opts;
  Buffer::InstancePtr captured_initial_msg;

  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _))
      .WillOnce(Invoke([&](absl::string_view service_full_name, absl::string_view method_name,
                           Grpc::RawAsyncStreamCallbacks&,
                           const Http::AsyncClient::StreamOptions& opts) -> Grpc::RawAsyncStream* {
        EXPECT_EQ(service_full_name, "xds.service.orca.v3.OpenRcaService");
        EXPECT_EQ(method_name, "StreamCoreMetrics");
        captured_opts = opts;
        return &mock_stream_;
      }));

  // Capture the single sendMessageRaw call (initial request, end_stream=true).
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, /*end_stream=*/true))
      .WillOnce(
          Invoke([&](Buffer::InstancePtr& msg, bool) { captured_initial_msg = std::move(msg); }));

  // Construct the client with concrete values we can verify on the wire.
  auto client = std::make_unique<OrcaOobClient>(
      mock_async_client_, dispatcher_, std::chrono::milliseconds(7500),
      std::vector<std::string>{"foo", "bar"}, "1.2.3.4:8080", stats_, callbacks_,
      OrcaOobClient::defaultBackoffStrategy(random_));

  // Library pins the target by translating the address into a strict upstream
  // override host on the async stream options.
  auto override = captured_opts.upstream_override_host_;
  EXPECT_EQ(override.host, "1.2.3.4:8080");
  EXPECT_TRUE(override.strict);

  // Parse the captured serialized initial request and verify its fields.
  // Note: `Grpc::Common::serializeMessage()` (called by the typed async client
  // via `sendMessageUntyped`) writes only the raw protobuf payload into the
  // buffer. The gRPC 5-byte length-prefix framing is added later at the HTTP/2
  // codec layer, not in the buffer handed to `sendMessageRaw`.
  ASSERT_NE(captured_initial_msg, nullptr);
  const std::string payload = captured_initial_msg->toString();

  xds::service::orca::v3::OrcaLoadReportRequest parsed_req;
  ASSERT_TRUE(parsed_req.ParseFromString(payload));
  EXPECT_EQ(parsed_req.report_interval().seconds(), 7);
  EXPECT_EQ(parsed_req.report_interval().nanos(), 500'000'000);
  ASSERT_EQ(parsed_req.request_cost_names_size(), 2);
  EXPECT_EQ(parsed_req.request_cost_names(0), "foo");
  EXPECT_EQ(parsed_req.request_cost_names(1), "bar");

  // Build a non-trivial response report and deliver it to the client.
  auto report = std::make_unique<xds::data::orca::v3::OrcaLoadReport>();
  report->set_cpu_utilization(0.42);
  report->set_mem_utilization(0.18);
  report->set_application_utilization(0.7);
  report->set_eps(13.5);
  report->set_rps_fractional(1250.0);
  (*report->mutable_named_metrics())["queue_depth"] = 17.0;
  (*report->mutable_utilization())["shard_load"] = 0.93;

  // Verify the subscriber receives the report with every field intact.
  EXPECT_CALL(callbacks_, onOrcaReport(_)).WillOnce(Invoke([&](const auto& delivered) {
    EXPECT_DOUBLE_EQ(delivered.cpu_utilization(), 0.42);
    EXPECT_DOUBLE_EQ(delivered.mem_utilization(), 0.18);
    EXPECT_DOUBLE_EQ(delivered.application_utilization(), 0.7);
    EXPECT_DOUBLE_EQ(delivered.eps(), 13.5);
    EXPECT_DOUBLE_EQ(delivered.rps_fractional(), 1250.0);
    EXPECT_DOUBLE_EQ(delivered.named_metrics().at("queue_depth"), 17.0);
    EXPECT_DOUBLE_EQ(delivered.utilization().at("shard_load"), 0.93);
  }));

  // Deliver the report via the AsyncStreamCallbacks interface.
  auto* stream_callbacks =
      static_cast<Grpc::AsyncStreamCallbacks<xds::data::orca::v3::OrcaLoadReport>*>(client.get());
  stream_callbacks->onReceiveMessage(std::move(report));

  // Verify stats after first report.
  EXPECT_EQ(stats_.reports_received_.value(), 1);
  EXPECT_EQ(stats_.stream_success_.value(), 1);
  EXPECT_EQ(stats_.stream_active_.value(), 1);
  EXPECT_EQ(stats_.stream_terminated_.value(), 0);

  // Deliver a second report — the first_message_received_ latch is already set,
  // so stream_success must NOT re-increment.
  auto report2 = std::make_unique<xds::data::orca::v3::OrcaLoadReport>();
  report2->set_cpu_utilization(0.55);
  EXPECT_CALL(callbacks_, onOrcaReport(_));
  stream_callbacks->onReceiveMessage(std::move(report2));
  EXPECT_EQ(stats_.reports_received_.value(), 2);
  EXPECT_EQ(stats_.stream_success_.value(), 1) << "latch must not re-fire on second message";
  EXPECT_EQ(stats_.stream_active_.value(), 1);

  // Destroy the client. Verify:
  //   - resetStream() is called on the live stream.
  //   - stream_terminated increments.
  //   - stream_active is set to 0.
  //   - onStreamClosed is NOT called (Decision #1 C - no callback from destructor).
  EXPECT_CALL(mock_stream_, resetStream());
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);

  client.reset();

  EXPECT_EQ(stats_.stream_terminated_.value(), 1);
  EXPECT_EQ(stats_.stream_active_.value(), 0);
}

// Sub-case: report_interval == 0ms encodes as Duration{0, 0} and is passed
// through unchanged per gRFC A51 (server clamps to its minimum).
TEST_F(OrcaOobClientTest, ReportIntervalZeroWireEncoding) {
  Buffer::InstancePtr captured_initial_msg;

  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(&mock_stream_));
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, true))
      .WillOnce(
          Invoke([&](Buffer::InstancePtr& msg, bool) { captured_initial_msg = std::move(msg); }));

  OrcaOobClient client(mock_async_client_, dispatcher_, std::chrono::milliseconds(0),
                       std::vector<std::string>{}, "host:1", stats_, callbacks_,
                       OrcaOobClient::defaultBackoffStrategy(random_));

  ASSERT_NE(captured_initial_msg, nullptr);
  const std::string payload = captured_initial_msg->toString();
  xds::service::orca::v3::OrcaLoadReportRequest parsed_req;
  ASSERT_TRUE(parsed_req.ParseFromString(payload));
  EXPECT_EQ(parsed_req.report_interval().seconds(), 0);
  EXPECT_EQ(parsed_req.report_interval().nanos(), 0);

  // Clean up without firing callback.
  EXPECT_CALL(mock_stream_, resetStream());
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);
}

// Transient-failure retry path:
//   - First start() returns null (cluster missing). Library schedules a retry
//     without firing any subscriber callback and without incrementing
//     stream_failure.
//   - Retry timer fires; second start() succeeds, then onRemoteClose(Unavailable)
//     closes the stream transiently. stream_failure increments and another
//     retry is scheduled with a larger delay.
//   - Third start() also closes transiently; delay ramps further.
// Uses Return(1700) for the random generator: with base=500/max=30000 the
// JitteredExponentialBackOffStrategy's internal next_interval_ doubles as
//   500 -> 1000 -> 2000 -> 4000 ...
// so `random % next_interval_` yields 200 -> 700 -> 1700, which is strictly
// monotonically increasing across the three failures this test observes.
TEST_F(OrcaOobClientTest, TransientFailuresRampBackoff) {
  // Pin jitter to 1700 so successive random()%next_interval_ calls return
  // 200, 700, 1700 - strictly monotonically increasing.
  ON_CALL(random_, random()).WillByDefault(Return(1700));

  // Capture the retry timer so we can simulate time passing. MockTimer installs
  // itself as the next timer returned from dispatcher_'s createTimer_() via an
  // EXPECT_CALL that RetiresOnSaturation.
  Event::MockTimer* retry_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  EXPECT_CALL(*retry_timer, enableTimer(_, _))
      .Times(testing::AtLeast(1))
      .WillRepeatedly(
          Invoke([retry_timer, this](std::chrono::milliseconds delay, const ScopeTrackedObject*) {
            last_retry_delay_ = delay;
            // Preserve the default side-effect of marking the timer enabled so
            // invokeCallback() works.
            retry_timer->enabled_ = true;
          }));

  // First start() returns null (cluster missing at call time). The library
  // should schedule a retry without firing any subscriber callback.
  Grpc::RawAsyncStream* first_return = nullptr;
  Grpc::RawAsyncStream* second_return = &mock_stream_;
  Grpc::RawAsyncStream* third_return = &mock_stream_;

  testing::InSequence seq;
  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(first_return));
  // No sendMessageRaw expected on the null-return path.
  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(second_return));
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, true));
  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(third_return));
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, true));

  // No report callback fires - we never deliver one.
  EXPECT_CALL(callbacks_, onOrcaReport(_)).Times(0);
  // No terminal callback fires either - these are transient failures.
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);

  auto client = std::make_unique<OrcaOobClient>(
      mock_async_client_, dispatcher_, std::chrono::milliseconds(7500), std::vector<std::string>{},
      "1.2.3.4:8080", stats_, callbacks_, OrcaOobClient::defaultBackoffStrategy(random_));

  // First null-start should have scheduled a retry with the base delay.
  const auto delay_after_first_failure = last_retry_delay_;
  EXPECT_GT(delay_after_first_failure.count(), 0);
  EXPECT_EQ(stats_.stream_failure_.value(), 0) << "null start does not increment stream_failure";

  // Fire the timer to trigger the first retry (which succeeds on startRaw but
  // closes transiently).
  retry_timer->invokeCallback();

  auto* stream_callbacks =
      static_cast<Grpc::AsyncStreamCallbacks<xds::data::orca::v3::OrcaLoadReport>*>(client.get());
  stream_callbacks->onRemoteClose(Grpc::Status::WellKnownGrpcStatus::Unavailable, "boom");

  const auto delay_after_second_failure = last_retry_delay_;
  EXPECT_GT(delay_after_second_failure, delay_after_first_failure)
      << "backoff should ramp on successive failures";
  EXPECT_EQ(stats_.stream_failure_.value(), 1);

  // Fire the timer again; next stream opens and closes again.
  retry_timer->invokeCallback();
  stream_callbacks->onRemoteClose(Grpc::Status::WellKnownGrpcStatus::Unavailable, "boom");

  const auto delay_after_third_failure = last_retry_delay_;
  EXPECT_GT(delay_after_third_failure, delay_after_second_failure) << "backoff should keep ramping";
  EXPECT_EQ(stats_.stream_failure_.value(), 2);

  // Clean up.
  EXPECT_CALL(mock_stream_, resetStream()).Times(testing::AtMost(1));
  client.reset();
}

// gRFC A51 "next attempt will occur immediately" latch: after a successful message
// has been received on a stream, the first retry following that stream's
// failure must fire at 0ms. Subsequent retries without another successful
// message ramp from the base delay again (the latch is one-shot).
TEST_F(OrcaOobClientTest, ImmediateRetryAfterFirstMessage) {
  // Pin jitter to 1700 (same as TransientFailuresRampBackoff) so that, after
  // the latch is cleared, the next backoff delay computes as
  //   1700 % 500 = 200 ms
  // - strictly greater than the 0 ms the latch produced. A Return(0) pin
  // would make BOTH delays 0 ms and the EXPECT_GT below would fail.
  ON_CALL(random_, random()).WillByDefault(Return(1700));

  Event::MockTimer* retry_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  EXPECT_CALL(*retry_timer, enableTimer(_, _))
      .WillRepeatedly(
          Invoke([retry_timer, this](std::chrono::milliseconds delay, const ScopeTrackedObject*) {
            last_retry_delay_ = delay;
            // Preserve the default side-effect of marking the timer enabled so
            // invokeCallback() works.
            retry_timer->enabled_ = true;
          }));

  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillRepeatedly(Return(&mock_stream_));
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, true)).Times(testing::AtLeast(1));
  EXPECT_CALL(callbacks_, onOrcaReport(_)).Times(testing::AtLeast(1));
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);

  auto client = std::make_unique<OrcaOobClient>(
      mock_async_client_, dispatcher_, std::chrono::milliseconds(7500), std::vector<std::string>{},
      "1.2.3.4:8080", stats_, callbacks_, OrcaOobClient::defaultBackoffStrategy(random_));

  auto* stream_callbacks =
      static_cast<Grpc::AsyncStreamCallbacks<xds::data::orca::v3::OrcaLoadReport>*>(client.get());

  // Deliver one report - this should set the first_message_received_ latch
  // and call backoff_->reset().
  auto report = std::make_unique<xds::data::orca::v3::OrcaLoadReport>();
  report->set_cpu_utilization(0.1);
  stream_callbacks->onReceiveMessage(std::move(report));
  EXPECT_EQ(stats_.stream_success_.value(), 1);
  EXPECT_EQ(stats_.stream_active_.value(), 1);

  // Close the stream. The retry should fire IMMEDIATELY (0ms) because the
  // latch is set.
  stream_callbacks->onRemoteClose(Grpc::Status::WellKnownGrpcStatus::Unavailable, "boom");
  EXPECT_EQ(last_retry_delay_, std::chrono::milliseconds(0))
      << "first retry after a successful message must be immediate per gRFC A51";

  // Fire the retry and close the new stream immediately (no message
  // received on it). The next retry should use the base delay, not another
  // 0ms - the latch is one-shot.
  retry_timer->invokeCallback();
  stream_callbacks->onRemoteClose(Grpc::Status::WellKnownGrpcStatus::Unavailable, "boom again");
  EXPECT_GT(last_retry_delay_, std::chrono::milliseconds(0))
      << "second failure without a message should ramp from base";

  // Clean up.
  EXPECT_CALL(mock_stream_, resetStream()).Times(testing::AtMost(1));
  client.reset();
}

TEST_F(OrcaOobClientTest, UnimplementedIsTerminal) {
  ON_CALL(random_, random()).WillByDefault(Return(0));
  Event::MockTimer* retry_timer = new NiceMock<Event::MockTimer>(&dispatcher_);

  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(&mock_stream_));
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, true));

  // Sub-case A: UNIMPLEMENTED on a fresh stream (no report received yet).
  auto client = std::make_unique<OrcaOobClient>(
      mock_async_client_, dispatcher_, std::chrono::milliseconds(30000), std::vector<std::string>{},
      "1.2.3.4:8080", stats_, callbacks_, OrcaOobClient::defaultBackoffStrategy(random_));

  auto* stream_callbacks =
      static_cast<Grpc::AsyncStreamCallbacks<xds::data::orca::v3::OrcaLoadReport>*>(client.get());

  // Terminal callback fires exactly once with UNIMPLEMENTED.
  EXPECT_CALL(callbacks_, onStreamClosed(Grpc::Status::WellKnownGrpcStatus::Unimplemented, _));
  // Retry timer MUST NOT be armed on UNIMPLEMENTED.
  EXPECT_CALL(*retry_timer, enableTimer(_, _)).Times(0);

  // gRFC A51 mandates an ERROR-level log message. Capture it via EXPECT_LOG_CONTAINS.
  EXPECT_LOG_CONTAINS("error", "does not implement OpenRcaService", {
    stream_callbacks->onRemoteClose(Grpc::Status::WellKnownGrpcStatus::Unimplemented,
                                    "not supported");
  });

  EXPECT_EQ(stats_.stream_terminated_.value(), 1);
  EXPECT_EQ(stats_.stream_active_.value(), 0);

  // Destructor is a no-op (client is already closed) and must not fire another
  // callback or double-increment stream_terminated.
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);
  client.reset();
  EXPECT_EQ(stats_.stream_terminated_.value(), 1);
}

TEST_F(OrcaOobClientTest, UnimplementedAfterSuccessfulReport) {
  // Sub-case B: UNIMPLEMENTED arriving on a stream that already received a
  // report. The first_message_received_ latch must not interfere with the
  // terminal path.
  ON_CALL(random_, random()).WillByDefault(Return(0));

  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(&mock_stream_));
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, true));

  auto client = std::make_unique<OrcaOobClient>(
      mock_async_client_, dispatcher_, std::chrono::milliseconds(30000), std::vector<std::string>{},
      "1.2.3.4:8080", stats_, callbacks_, OrcaOobClient::defaultBackoffStrategy(random_));

  auto* stream_callbacks =
      static_cast<Grpc::AsyncStreamCallbacks<xds::data::orca::v3::OrcaLoadReport>*>(client.get());

  // Deliver one report to set the latch.
  auto report = std::make_unique<xds::data::orca::v3::OrcaLoadReport>();
  EXPECT_CALL(callbacks_, onOrcaReport(_));
  stream_callbacks->onReceiveMessage(std::move(report));

  // Now fire UNIMPLEMENTED.
  EXPECT_CALL(callbacks_, onStreamClosed(Grpc::Status::WellKnownGrpcStatus::Unimplemented, _));
  stream_callbacks->onRemoteClose(Grpc::Status::WellKnownGrpcStatus::Unimplemented,
                                  "not supported");

  EXPECT_EQ(stats_.stream_terminated_.value(), 1);

  // Destructor is a no-op.
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);
}

TEST_F(OrcaOobClientTest, CloseCancelsActiveStreamAndRetryTimerNoCallbackFires) {
  ON_CALL(random_, random()).WillByDefault(Return(0));

  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(&mock_stream_));
  EXPECT_CALL(mock_stream_, sendMessageRaw_(_, true));

  // No subscriber callback should ever fire in this test.
  EXPECT_CALL(callbacks_, onOrcaReport(_)).Times(0);
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);

  auto client = std::make_unique<OrcaOobClient>(
      mock_async_client_, dispatcher_, std::chrono::milliseconds(30000), std::vector<std::string>{},
      "1.2.3.4:8080", stats_, callbacks_, OrcaOobClient::defaultBackoffStrategy(random_));

  {
    SCOPED_TRACE("Sub-scenario A: close() on a live stream");
    EXPECT_CALL(mock_stream_, resetStream());
    client->close();
    EXPECT_EQ(stats_.stream_terminated_.value(), 1);
    EXPECT_EQ(stats_.stream_active_.value(), 0);
    // Idempotent: second close() is a no-op.
    client->close();
    EXPECT_EQ(stats_.stream_terminated_.value(), 1);
  }

  // onRemoteClose after close() must be a no-op (reentrant guard).
  auto* stream_callbacks =
      static_cast<Grpc::AsyncStreamCallbacks<xds::data::orca::v3::OrcaLoadReport>*>(client.get());
  stream_callbacks->onRemoteClose(Grpc::Status::WellKnownGrpcStatus::Unavailable, "late");
  EXPECT_EQ(stats_.stream_terminated_.value(), 1);
  EXPECT_EQ(stats_.stream_failure_.value(), 0);

  // Destructor runs on `client.reset()` - still no callback, no double-increment.
  client.reset();
  EXPECT_EQ(stats_.stream_terminated_.value(), 1);
}

TEST_F(OrcaOobClientTest, CloseWhileRetryTimerPendingNoCallbackFires) {
  ON_CALL(random_, random()).WillByDefault(Return(0));

  Event::MockTimer* retry_timer = new NiceMock<Event::MockTimer>(&dispatcher_);

  // Force a transient failure path so the retry timer is pending.
  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).WillOnce(Return(nullptr));
  EXPECT_CALL(*retry_timer, enableTimer(_, _));

  EXPECT_CALL(callbacks_, onOrcaReport(_)).Times(0);
  EXPECT_CALL(callbacks_, onStreamClosed(_, _)).Times(0);

  auto client = std::make_unique<OrcaOobClient>(
      mock_async_client_, dispatcher_, std::chrono::milliseconds(30000), std::vector<std::string>{},
      "1.2.3.4:8080", stats_, callbacks_, OrcaOobClient::defaultBackoffStrategy(random_));

  // Close while timer is pending.
  EXPECT_CALL(*retry_timer, disableTimer());
  client->close();
  EXPECT_EQ(stats_.stream_terminated_.value(), 1);

  // Verify openStream() closed guard: force-fire the timer after close().
  EXPECT_CALL(*mock_async_client_, startRaw(_, _, _, _)).Times(0);
  retry_timer->enabled_ = true;
  retry_timer->invokeCallback();

  // Destructor no-op.
  client.reset();
  EXPECT_EQ(stats_.stream_terminated_.value(), 1);
}

} // namespace
} // namespace Orca
} // namespace Envoy
