#include "source/common/orca/orca_oob_session.h"

#include <chrono>
#include <memory>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codec_client.h"

#include "test/common/http/common.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/upstream/host.h"
#include "test/common/upstream/utility.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"
#include "xds/service/orca/v3/orca.pb.h"

using testing::_;
using testing::AtLeast;
using testing::DoAll;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;

namespace Envoy {
namespace Orca {
namespace {

class MockOrcaOobCallbacks : public OrcaOobCallbacks {
public:
  MOCK_METHOD(void, onOrcaOobReport,
              (const xds::data::orca::v3::OrcaLoadReport& report), (override));
  MOCK_METHOD(void, onOrcaOobStreamFailure, (Grpc::Status::GrpcStatus status), (override));
};

// Test subclass that overrides createCodecClient to inject a mock codec client,
// following the same pattern as TestHttpHealthCheckerImpl.
class TestOrcaOobSession : public OrcaOobSession {
public:
  using OrcaOobSession::OrcaOobSession;

  Http::CodecClientPtr
  createCodecClient(Upstream::Host::CreateConnectionData& conn_data) override {
    return Http::CodecClientPtr{createCodecClient_(conn_data)};
  }

  MOCK_METHOD(Http::CodecClient*, createCodecClient_,
              (Upstream::Host::CreateConnectionData& conn_data));
};

class OrcaOobSessionTest : public testing::Test {
protected:
  OrcaOobSessionTest() {
    auto address = *Network::Utility::resolveUrl("tcp://127.0.0.1:80");
    ON_CALL(*host_, address()).WillByDefault(Return(address));
    ON_CALL(*host_, hostname()).WillByDefault(ReturnRef(empty_hostname_));
    ON_CALL(random_, random()).WillByDefault(Return(0));
  }

  void createSession(
      std::chrono::milliseconds reporting_period = std::chrono::milliseconds(10000)) {
    session_ = std::make_unique<TestOrcaOobSession>(host_, dispatcher_, random_, reporting_period,
                                                     callbacks_, stats_);
  }

  // Set up mock expectations so that start() succeeds and returns the mock codec client.
  // Returns the raw pointer to the mock request encoder for sending responses.
  void expectSessionStart() {
    // Allocate mock connection and codec - ownership is transferred to
    // CodecClientForTest via the createCodecClient_ mock.
    client_connection_ = new NiceMock<Network::MockClientConnection>();
    codec_ = new NiceMock<Http::MockClientConnection>();

    EXPECT_CALL(*host_, createConnection_(_, _))
        .WillOnce(Invoke([this](Event::Dispatcher&,
                                const Network::ConnectionSocket::OptionsSharedPtr&)
                             -> Upstream::MockHost::MockCreateConnectionData {
          return {client_connection_, host_};
        }));

    EXPECT_CALL(*session_, createCodecClient_(_))
        .WillOnce(Invoke([this](Upstream::Host::CreateConnectionData& conn_data)
                             -> Http::CodecClient* {
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
    // Clear any callbacks from previous test iterations.
    request_encoder_.stream_.callbacks_.clear();
    EXPECT_CALL(request_encoder_, encodeHeaders(_, false));
    EXPECT_CALL(request_encoder_, encodeData(_, true));
    // addCallbacks is called twice: once by CodecClient::newStream() internally
    // (for its ActiveRequest) and once by OrcaOobSession::startStream().
    EXPECT_CALL(request_encoder_.stream_, addCallbacks(_)).Times(2);
  }

  // Serialize an OrcaLoadReport into a gRPC-framed buffer.
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
  OrcaOobStats stats_{ALL_ORCA_OOB_STATS(
      POOL_COUNTER_PREFIX(*stats_store_.rootScope(), "orca_oob."),
      POOL_GAUGE_PREFIX(*stats_store_.rootScope(), "orca_oob."))};
  std::unique_ptr<TestOrcaOobSession> session_;
  std::string empty_hostname_;

  // Test codec infrastructure (following health checker test pattern).
  // These are allocated in expectSessionStart() and owned by CodecClientForTest.
  NiceMock<Network::MockClientConnection>* client_connection_{};
  Http::MockClientConnection* codec_{};
  NiceMock<Http::MockRequestEncoder> request_encoder_;
  Http::ResponseDecoder* response_decoder_{};
  CodecClientForTest* codec_client_{};
};

// --- Lifecycle Tests ---

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

// --- Stream Start Tests ---

TEST_F(OrcaOobSessionTest, StartOpensStreamAndIncrementsStats) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_EQ(1, stats_.streams_started_.value());
  EXPECT_EQ(1, stats_.active_streams_.value());

  session_->stop();
}

// --- Report Decoding Tests ---

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

TEST_F(OrcaOobSessionTest, DecodeDataInvalidProtoIncrementsReportsFailed) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_, onOrcaOobReport(_)).Times(0);

  // Create a gRPC frame with garbage data.
  Buffer::OwnedImpl garbage;
  // gRPC frame: 1 byte compressed flag + 4 bytes length + data
  garbage.writeByte(0);                // not compressed
  garbage.writeBEInt<uint32_t>(4);     // length
  garbage.add("junk");                 // invalid protobuf
  response_decoder_->decodeData(garbage, false);

  EXPECT_EQ(0, stats_.reports_received_.value());
  EXPECT_EQ(1, stats_.reports_failed_.value());

  session_->stop();
}

// --- UNIMPLEMENTED Handling Tests ---

TEST_F(OrcaOobSessionTest, TrailersWithUnimplementedStopsRetries) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_,
              onOrcaOobStreamFailure(Grpc::Status::WellKnownGrpcStatus::Unimplemented));

  auto trailers = Http::ResponseTrailerMapImpl::create();
  trailers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Unimplemented));
  response_decoder_->decodeTrailers(std::move(trailers));

  EXPECT_EQ(1, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  // Subsequent start() should be a no-op (server_supports_oob_ is false).
  // No new createConnection should be called.
  EXPECT_CALL(*host_, createConnection_(_, _)).Times(0);
  session_->start();

  session_->stop();
}

TEST_F(OrcaOobSessionTest, HeadersEndStreamWithUnimplementedStopsRetries) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_CALL(callbacks_,
              onOrcaOobStreamFailure(Grpc::Status::WellKnownGrpcStatus::Unimplemented));

  // gRPC UNIMPLEMENTED can come as headers-only response (end_stream=true).
  auto headers = Http::ResponseHeaderMapImpl::create();
  headers->setStatus(200);
  headers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Unimplemented));
  response_decoder_->decodeHeaders(std::move(headers), true);

  EXPECT_EQ(1, stats_.backend_not_supported_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

// --- Stream Reset Tests ---

TEST_F(OrcaOobSessionTest, StreamResetSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  // Call the session's stream callback (back of the list; front is the
  // CodecClient's internal ActiveRequest callback).
  request_encoder_.stream_.callbacks_.back()->onResetStream(
      Http::StreamResetReason::RemoteReset, "");

  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

// --- Trailers with non-UNIMPLEMENTED status ---

TEST_F(OrcaOobSessionTest, TrailersWithOkSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  auto trailers = Http::ResponseTrailerMapImpl::create();
  trailers->setGrpcStatus(static_cast<uint64_t>(Grpc::Status::WellKnownGrpcStatus::Ok));
  response_decoder_->decodeTrailers(std::move(trailers));

  EXPECT_EQ(1, stats_.streams_closed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());
  EXPECT_EQ(0, stats_.backend_not_supported_.value());

  session_->stop();
}

// --- GOAWAY Tests ---

TEST_F(OrcaOobSessionTest, GoAwayDoesNotDoubleBackoff) {
  createSession();
  expectSessionStart();
  session_->start();

  EXPECT_EQ(1, stats_.active_streams_.value());
  EXPECT_EQ(0, stats_.streams_failed_.value());

  // Simulate GOAWAY: the HttpConnectionCallbackImpl delegates to onGoAway(),
  // which closes the connection. The close triggers onEvent(LocalClose).
  // Verify we only get one streams_failed_ increment (not two).
  codec_client_->raiseGoAway(Http::GoAwayErrorCode::NoError);

  // After GOAWAY, the stream should be cleaned up.
  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

// --- Non-200 Header Tests ---

TEST_F(OrcaOobSessionTest, Non200HeadersScheduleReconnect) {
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

// --- Connection Close Tests ---

TEST_F(OrcaOobSessionTest, RemoteCloseSchedulesReconnect) {
  createSession();
  expectSessionStart();
  session_->start();

  client_connection_->raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1, stats_.streams_failed_.value());
  EXPECT_EQ(0, stats_.active_streams_.value());

  session_->stop();
}

} // namespace
} // namespace Orca
} // namespace Envoy
