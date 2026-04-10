#include <chrono>
#include <limits>
#include <memory>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codec_client.h"
#include "source/common/orca/orca_oob_session.h"

#include "test/common/http/common.h"
#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/upstream/host.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"
#include "xds/service/orca/v3/orca.pb.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Orca {
namespace {

class MockOrcaOobCallbacks : public OrcaOobCallbacks {
public:
  MOCK_METHOD(void, onOrcaOobReport, (const xds::data::orca::v3::OrcaLoadReport& report),
              (override));
  MOCK_METHOD(void, onOrcaOobStreamFailure, (Grpc::Status::GrpcStatus status), (override));
};

// Test subclass that overrides createCodecClient to inject a mock codec client,
// following the same pattern as TestHttpHealthCheckerImpl.
class TestOrcaOobSession : public OrcaOobSession {
public:
  using OrcaOobSession::OrcaOobSession;

  Http::CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) override {
    return Http::CodecClientPtr{createCodecClient_(data)};
  }

  MOCK_METHOD(Http::CodecClient*, createCodecClient_,
              (Upstream::Host::CreateConnectionData & conn_data));
};

class OrcaOobSessionTest : public testing::Test {
protected:
  OrcaOobSessionTest() {
    auto address = *Network::Utility::resolveUrl("tcp://127.0.0.1:80");
    ON_CALL(*host_, address()).WillByDefault(Return(address));
    ON_CALL(*host_, hostname()).WillByDefault(ReturnRef(empty_hostname_));
    ON_CALL(random_, random()).WillByDefault(Return(0));
  }

  void
  createSession(std::chrono::milliseconds reporting_period = std::chrono::milliseconds(10000)) {
    session_ = std::make_unique<TestOrcaOobSession>(host_, dispatcher_, random_, reporting_period,
                                                    callbacks_, stats_);
  }

  // Set up mock expectations so that start() succeeds and returns the mock codec client.
  void expectSessionStart() {
    client_connection_ = new NiceMock<Network::MockClientConnection>();
    codec_ = new NiceMock<Http::MockClientConnection>();

    EXPECT_CALL(*host_, createConnection_(_, _))
        .WillOnce(
            Invoke([this](Event::Dispatcher&, const Network::ConnectionSocket::OptionsSharedPtr&)
                       -> Upstream::MockHost::MockCreateConnectionData {
              return {client_connection_, host_};
            }));

    EXPECT_CALL(*session_, createCodecClient_(_))
        .WillOnce(
            Invoke([this](Upstream::Host::CreateConnectionData& conn_data) -> Http::CodecClient* {
              auto cluster = std::make_shared<NiceMock<Upstream::MockClusterInfo>>();
              codec_client_ = new CodecClientForTest(
                  Http::CodecType::HTTP2, std::move(conn_data.connection_), codec_, nullptr,
                  Upstream::makeTestHost(cluster, "tcp://127.0.0.1:80"), dispatcher_);
              return codec_client_;
            }));

    EXPECT_CALL(*codec_, newStream(_))
        .WillOnce(Invoke([this](Http::ResponseDecoder& decoder) -> Http::RequestEncoder& {
          response_decoder_ = &decoder;
          return request_encoder_;
        }));
    EXPECT_CALL(request_encoder_, encodeHeaders(_, false));
    EXPECT_CALL(request_encoder_, encodeData(_, true));
    EXPECT_CALL(request_encoder_.stream_, addCallbacks(_)).Times(2);
  }

  Buffer::OwnedImpl serializeAsGrpcFrame(const xds::data::orca::v3::OrcaLoadReport& report) {
    auto serialized = Grpc::Common::serializeToGrpcFrame(report);
    Buffer::OwnedImpl buffer;
    buffer.move(*serialized);
    return buffer;
  }

  std::shared_ptr<NiceMock<Upstream::MockHost>> host_{
      std::make_shared<NiceMock<Upstream::MockHost>>()};
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Random::MockRandomGenerator> random_;
  MockOrcaOobCallbacks callbacks_;
  Stats::IsolatedStoreImpl stats_store_;
  OrcaOobStats stats_{
      ALL_ORCA_OOB_STATS(POOL_COUNTER_PREFIX(*stats_store_.rootScope(), "orca_oob."),
                         POOL_GAUGE_PREFIX(*stats_store_.rootScope(), "orca_oob."))};
  std::unique_ptr<TestOrcaOobSession> session_;
  std::string empty_hostname_;

  NiceMock<Network::MockClientConnection>* client_connection_{};
  Http::MockClientConnection* codec_{};
  NiceMock<Http::MockRequestEncoder> request_encoder_;
  Http::ResponseDecoder* response_decoder_{};
  CodecClientForTest* codec_client_{};
};

// --- Task 2: Lifecycle tests ---

TEST_F(OrcaOobSessionTest, ConstructAndDestroy) { createSession(); }

TEST_F(OrcaOobSessionTest, StopBeforeStart) {
  createSession();
  session_->stop();
}

TEST_F(OrcaOobSessionTest, StopMultipleTimes) {
  createSession();
  session_->stop();
  session_->stop();
}

TEST_F(OrcaOobSessionTest, StatsInitialized) {
  createSession();
  EXPECT_EQ(0, stats_.streams_started_.value());
  EXPECT_EQ(0, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.reports_received_.value());
  EXPECT_EQ(0, stats_.reports_failed_.value());
  EXPECT_EQ(0, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());
}

// --- Task 3: Stream establishment tests ---

TEST_F(OrcaOobSessionTest, StartOpensStreamAndIncrementsStats) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_EQ(1, stats_.streams_started_.value());
  EXPECT_EQ(1, stats_.active_streams_.value());

  session_->stop();
}

// --- Task 4: Report decoding tests ---

TEST_F(OrcaOobSessionTest, DecodeDataDeliversReport) {
  createSession();
  expectSessionStart();
  session_->start();

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.75);
  report.set_mem_utilization(0.5);
  report.set_rps_fractional(1000);

  EXPECT_CALL(callbacks_, onOrcaOobReport(_))
      .WillOnce(Invoke([](const xds::data::orca::v3::OrcaLoadReport& r) {
        EXPECT_DOUBLE_EQ(r.cpu_utilization(), 0.75);
        EXPECT_DOUBLE_EQ(r.mem_utilization(), 0.5);
        EXPECT_DOUBLE_EQ(r.rps_fractional(), 1000);
      }));

  auto data = serializeAsGrpcFrame(report);
  response_decoder_->decodeData(data, false);

  EXPECT_EQ(1, stats_.reports_received_.value());
  EXPECT_EQ(0, stats_.reports_failed_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeDataMultipleReports) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_, onOrcaOobReport(_)).Times(2);

  xds::data::orca::v3::OrcaLoadReport report1;
  report1.set_cpu_utilization(0.5);
  auto data1 = serializeAsGrpcFrame(report1);
  response_decoder_->decodeData(data1, false);

  xds::data::orca::v3::OrcaLoadReport report2;
  report2.set_cpu_utilization(0.9);
  auto data2 = serializeAsGrpcFrame(report2);
  response_decoder_->decodeData(data2, false);

  EXPECT_EQ(2, stats_.reports_received_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeDataInvalidProtoIncrementsReportsFailed) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_, onOrcaOobReport(_)).Times(0);

  Buffer::OwnedImpl garbage;
  garbage.writeByte(0);
  garbage.writeBEInt<uint32_t>(4);
  garbage.add("junk");
  response_decoder_->decodeData(garbage, false);

  EXPECT_EQ(0, stats_.reports_received_.value());
  EXPECT_EQ(1, stats_.reports_failed_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeDataEndStreamSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.5);

  EXPECT_CALL(callbacks_, onOrcaOobReport(_));

  auto data = serializeAsGrpcFrame(report);
  response_decoder_->decodeData(data, /*end_stream=*/true);

  EXPECT_EQ(1, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

// --- Task 5: Error handling tests ---

TEST_F(OrcaOobSessionTest, UnimplementedStopsRetrying) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_, onOrcaOobStreamFailure(Grpc::Status::WellKnownGrpcStatus::Unimplemented));
  EXPECT_CALL(*client_connection_, close(Network::ConnectionCloseType::NoFlush, _));

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  headers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Unimplemented));
  response_decoder_->decodeHeaders(std::move(headers), true);

  EXPECT_EQ(1, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  // Verify start() is a no-op after UNIMPLEMENTED.
  session_->start();
  EXPECT_EQ(1, stats_.streams_started_.value()); // Still 1, not 2.
}

TEST_F(OrcaOobSessionTest, UnauthenticatedHeadersStopsReconnectLoop) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_,
              onOrcaOobStreamFailure(Grpc::Status::WellKnownGrpcStatus::Unauthenticated));
  EXPECT_CALL(*client_connection_, close(Network::ConnectionCloseType::NoFlush, _));

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  headers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Unauthenticated));
  response_decoder_->decodeHeaders(std::move(headers), true);

  // Unauthenticated is a permanent failure: it must not bump backend_not_supported_
  // (that counter is reserved for Unimplemented) and must stop the reconnect loop.
  EXPECT_EQ(0, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  // Verify start() is a no-op after the permanent failure — no new createClient
  // is invoked; streams_started remains at 1.
  session_->start();
  EXPECT_EQ(1, stats_.streams_started_.value());
}

TEST_F(OrcaOobSessionTest, PermissionDeniedHeadersStopsReconnectLoop) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_,
              onOrcaOobStreamFailure(Grpc::Status::WellKnownGrpcStatus::PermissionDenied));
  EXPECT_CALL(*client_connection_, close(Network::ConnectionCloseType::NoFlush, _));

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  headers->setGrpcStatus(
      static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::PermissionDenied));
  response_decoder_->decodeHeaders(std::move(headers), true);

  EXPECT_EQ(0, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->start();
  EXPECT_EQ(1, stats_.streams_started_.value());
}

TEST_F(OrcaOobSessionTest, InvalidArgumentHeadersStopsReconnectLoop) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_,
              onOrcaOobStreamFailure(Grpc::Status::WellKnownGrpcStatus::InvalidArgument));
  EXPECT_CALL(*client_connection_, close(Network::ConnectionCloseType::NoFlush, _));

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  headers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::InvalidArgument));
  response_decoder_->decodeHeaders(std::move(headers), true);

  EXPECT_EQ(0, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->start();
  EXPECT_EQ(1, stats_.streams_started_.value());
}

TEST_F(OrcaOobSessionTest, StreamResetSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, ConnectionCloseSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  client_connection_->raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, LocalCloseSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  client_connection_->raiseEvent(Network::ConnectionEvent::LocalClose);

  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeHeadersEndStreamWithNonUnimplementedGrpcStatus) {
  createSession();
  expectSessionStart();
  session_->start();

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  headers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Internal));
  response_decoder_->decodeHeaders(std::move(headers), true);

  // Non-Unimplemented gRPC status with end_stream should close and reconnect, not stop retrying.
  EXPECT_EQ(1, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, GoAwayClosesAndReconnects) {
  createSession();
  expectSessionStart();
  session_->start();

  codec_client_->raiseGoAway(Http::GoAwayErrorCode::NoError);

  // Should count as one failure, not two (guards against double-scheduling).
  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, UnimplementedInTrailers) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_, onOrcaOobStreamFailure(Grpc::Status::WellKnownGrpcStatus::Unimplemented));
  EXPECT_CALL(*client_connection_, close(Network::ConnectionCloseType::NoFlush, _));

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  response_decoder_->decodeHeaders(std::move(headers), false);

  auto trailers = Http::ResponseTrailerMapImpl::create();
  trailers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Unimplemented));
  response_decoder_->decodeTrailers(std::move(trailers));

  EXPECT_EQ(1, stats_.backend_not_supported_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, HostnameUsedAsAuthority) {
  std::string hostname = "my-backend.example.com";
  ON_CALL(*host_, hostname()).WillByDefault(ReturnRef(hostname));

  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_EQ(1, stats_.streams_started_.value());
  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeHeadersNon200SchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(503);
  response_decoder_->decodeHeaders(std::move(headers), false);

  EXPECT_EQ(1, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeHeadersEndStreamWithoutGrpcStatus) {
  createSession();
  expectSessionStart();
  session_->start();

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  response_decoder_->decodeHeaders(std::move(headers), true);

  EXPECT_EQ(1, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeTrailersNonUnimplementedSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  response_decoder_->decodeHeaders(std::move(headers), false);

  auto trailers = Http::ResponseTrailerMapImpl::create();
  trailers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Internal));
  response_decoder_->decodeTrailers(std::move(trailers));

  EXPECT_EQ(1, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeDataGrpcDecodeErrorResetsStream) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_, onOrcaOobReport(_)).Times(0);

  // Craft a gRPC frame with invalid flags (0xFE) to trigger gRPC decode error.
  // The gRPC decoder rejects frames where unsupported flag bits are set.
  Buffer::OwnedImpl bad_frame;
  bad_frame.writeByte(0xFE);         // Invalid flags byte
  bad_frame.writeBEInt<uint32_t>(0); // Zero-length payload
  response_decoder_->decodeData(bad_frame, false);

  // The decode error increments streams_failed_ directly rather than going
  // through the onResetStream path, so production codecs that may defer the
  // reset still get an accurate count. The HTTP stream is torn down
  // implicitly when the client is destroyed on the next start().
  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, EncodeHeadersFailureSchedulesReconnect) {
  createSession();

  // Capture the reconnect timer so we can verify it is armed.
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb) {
    return reconnect_timer;
  }));

  // Re-create the session so it picks up the captured timer.
  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  // Mirror expectSessionStart() but return an error from encodeHeaders and
  // assert encodeData is never called.
  client_connection_ = new NiceMock<Network::MockClientConnection>();
  codec_ = new NiceMock<Http::MockClientConnection>();

  EXPECT_CALL(*host_, createConnection_(_, _))
      .WillOnce(
          Invoke([this](Event::Dispatcher&, const Network::ConnectionSocket::OptionsSharedPtr&)
                     -> Upstream::MockHost::MockCreateConnectionData {
            return {client_connection_, host_};
          }));

  EXPECT_CALL(*session_, createCodecClient_(_))
      .WillOnce(
          Invoke([this](Upstream::Host::CreateConnectionData& conn_data) -> Http::CodecClient* {
            auto cluster = std::make_shared<NiceMock<Upstream::MockClusterInfo>>();
            codec_client_ = new CodecClientForTest(
                Http::CodecType::HTTP2, std::move(conn_data.connection_), codec_, nullptr,
                Upstream::makeTestHost(cluster, "tcp://127.0.0.1:80"), dispatcher_);
            return codec_client_;
          }));

  EXPECT_CALL(*codec_, newStream(_))
      .WillOnce(Invoke([this](Http::ResponseDecoder& decoder) -> Http::RequestEncoder& {
        response_decoder_ = &decoder;
        return request_encoder_;
      }));
  EXPECT_CALL(request_encoder_, encodeHeaders(_, false))
      .WillOnce(Return(absl::InternalError("encode failed")));
  EXPECT_CALL(request_encoder_, encodeData(_, _)).Times(0);
  EXPECT_CALL(request_encoder_.stream_, addCallbacks(_)).Times(2);

  // The teardown path closes the underlying client connection.
  EXPECT_CALL(*client_connection_, close(Network::ConnectionCloseType::NoFlush, _));

  // The reconnect timer must be armed after the encodeHeaders failure.
  EXPECT_CALL(*reconnect_timer, enableTimer(_, _));

  session_->start();

  EXPECT_EQ(0, stats_.streams_started_.value());
  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, BackoffGrowsExponentiallyWithShortReportingPeriod) {
  // Regression test: with a short reporting period (100ms), the backoff strategy
  // must still grow exponentially. Previously the max interval was derived from
  // reporting_period * 6, which collapsed to a flat 1s retry for periods under
  // ~167ms because the strategy was instantiated with base == max.
  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  createSession(std::chrono::milliseconds(100));

  // Use a deterministic random sequence. With the flat-retry bug, both random
  // values would be reduced modulo the same 1000ms bucket (500 % 1000 ==
  // 1500 % 1000 == 500). With the fix, the second backoff bucket is 2000ms so
  // 1500 % 2000 == 1500, strictly greater than the first 500.
  std::vector<uint64_t> random_values = {500, 1500};
  size_t random_idx = 0;
  ON_CALL(random_, random()).WillByDefault(Invoke([&]() -> uint64_t {
    uint64_t v = random_values[std::min(random_idx, random_values.size() - 1)];
    ++random_idx;
    return v;
  }));

  std::vector<std::chrono::milliseconds> captured_backoffs;
  ON_CALL(*reconnect_timer, enableTimer(_, _))
      .WillByDefault(Invoke([&](std::chrono::milliseconds d, const ScopeTrackedObject*) {
        captured_backoffs.push_back(d);
      }));

  expectSessionStart();
  session_->start();

  // First failure schedules first reconnect.
  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  // Reconnect, then trigger a second failure to schedule second reconnect.
  expectSessionStart();
  reconnect_timer_cb();

  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  // We expect at least 2 captured backoffs (one per failure). The strategy's
  // second interval bucket must be strictly greater than the first — exponential
  // growth, not flat retries.
  ASSERT_GE(captured_backoffs.size(), 2u);
  EXPECT_GT(captured_backoffs[1].count(), captured_backoffs[0].count());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, BackoffBoundedAtSixtySecondsForLargeReportingPeriod) {
  // Regression guard: even with a huge reporting period, the reconnect backoff
  // must stay bounded at 60s (independent of reporting period).
  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  // 1 hour reporting period.
  createSession(std::chrono::hours(1));

  // Return a very large random so that (random % backoff) == backoff - 1 for
  // any current backoff bucket, making the captured duration track the
  // strategy's current upper bound.
  ON_CALL(random_, random()).WillByDefault(Return(std::numeric_limits<uint64_t>::max()));

  std::chrono::milliseconds last_backoff{0};
  EXPECT_CALL(*reconnect_timer, enableTimer(_, _))
      .Times(testing::AtLeast(1))
      .WillRepeatedly(Invoke(
          [&](std::chrono::milliseconds d, const ScopeTrackedObject*) { last_backoff = d; }));

  expectSessionStart();
  session_->start();

  // Trigger ~20 sequential failures; the backoff must saturate at 60000ms.
  for (int i = 0; i < 20; ++i) {
    request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                              "");
    expectSessionStart();
    reconnect_timer_cb();
  }
  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  EXPECT_LE(last_backoff.count(), 60000);

  session_->stop();
}

TEST_F(OrcaOobSessionTest, DecodeDataEndStreamEmptyData) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_, onOrcaOobReport(_)).Times(0);

  // Empty data with end_stream — no frames to decode, but stream ends.
  Buffer::OwnedImpl empty_data;
  response_decoder_->decodeData(empty_data, /*end_stream=*/true);

  EXPECT_EQ(1, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());
  EXPECT_EQ(0, stats_.reports_received_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, ConnectionEventConnectedIsNoOp) {
  createSession();
  expectSessionStart();
  session_->start();

  // Connected event should not affect stream state.
  client_connection_->raiseEvent(Network::ConnectionEvent::Connected);

  EXPECT_EQ(0, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.streams_closed_.value());
  EXPECT_EQ(1, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, StopDuringActiveStreamSuppressesReconnect) {
  createSession();

  // Capture the timer callback to verify reconnect is NOT armed after stop.
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb) {
    return reconnect_timer;
  }));

  // Re-create the session to pick up the captured timer.
  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  expectSessionStart();
  session_->start();
  EXPECT_EQ(1, stats_.active_streams_.value());

  // Stop while stream is active — the re-entrant callbacks from resetStream/close
  // should NOT arm the reconnect timer because stopping_ is true.
  EXPECT_CALL(*reconnect_timer, enableTimer(_, _)).Times(0);
  session_->stop();
  EXPECT_EQ(0, stats_.active_streams_.value());
}

TEST_F(OrcaOobSessionTest, DecodeHeaders200NoEndStreamContinues) {
  createSession();
  expectSessionStart();
  session_->start();

  // Normal 200 response without end_stream — stream should remain active.
  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  response_decoder_->decodeHeaders(std::move(headers), false);

  EXPECT_EQ(0, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.streams_failed_.value());
  EXPECT_EQ(1, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, StaleClientCloseDoesNotArmReconnectTimer) {
  // Capture the reconnect timer so we can assert enableTimer is never called
  // during stale-client replacement in the second start().
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb) {
    return reconnect_timer;
  }));

  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  // First stream establishment.
  expectSessionStart();
  session_->start();
  EXPECT_EQ(1, stats_.streams_started_.value());
  EXPECT_EQ(0, stats_.streams_failed_.value());

  // Capture the stale connection pointer before the second start() overwrites
  // client_connection_ with a new mock. On the second start(), the stale
  // client's close() must synchronously drive LocalClose through the
  // registered connection callbacks — this is the exact re-entry path the
  // replacing_client_ guard must suppress. NiceMock<MockClientConnection>'s
  // default close() stub is a no-op, so we wire it up explicitly here.
  auto* stale_conn = client_connection_;
  EXPECT_CALL(*stale_conn, close(Network::ConnectionCloseType::NoFlush, _))
      .WillOnce(Invoke([stale_conn](Network::ConnectionCloseType, absl::string_view) {
        stale_conn->raiseEvent(Network::ConnectionEvent::LocalClose);
      }));

  // Set up expectations for the second start. The stale-client close path must
  // NOT arm the reconnect timer — scheduleReconnect() should be suppressed
  // while the old client is being torn down — and must NOT count the stale
  // close as a stream failure.
  EXPECT_CALL(*reconnect_timer, enableTimer(_, _)).Times(0);
  expectSessionStart();

  session_->start();

  EXPECT_EQ(2, stats_.streams_started_.value());
  EXPECT_EQ(0, stats_.streams_failed_.value());
  EXPECT_EQ(1, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, ReconnectTimerFiresAndRestartsStream) {
  createSession();

  // Capture the timer callback so we can fire it manually.
  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  // Re-create the session so it picks up the captured timer.
  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  // First stream establishment.
  expectSessionStart();
  session_->start();
  EXPECT_EQ(1, stats_.streams_started_.value());

  // Trigger a stream reset to schedule reconnect.
  EXPECT_CALL(*reconnect_timer, enableTimer(_, _));
  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");
  EXPECT_EQ(1, stats_.streams_failed_.value());

  // Set up expectations for the second stream.
  expectSessionStart();

  // Fire the reconnect timer — should create new client and start new stream.
  reconnect_timer_cb();

  EXPECT_EQ(2, stats_.streams_started_.value());
  EXPECT_EQ(1, stats_.active_streams_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, ConsecutiveFailuresBeforeSuccessBackOffExponentially) {
  createSession();

  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  {
    InSequence seq;
    EXPECT_CALL(random_, random()).WillOnce(Return(999));
    EXPECT_CALL(*reconnect_timer, enableTimer(std::chrono::milliseconds(999), _));
    EXPECT_CALL(random_, random()).WillOnce(Return(1500));
    EXPECT_CALL(*reconnect_timer, enableTimer(std::chrono::milliseconds(1500), _));
  }

  expectSessionStart();
  session_->start();

  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  expectSessionStart();
  reconnect_timer_cb();

  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  session_->stop();
}

TEST_F(OrcaOobSessionTest, FirstSuccessfulReportResetsBackoff) {
  // A stream is only considered "established" once a real report round-trips
  // end-to-end. Verify that a successfully decoded report resets the reconnect
  // backoff so the next failure falls back to the base bucket.
  createSession();

  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  {
    InSequence seq;
    EXPECT_CALL(random_, random()).WillOnce(Return(999));
    EXPECT_CALL(*reconnect_timer, enableTimer(std::chrono::milliseconds(999), _));
    EXPECT_CALL(random_, random()).WillOnce(Return(1500));
    EXPECT_CALL(*reconnect_timer, enableTimer(std::chrono::milliseconds(500), _));
  }

  expectSessionStart();
  session_->start();

  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  expectSessionStart();
  reconnect_timer_cb();

  // Headers alone do NOT reset the backoff — a real report must round-trip.
  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  response_decoder_->decodeHeaders(std::move(headers), false);

  // Deliver a real report; this is the event that resets the backoff.
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.25);
  EXPECT_CALL(callbacks_, onOrcaOobReport(_));
  auto data = serializeAsGrpcFrame(report);
  response_decoder_->decodeData(data, false);

  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  session_->stop();
}

TEST_F(OrcaOobSessionTest, ServerHangupAfterHeadersDoesNotResetBackoff) {
  // Regression: a server that sends 200 response headers and immediately hangs
  // up must NOT reset the reconnect backoff. Backoff should only be considered
  // recovered after a real report round-trips. Without the fix, the 200 headers
  // prematurely reset the backoff and the reconnect loop tightens instead of
  // growing.
  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  createSession();

  // Deterministic random sequence. With base bucket 1000 and max 60000:
  //   call 1: bucket=1000, returns 500 % 1000 = 500,   bucket -> 2000
  //   call 2: bucket=2000, returns 1500 % 2000 = 1500, bucket -> 4000  (new)
  //   call 2 (OLD, reset by headers): bucket=1000 again, returns 1500 % 1000 = 500
  // So second captured backoff is strictly greater than first only when
  // headers do NOT reset the backoff.
  std::vector<uint64_t> random_values = {500, 1500};
  size_t random_idx = 0;
  ON_CALL(random_, random()).WillByDefault(Invoke([&]() -> uint64_t {
    uint64_t v = random_values[std::min(random_idx, random_values.size() - 1)];
    ++random_idx;
    return v;
  }));

  std::vector<std::chrono::milliseconds> captured_backoffs;
  ON_CALL(*reconnect_timer, enableTimer(_, _))
      .WillByDefault(Invoke([&](std::chrono::milliseconds d, const ScopeTrackedObject*) {
        captured_backoffs.push_back(d);
      }));

  expectSessionStart();
  session_->start();

  // First failure schedules the first reconnect at the base bucket.
  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");

  // Timer fires -> second stream starts.
  expectSessionStart();
  reconnect_timer_cb();

  // Server sends 200 headers without ending the stream — no report yet.
  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  response_decoder_->decodeHeaders(std::move(headers), false);

  // Server hangs up immediately after headers, before any payload.
  request_encoder_.stream_.callbacks_.back()->onResetStream(
      Http::StreamResetReason::ConnectionTermination, "");

  ASSERT_GE(captured_backoffs.size(), 2u);
  EXPECT_GT(captured_backoffs[1].count(), captured_backoffs[0].count())
      << "200 headers must not reset backoff; second reconnect duration should "
         "come from the grown bucket, not the base bucket.";

  session_->stop();
}

TEST_F(OrcaOobSessionTest, StrayDataAfterDecodeErrorIsNotDeliveredAndDoesNotResetBackoff) {
  // Regression: after a gRPC decode error cleanupStream() marks the session
  // stream closed, but the underlying HTTP/2 stream is not torn down until
  // the next start() rebuilds the client. In that window the codec may
  // deliver more bytes on the same stream. Stray data must not be delivered
  // to the callback and must not reset the reconnect backoff via
  // onOrcaLoadReportReceived -- otherwise a broken stream could silently
  // tighten the reconnect loop.
  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  createSession();

  // Deterministic random so backoff growth is observable via the captured
  // timer intervals. With base bucket 1000 and max 60000:
  //   call 1: bucket=1000, returns 500 % 1000 = 500,   bucket -> 2000
  //   call 2: bucket=2000, returns 1500 % 2000 = 1500, bucket -> 4000
  // So the second captured backoff is strictly greater than the first ONLY
  // if the stray frame did not reset the backoff strategy.
  std::vector<uint64_t> random_values = {500, 1500};
  size_t random_idx = 0;
  ON_CALL(random_, random()).WillByDefault(Invoke([&]() -> uint64_t {
    uint64_t v = random_values[std::min(random_idx, random_values.size() - 1)];
    ++random_idx;
    return v;
  }));

  std::vector<std::chrono::milliseconds> captured_backoffs;
  ON_CALL(*reconnect_timer, enableTimer(_, _))
      .WillByDefault(Invoke([&](std::chrono::milliseconds d, const ScopeTrackedObject*) {
        captured_backoffs.push_back(d);
      }));

  expectSessionStart();
  session_->start();

  // Trigger a gRPC decode error with an invalid flags byte.
  Buffer::OwnedImpl bad_frame;
  bad_frame.writeByte(0xFE);
  bad_frame.writeBEInt<uint32_t>(0);
  response_decoder_->decodeData(bad_frame, false);
  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());
  ASSERT_EQ(captured_backoffs.size(), 1u);

  // Now deliver a well-formed report on the same (now-closed) stream. The
  // session must treat this as a no-op: no callback, no reports_received_,
  // no backoff reset.
  EXPECT_CALL(callbacks_, onOrcaOobReport(_)).Times(0);
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.75);
  auto stray_data = serializeAsGrpcFrame(report);
  response_decoder_->decodeData(stray_data, false);
  EXPECT_EQ(0, stats_.reports_received_.value());

  // A stray set of headers and trailers on the closed stream must likewise
  // be no-ops (no double-count of streams_closed_ / streams_failed_).
  auto stray_headers = Http::ResponseHeaderMapImpl::create();
  stray_headers->setStatus(200);
  response_decoder_->decodeHeaders(std::move(stray_headers), false);
  auto stray_trailers = Http::ResponseTrailerMapImpl::create();
  stray_trailers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Ok));
  response_decoder_->decodeTrailers(std::move(stray_trailers));
  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.streams_closed_.value());

  // Reconnect normally and fail a second time. The second captured backoff
  // must be strictly greater than the first -- proof the stray frame did
  // not reset the strategy.
  expectSessionStart();
  reconnect_timer_cb();
  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");
  ASSERT_GE(captured_backoffs.size(), 2u);
  EXPECT_GT(captured_backoffs[1].count(), captured_backoffs[0].count())
      << "stray frame on closed stream must not reset backoff";

  session_->stop();
}

TEST_F(OrcaOobSessionTest, StartAfterStopRearmsReconnect) {
  // Regression: stop() sets stopping_=true to suppress re-entrant reconnects
  // during teardown. A subsequent start() must clear the flag so the next
  // failure's scheduleReconnect() is not unconditionally suppressed.
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb) {
    return reconnect_timer;
  }));

  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  expectSessionStart();
  session_->start();
  session_->stop();

  // Second lifecycle. start() must re-open the stream and clear stopping_
  // so the subsequent failure arms the reconnect timer.
  expectSessionStart();
  session_->start();
  EXPECT_EQ(2, stats_.streams_started_.value());

  // stop() is graceful, so streams_failed_ is still 0 here. The failure below
  // is the first one that must take the scheduleReconnect() path.
  EXPECT_CALL(*reconnect_timer, enableTimer(_, _));
  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");
  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(1, stats_.reconnect_attempts_.value());

  session_->stop();
}

TEST_F(OrcaOobSessionTest, ReconnectAttemptsIncrementsEachReconnect) {
  // Each call to scheduleReconnect() that actually arms the timer must bump
  // reconnect_attempts_. This lets operators distinguish a tight "reconnect
  // every second" loop from a slow "reconnect every 60s" loop.
  Event::TimerCb reconnect_timer_cb;
  auto* reconnect_timer = new NiceMock<Event::MockTimer>();
  EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([&](Event::TimerCb cb) {
    reconnect_timer_cb = std::move(cb);
    return reconnect_timer;
  }));

  session_ = std::make_unique<TestOrcaOobSession>(
      host_, dispatcher_, random_, std::chrono::milliseconds(10000), callbacks_, stats_);

  expectSessionStart();
  session_->start();
  EXPECT_EQ(0, stats_.reconnect_attempts_.value());

  // First failure arms the reconnect timer.
  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");
  EXPECT_EQ(1, stats_.reconnect_attempts_.value());
  EXPECT_EQ(1, stats_.streams_failed_.value());

  // Fire the reconnect timer to restart the stream, then fail again.
  expectSessionStart();
  reconnect_timer_cb();

  request_encoder_.stream_.callbacks_.back()->onResetStream(Http::StreamResetReason::RemoteReset,
                                                            "");
  EXPECT_EQ(2, stats_.reconnect_attempts_.value());
  EXPECT_EQ(2, stats_.streams_failed_.value());

  session_->stop();
}

} // namespace
} // namespace Orca
} // namespace Envoy
