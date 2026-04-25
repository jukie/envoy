#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/grpc/status.h"
#include "envoy/http/codec.h"
#include "envoy/http/stream_reset_handler.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/buffer/zero_copy_input_stream_impl.h"
#include "source/common/grpc/codec.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codec_client.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/orca/orca_oob_session.h"

#include "test/common/http/common.h"
#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/http/stream_encoder.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"
#include "xds/service/orca/v3/orca.pb.h"

namespace Envoy {
namespace Orca {
namespace {

using testing::_;
using testing::DoAll;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

// Per-attempt mocks. The codec-client factory captured by the session
// constructs one of these for each connection attempt.
struct AttemptMocks {
  NiceMock<Network::MockClientConnection>* network_connection{nullptr};
  NiceMock<Http::MockClientConnection>* codec{nullptr};
  NiceMock<Http::MockRequestEncoder> request_encoder;
  Http::ResponseDecoder* response_decoder{nullptr};
  CodecClientForTest* codec_client{nullptr};
  bool encode_headers_called{false};
  bool encode_data_called{false};
  // The OrcaLoadReportRequest parsed off the wire when the session sent
  // its first (and only) data frame on this attempt.
  xds::service::orca::v3::OrcaLoadReportRequest parsed_request;
  // Captured :scheme pseudo-header value.
  std::string captured_scheme;
};

// Drives stream reset through the mock encoder's stream so that the
// session's StreamCallbacks::onResetStream is invoked.
void triggerStreamReset(AttemptMocks& attempt, Http::StreamResetReason reason) {
  for (auto* cb : attempt.request_encoder.stream_.callbacks_) {
    if (cb != nullptr) {
      cb->onResetStream(reason, absl::string_view());
    }
  }
}

class OrcaOobSessionTest : public Event::TestUsingSimulatedTime, public testing::Test {
public:
  OrcaOobSessionTest() {
    cluster_info_ = std::make_shared<NiceMock<Upstream::MockClusterInfo>>();
    backoff_ = new NiceMock<MockBackOffStrategy>();
    ON_CALL(*backoff_, nextBackOffMs()).WillByDefault(Return(100));
  }

  void createSession(std::chrono::milliseconds report_interval = std::chrono::milliseconds(1000),
                     std::vector<std::string> request_costs = {"cpu_utilization"},
                     bool upstream_secure_transport = false) {
    // MockTimer(MockDispatcher*) registers an EXPECT_CALL on the dispatcher's
    // createTimer_; expectations match LIFO. The session constructs the
    // retry timer first and the GOAWAY drain timer second, so we must
    // register them in REVERSE order here so LIFO matching pairs them up
    // correctly.
    goaway_timer_ = new NiceMock<Event::MockTimer>(&dispatcher_);
    retry_timer_ = new NiceMock<Event::MockTimer>(&dispatcher_);
    session_ = std::make_unique<OrcaOobSession>(
        [this]() -> Http::CodecClientPtr { return makeCodecClient(); }, dispatcher_,
        report_interval, std::move(request_costs), upstream_secure_transport,
        [this](const xds::data::orca::v3::OrcaLoadReport& report) { reports_.push_back(report); },
        [this](Grpc::Status::GrpcStatus status, absl::string_view message) {
          terminated_status_ = status;
          terminated_message_ = std::string(message);
          terminated_ = true;
        },
        [this](OrcaOobLifecycleEvent event) { lifecycle_events_.push_back(event); },
        BackOffStrategyPtr(backoff_));
  }

  Http::CodecClientPtr makeCodecClient() {
    auto attempt = std::make_unique<AttemptMocks>();
    attempt->network_connection = new NiceMock<Network::MockClientConnection>();
    attempt->codec = new NiceMock<Http::MockClientConnection>();
    EXPECT_CALL(*attempt->codec, newStream(_))
        .WillOnce(DoAll(Envoy::SaveArgAddress(&attempt->response_decoder),
                        ReturnRef(attempt->request_encoder)));

    EXPECT_CALL(attempt->request_encoder, encodeHeaders(_, false))
        .WillOnce(Invoke([attempt_ptr = attempt.get()](const Http::RequestHeaderMap& headers,
                                                       bool) -> Http::Status {
          EXPECT_EQ(Http::Headers::get().ContentTypeValues.Grpc, headers.getContentTypeValue());
          EXPECT_EQ("/xds.service.orca.v3.OpenRcaService/StreamCoreMetrics",
                    headers.getPathValue());
          EXPECT_EQ("orca-oob", headers.getHostValue());
          attempt_ptr->captured_scheme = std::string(headers.getSchemeValue());
          attempt_ptr->encode_headers_called = true;
          return Http::okStatus();
        }));
    EXPECT_CALL(attempt->request_encoder, encodeData(_, true))
        .WillOnce(Invoke([attempt_ptr = attempt.get()](Buffer::Instance& data, bool) {
          std::vector<Grpc::Frame> frames;
          Grpc::Decoder decoder;
          ASSERT_TRUE(decoder.decode(data, frames).ok());
          ASSERT_EQ(1u, frames.size());
          Buffer::ZeroCopyInputStreamImpl stream(std::move(frames[0].data_));
          ASSERT_TRUE(attempt_ptr->parsed_request.ParseFromZeroCopyStream(&stream));
          attempt_ptr->encode_data_called = true;
        }));

    Network::ClientConnectionPtr conn{attempt->network_connection};
    auto codec_client = std::make_unique<CodecClientForTest>(
        Http::CodecType::HTTP2, std::move(conn), attempt->codec, nullptr,
        Upstream::makeTestHost(cluster_info_, "tcp://127.0.0.1:9000"), dispatcher_);
    attempt->codec_client = codec_client.get();
    attempts_.push_back(std::move(attempt));
    return codec_client;
  }

  AttemptMocks& currentAttempt() {
    EXPECT_FALSE(attempts_.empty());
    return *attempts_.back();
  }

  void respondHeadersOk(AttemptMocks& attempt) {
    auto headers =
        std::make_unique<Http::TestResponseHeaderMapImpl>(Http::TestResponseHeaderMapImpl(
            {{":status", "200"}, {"content-type", "application/grpc"}}));
    attempt.response_decoder->decodeHeaders(std::move(headers), false);
  }

  void respondReport(AttemptMocks& attempt, const xds::data::orca::v3::OrcaLoadReport& report,
                     bool end_stream = false) {
    auto frame = Grpc::Common::serializeToGrpcFrame(report);
    attempt.response_decoder->decodeData(*frame, end_stream);
  }

  void respondTrailers(AttemptMocks& attempt, Grpc::Status::GrpcStatus status,
                       const std::string& message = "") {
    auto trailers = std::make_unique<Http::TestResponseTrailerMapImpl>(
        Http::TestResponseTrailerMapImpl({{"grpc-status", absl::StrCat(status)}}));
    if (!message.empty()) {
      trailers->addCopy(Http::Headers::get().GrpcMessage, message);
    }
    attempt.response_decoder->decodeTrailers(std::move(trailers));
  }

  // Drive a successful end-to-end attempt with one report.
  void runSuccessfulAttempt(AttemptMocks& attempt, double cpu_utilization) {
    respondHeadersOk(attempt);
    xds::data::orca::v3::OrcaLoadReport report;
    report.set_cpu_utilization(cpu_utilization);
    respondReport(attempt, report);
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  std::shared_ptr<Upstream::MockClusterInfo> cluster_info_;
  NiceMock<MockBackOffStrategy>* backoff_; // ownership transferred to session
  NiceMock<Event::MockTimer>* retry_timer_{nullptr};
  NiceMock<Event::MockTimer>* goaway_timer_{nullptr};
  std::vector<std::unique_ptr<AttemptMocks>> attempts_;
  std::vector<xds::data::orca::v3::OrcaLoadReport> reports_;
  std::vector<OrcaOobLifecycleEvent> lifecycle_events_;
  bool terminated_{false};
  Grpc::Status::GrpcStatus terminated_status_{Grpc::Status::WellKnownGrpcStatus::Ok};
  std::string terminated_message_;
  std::unique_ptr<OrcaOobSession> session_;
};

// Open stream, server replies with headers + a single OrcaLoadReport
// frame; lifecycle StreamOpen fires and the report callback observes the
// payload. Also asserts that the session encoded the report interval,
// requested cost names, and upstream scheme into the request.
TEST_F(OrcaOobSessionTest, HappyPathReceivesReport) {
  createSession(std::chrono::milliseconds(1000), {"cpu_utilization"});
  session_->start();
  ASSERT_EQ(1u, attempts_.size());
  EXPECT_TRUE(attempts_[0]->encode_headers_called);
  EXPECT_TRUE(attempts_[0]->encode_data_called);

  // Body content: report interval round-trips and cost names made it into
  // the request.
  const auto& req = attempts_[0]->parsed_request;
  ASSERT_TRUE(req.has_report_interval());
  EXPECT_EQ(1, req.report_interval().seconds());
  EXPECT_EQ(0, req.report_interval().nanos());
  ASSERT_EQ(1, req.request_cost_names_size());
  EXPECT_EQ("cpu_utilization", req.request_cost_names(0));

  EXPECT_EQ("http", attempts_[0]->captured_scheme);

  respondHeadersOk(*attempts_[0]);
  ASSERT_EQ(1u, lifecycle_events_.size());
  EXPECT_EQ(OrcaOobLifecycleEvent::StreamOpen, lifecycle_events_[0]);

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.42);
  respondReport(*attempts_[0], report);

  ASSERT_EQ(1u, reports_.size());
  EXPECT_DOUBLE_EQ(0.42, reports_[0].cpu_utilization());
  EXPECT_FALSE(terminated_);

  session_->close();
}

TEST_F(OrcaOobSessionTest, SecureUpstreamSetsHttpsScheme) {
  createSession(std::chrono::milliseconds(1000), {"cpu_utilization"},
                /*upstream_secure_transport=*/true);
  session_->start();
  ASSERT_EQ(1u, attempts_.size());
  EXPECT_EQ("https", attempts_[0]->captured_scheme);

  session_->close();
}

// Sub-second report intervals must split correctly into seconds + nanos
// rather than dropping the fractional portion. 1500ms => 1s + 500_000_000ns.
TEST_F(OrcaOobSessionTest, SubSecondReportIntervalSplitsSecondsAndNanos) {
  createSession(std::chrono::milliseconds(1500), {"cpu_utilization"});
  session_->start();
  ASSERT_EQ(1u, attempts_.size());
  ASSERT_TRUE(attempts_[0]->encode_data_called);

  const auto& req = attempts_[0]->parsed_request;
  ASSERT_TRUE(req.has_report_interval());
  EXPECT_EQ(1, req.report_interval().seconds());
  EXPECT_EQ(500'000'000, req.report_interval().nanos());

  session_->close();
}

// Non-default cost names propagate into the request, in order. This guards
// against accidentally dropping or reordering entries.
TEST_F(OrcaOobSessionTest, RequestCostNamesAreEncoded) {
  createSession(std::chrono::milliseconds(1000), {"named_metrics.foo", "named_metrics.bar"});
  session_->start();
  ASSERT_EQ(1u, attempts_.size());
  ASSERT_TRUE(attempts_[0]->encode_data_called);

  const auto& req = attempts_[0]->parsed_request;
  ASSERT_EQ(2, req.request_cost_names_size());
  EXPECT_EQ("named_metrics.foo", req.request_cost_names(0));
  EXPECT_EQ("named_metrics.bar", req.request_cost_names(1));

  session_->close();
}

// Multiple reports in one stream are all surfaced.
TEST_F(OrcaOobSessionTest, MultipleReports) {
  createSession();
  session_->start();
  respondHeadersOk(*attempts_[0]);
  for (int i = 0; i < 3; ++i) {
    xds::data::orca::v3::OrcaLoadReport report;
    report.set_cpu_utilization(0.1 * (i + 1));
    respondReport(*attempts_[0], report);
  }
  EXPECT_EQ(3u, reports_.size());
  session_->close();
}

// UNIMPLEMENTED is terminal: on_terminated_ fires and no retry is
// scheduled.
TEST_F(OrcaOobSessionTest, UnimplementedIsTerminal) {
  createSession();
  EXPECT_CALL(*retry_timer_, enableTimer(_, _)).Times(0);

  session_->start();
  respondHeadersOk(*attempts_[0]);
  respondTrailers(*attempts_[0], Grpc::Status::WellKnownGrpcStatus::Unimplemented, "no service");

  EXPECT_TRUE(terminated_);
  EXPECT_EQ(Grpc::Status::WellKnownGrpcStatus::Unimplemented, terminated_status_);
  EXPECT_EQ(1u, attempts_.size());
}

// Non-200 response with end_stream=false and no trailers must route
// through Grpc::Utility::httpToGrpcStatus and the transient-failure path
// (i.e. schedule a retry, NOT terminate). HTTP 500 maps to gRPC Unknown.
TEST_F(OrcaOobSessionTest, Non200NoTrailersIsTransientFailure) {
  createSession();
  EXPECT_CALL(*backoff_, nextBackOffMs()).WillOnce(Return(75));
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(75), _));

  session_->start();
  // 500 response with end_stream=false: no grpc-status header, server
  // hasn't ended the stream cleanly. The session should reset the stream
  // itself and queue a retry rather than fire on_terminated_.
  auto headers = std::make_unique<Http::TestResponseHeaderMapImpl>(
      Http::TestResponseHeaderMapImpl({{":status", "500"}}));
  attempts_[0]->response_decoder->decodeHeaders(std::move(headers), false);

  EXPECT_FALSE(terminated_);
  ASSERT_EQ(1u, lifecycle_events_.size());
  EXPECT_EQ(OrcaOobLifecycleEvent::StreamFailure, lifecycle_events_[0]);

  retry_timer_->invokeCallback();
  ASSERT_EQ(2u, attempts_.size());
}

// Trailers-only UNIMPLEMENTED also terminates.
TEST_F(OrcaOobSessionTest, UnimplementedHeadersOnlyIsTerminal) {
  createSession();
  EXPECT_CALL(*retry_timer_, enableTimer(_, _)).Times(0);
  session_->start();
  auto headers = std::make_unique<Http::TestResponseHeaderMapImpl>(Http::TestResponseHeaderMapImpl(
      {{":status", "200"}, {"content-type", "application/grpc"}, {"grpc-status", "12"}}));
  attempts_[0]->response_decoder->decodeHeaders(std::move(headers), true);
  EXPECT_TRUE(terminated_);
  EXPECT_EQ(Grpc::Status::WellKnownGrpcStatus::Unimplemented, terminated_status_);
}

// Transient failure schedules a backoff retry; firing the retry timer
// creates a new connection attempt.
TEST_F(OrcaOobSessionTest, TransientFailureSchedulesBackoffAndRetries) {
  createSession();
  EXPECT_CALL(*backoff_, nextBackOffMs()).WillOnce(Return(250));
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(250), _));

  session_->start();
  respondHeadersOk(*attempts_[0]);
  triggerStreamReset(*attempts_[0], Http::StreamResetReason::RemoteReset);

  ASSERT_EQ(2u, lifecycle_events_.size());
  EXPECT_EQ(OrcaOobLifecycleEvent::StreamOpen, lifecycle_events_[0]);
  EXPECT_EQ(OrcaOobLifecycleEvent::StreamFailure, lifecycle_events_[1]);
  EXPECT_FALSE(terminated_);

  retry_timer_->invokeCallback();
  ASSERT_EQ(2u, attempts_.size());
  EXPECT_TRUE(attempts_[1]->encode_headers_called);
}

// First successful report resets backoff and arms the immediate-retry
// budget; the next failure retries with zero delay rather than consulting
// the strategy.
TEST_F(OrcaOobSessionTest, FirstReportResetsBackoffAndImmediateRetry) {
  createSession();
  EXPECT_CALL(*backoff_, reset()).Times(testing::AtLeast(1));
  EXPECT_CALL(*backoff_, nextBackOffMs()).Times(0);
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(0), _));

  session_->start();
  runSuccessfulAttempt(*attempts_[0], 0.1);
  EXPECT_EQ(1u, reports_.size());

  triggerStreamReset(*attempts_[0], Http::StreamResetReason::RemoteReset);

  ASSERT_EQ(2u, lifecycle_events_.size());
  EXPECT_EQ(OrcaOobLifecycleEvent::StreamOpen, lifecycle_events_[0]);
  EXPECT_EQ(OrcaOobLifecycleEvent::StreamFailure, lifecycle_events_[1]);
}

// After the immediate-retry budget is consumed by the first
// post-success failure, the next failure consults the backoff strategy.
TEST_F(OrcaOobSessionTest, SecondFailureUsesBackoff) {
  createSession();
  EXPECT_CALL(*backoff_, reset()).Times(testing::AtLeast(1));
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(0), _));

  session_->start();
  runSuccessfulAttempt(*attempts_[0], 0.1);
  triggerStreamReset(*attempts_[0], Http::StreamResetReason::RemoteReset);

  testing::Mock::VerifyAndClearExpectations(retry_timer_);
  EXPECT_CALL(*backoff_, nextBackOffMs()).WillOnce(Return(500));
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(500), _));

  retry_timer_->invokeCallback();
  ASSERT_EQ(2u, attempts_.size());
  triggerStreamReset(*attempts_[1], Http::StreamResetReason::RemoteReset);
}

// close() suppresses subsequent callbacks.
TEST_F(OrcaOobSessionTest, CloseSuppressesCallbacks) {
  createSession();
  session_->start();
  respondHeadersOk(*attempts_[0]);
  EXPECT_EQ(1u, lifecycle_events_.size());

  session_->close();

  // Even if the response decoder were to be driven further, callbacks
  // should be suppressed.
  EXPECT_TRUE(reports_.empty());
  EXPECT_FALSE(terminated_);
}

// Destruction does not fire callbacks.
TEST_F(OrcaOobSessionTest, DtorSuppressesCallbacks) {
  createSession();
  session_->start();
  respondHeadersOk(*attempts_[0]);
  EXPECT_EQ(1u, lifecycle_events_.size());

  session_.reset();
  EXPECT_FALSE(terminated_);
}

// GOAWAY(NoError) starts a drain timer; reports continue to flow until
// the timer fires; on timer expiry, the session reconnects.
TEST_F(OrcaOobSessionTest, GoAwayNoErrorDrainsThenReconnects) {
  createSession();
  EXPECT_CALL(*goaway_timer_, enableTimer(OrcaOobSession::kGoAwayDrainDeadline, _));

  session_->start();
  respondHeadersOk(*attempts_[0]);

  attempts_[0]->codec_client->raiseGoAway(Http::GoAwayErrorCode::NoError);

  // Reports during drain are still delivered.
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.7);
  respondReport(*attempts_[0], report);
  EXPECT_EQ(1u, reports_.size());

  // After the drain timer fires, the session treats it as a transient
  // failure. Because we already saw a report, the immediate-retry
  // budget applies.
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(0), _));
  goaway_timer_->invokeCallback();

  retry_timer_->invokeCallback();
  EXPECT_EQ(2u, attempts_.size());
}

// If the draining attempt fails before the GOAWAY drain deadline, the stale
// drain timer must be disabled so it cannot fire against a later retry.
TEST_F(OrcaOobSessionTest, GoAwayNoErrorEarlyFailureDisablesDrainTimer) {
  createSession();
  EXPECT_CALL(*goaway_timer_, enableTimer(OrcaOobSession::kGoAwayDrainDeadline, _));
  EXPECT_CALL(*backoff_, nextBackOffMs()).WillOnce(Return(321));
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(321), _));

  session_->start();
  respondHeadersOk(*attempts_[0]);
  attempts_[0]->codec_client->raiseGoAway(Http::GoAwayErrorCode::NoError);

  EXPECT_CALL(*goaway_timer_, disableTimer());
  triggerStreamReset(*attempts_[0], Http::StreamResetReason::RemoteReset);
  testing::Mock::VerifyAndClearExpectations(goaway_timer_);

  retry_timer_->invokeCallback();
  EXPECT_EQ(2u, attempts_.size());
}

// GOAWAY with a non-NoError code is a transient failure.
TEST_F(OrcaOobSessionTest, GoAwayOtherIsTransientFailure) {
  createSession();
  EXPECT_CALL(*goaway_timer_, enableTimer(_, _)).Times(0);
  EXPECT_CALL(*backoff_, nextBackOffMs()).WillOnce(Return(123));
  EXPECT_CALL(*retry_timer_, enableTimer(std::chrono::milliseconds(123), _));

  session_->start();
  respondHeadersOk(*attempts_[0]);
  attempts_[0]->codec_client->raiseGoAway(Http::GoAwayErrorCode::Other);

  ASSERT_EQ(2u, lifecycle_events_.size());
  EXPECT_EQ(OrcaOobLifecycleEvent::StreamFailure, lifecycle_events_[1]);
}

// Each retry constructs a fresh codec client.
TEST_F(OrcaOobSessionTest, RecreatesCodecClientPerAttempt) {
  createSession();
  session_->start();
  respondHeadersOk(*attempts_[0]);
  triggerStreamReset(*attempts_[0], Http::StreamResetReason::RemoteReset);

  retry_timer_->invokeCallback();
  ASSERT_EQ(2u, attempts_.size());
  EXPECT_NE(attempts_[0]->codec_client, attempts_[1]->codec_client);
}

} // namespace
} // namespace Orca
} // namespace Envoy
