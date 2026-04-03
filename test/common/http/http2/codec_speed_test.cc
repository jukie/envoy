// Micro-benchmark comparing oghttp2 vs nghttp2 HTTP/2 codec performance.
// Measures round-trip, header encoding, header decoding, and data transfer
// to isolate the overhead introduced by oghttp2's header encoding path.
//
// Context: https://github.com/envoyproxy/envoy/issues/40070
//
// Usage:
//   bazel run -c opt //test/common/http/http2:codec_speed_test
//
// Codec selection: first arg is 0 (nghttp2) or 1 (oghttp2).

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/http2/codec_impl.h"

#include "test/common/http/common.h"
#include "test/common/http/http2/codec_impl_test_util.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"

using testing::_;
using testing::Invoke;
using testing::NiceMock;

namespace Envoy {
namespace Http {
namespace Http2 {
namespace {

namespace CommonUtility = ::Envoy::Http2::Utility;

TestRequestHeaderMapImpl makeRequestHeaders(int num_headers, int header_value_size) {
  TestRequestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  std::string value(header_value_size, 'a');
  for (int i = 0; i < num_headers; i++) {
    headers.addCopy(Http::LowerCaseString("x-benchmark-" + std::to_string(i)), value);
  }
  return headers;
}

TestResponseHeaderMapImpl makeResponseHeaders(int num_headers, int header_value_size) {
  TestResponseHeaderMapImpl headers{{":status", "200"}};
  std::string value(header_value_size, 'a');
  for (int i = 0; i < num_headers; i++) {
    headers.addCopy(Http::LowerCaseString("x-benchmark-" + std::to_string(i)), value);
  }
  return headers;
}

// Benchmark fixture adapted from Http2CodecImplTestFixture. Wires up client/server
// codec pairs with buffer shuttling, stripped of test assertions.
class CodecBenchmarkFixture {
public:
  struct ConnectionWrapper {
    explicit ConnectionWrapper(ConnectionImpl* connection) : connection_(connection) {}

    void driveDispatch() {
      while (canDispatch()) {
        status_ = connection_->dispatch(buffer_);
      }
    }

    bool canDispatch() const {
      return (buffer_.length() > 0 || connection_->wantsToWrite()) && status_.ok();
    }

    Buffer::OwnedImpl buffer_;
    ConnectionImpl* connection_{};
    Http::Status status_;
  };

  ~CodecBenchmarkFixture() {
    client_connection_.dispatcher_.clearDeferredDeleteList();
    server_connection_.dispatcher_.clearDeferredDeleteList();
  }

  void initialize(bool use_oghttp2) {
    scoped_runtime_.mergeValues(
        {{"envoy.reloadable_features.http2_use_oghttp2", use_oghttp2 ? "true" : "false"}});

    auto setDefaultOptions = [](envoy::config::core::v3::Http2ProtocolOptions& options) {
      options.mutable_hpack_table_size()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_HPACK_TABLE_SIZE);
      options.mutable_max_concurrent_streams()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_MAX_CONCURRENT_STREAMS);
      options.mutable_initial_stream_window_size()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_INITIAL_STREAM_WINDOW_SIZE);
      options.mutable_initial_connection_window_size()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_INITIAL_CONNECTION_WINDOW_SIZE);
      options.mutable_max_outbound_frames()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_MAX_OUTBOUND_FRAMES);
      options.mutable_max_outbound_control_frames()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_MAX_OUTBOUND_CONTROL_FRAMES);
      options.mutable_max_consecutive_inbound_frames_with_empty_payload()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_MAX_CONSECUTIVE_INBOUND_FRAMES_WITH_EMPTY_PAYLOAD);
      options.mutable_max_inbound_priority_frames_per_stream()->set_value(
          CommonUtility::OptionsLimits::DEFAULT_MAX_INBOUND_PRIORITY_FRAMES_PER_STREAM);
      options.mutable_max_inbound_window_update_frames_per_data_frame_sent()->set_value(
          CommonUtility::OptionsLimits::
              DEFAULT_MAX_INBOUND_WINDOW_UPDATE_FRAMES_PER_DATA_FRAME_SENT);
    };

    envoy::config::core::v3::Http2ProtocolOptions client_options;
    setDefaultOptions(client_options);

    envoy::config::core::v3::Http2ProtocolOptions server_options;
    setDefaultOptions(server_options);

    ON_CALL(client_connection_, write(_, _))
        .WillByDefault(
            Invoke([this](Buffer::Instance& data, bool) { server_wrapper_->buffer_.add(data); }));
    ON_CALL(server_connection_, write(_, _))
        .WillByDefault(
            Invoke([this](Buffer::Instance& data, bool) { client_wrapper_->buffer_.add(data); }));
    ON_CALL(server_connection_, bufferLimit()).WillByDefault(testing::Return(16 * 1024));

    client_ = std::make_unique<TestClientConnectionImpl>(
        client_connection_, client_callbacks_, *client_stats_store_.rootScope(), client_options,
        random_, Http::DEFAULT_MAX_REQUEST_HEADERS_KB, Http::DEFAULT_MAX_HEADERS_COUNT,
        ProdNghttp2SessionFactory::get());
    client_wrapper_ = std::make_unique<ConnectionWrapper>(client_.get());

    server_ = std::make_unique<TestServerConnectionImpl>(
        server_connection_, server_callbacks_, *server_stats_store_.rootScope(), server_options,
        random_, Http::DEFAULT_MAX_REQUEST_HEADERS_KB, Http::DEFAULT_MAX_HEADERS_COUNT,
        envoy::config::core::v3::HttpProtocolOptions::ALLOW);
    server_wrapper_ = std::make_unique<ConnectionWrapper>(server_.get());

    ON_CALL(request_decoder_, getRequestDecoderHandle()).WillByDefault(Invoke([this]() {
      auto handle = std::make_unique<NiceMock<MockRequestDecoderHandle>>();
      ON_CALL(*handle, get())
          .WillByDefault(testing::Return(OptRef<RequestDecoder>(request_decoder_)));
      return handle;
    }));

    driveToCompletion();

    ON_CALL(server_callbacks_, newStream(_, _))
        .WillByDefault(Invoke([this](ResponseEncoder& encoder, bool) -> RequestDecoder& {
          response_encoder_ = &encoder;
          encoder.getStream().addCallbacks(server_stream_callbacks_);
          return request_decoder_;
        }));
  }

  void driveToCompletion() {
    while (client_wrapper_->canDispatch() || server_wrapper_->canDispatch()) {
      client_wrapper_->driveDispatch();
      server_wrapper_->driveDispatch();
    }
  }

  TestScopedRuntime scoped_runtime_;
  Stats::TestUtil::TestStore client_stats_store_;
  Stats::TestUtil::TestStore server_stats_store_;
  NiceMock<Network::MockConnection> client_connection_;
  NiceMock<Network::MockConnection> server_connection_;
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<MockConnectionCallbacks> client_callbacks_;
  NiceMock<MockServerConnectionCallbacks> server_callbacks_;
  NiceMock<MockResponseDecoder> response_decoder_;
  NiceMock<MockRequestDecoder> request_decoder_;
  NiceMock<MockStreamCallbacks> server_stream_callbacks_;
  ResponseEncoder* response_encoder_{};
  std::unique_ptr<TestClientConnectionImpl> client_;
  std::unique_ptr<TestServerConnectionImpl> server_;
  std::unique_ptr<ConnectionWrapper> client_wrapper_;
  std::unique_ptr<ConnectionWrapper> server_wrapper_;
};

// Full request-response cycle.
// Args: codec (0=nghttp2, 1=oghttp2), num_headers, header_value_size, body_size
void benchmarkRoundTrip(::benchmark::State& state) {
  const bool use_oghttp2 = state.range(0) != 0;
  const int num_headers = state.range(1);
  const int header_value_size = state.range(2);
  const int body_size = state.range(3);

  CodecBenchmarkFixture fixture;
  fixture.initialize(use_oghttp2);

  for (auto _ : state) { // NOLINT
    state.PauseTiming();
    auto request_headers = makeRequestHeaders(num_headers, header_value_size);
    auto response_headers = makeResponseHeaders(num_headers, header_value_size);
    const bool has_body = body_size > 0;
    RequestEncoder* request_encoder = &fixture.client_->newStream(fixture.response_decoder_);
    state.ResumeTiming();

    request_encoder->encodeHeaders(request_headers, !has_body).IgnoreError();
    if (has_body) {
      Buffer::OwnedImpl request_body(std::string(body_size, 'b'));
      request_encoder->encodeData(request_body, true);
    }
    fixture.driveToCompletion();

    fixture.response_encoder_->encodeHeaders(response_headers, !has_body);
    if (has_body) {
      Buffer::OwnedImpl response_body(std::string(body_size, 'b'));
      fixture.response_encoder_->encodeData(response_body, true);
    }
    fixture.driveToCompletion();
  }
}
BENCHMARK(benchmarkRoundTrip)
    ->Args({0, 5, 32, 0})     // nghttp2, minimal
    ->Args({1, 5, 32, 0})     // oghttp2, minimal
    ->Args({0, 15, 64, 1024}) // nghttp2, typical
    ->Args({1, 15, 64, 1024}) // oghttp2, typical
    ->Args({0, 50, 128, 0})   // nghttp2, heavy headers
    ->Args({1, 50, 128, 0})   // oghttp2, heavy headers
    ->Unit(::benchmark::kMicrosecond);

// Header encoding only (client-side HPACK compression).
// Args: codec (0=nghttp2, 1=oghttp2), num_headers, header_value_size
void benchmarkEncodeHeaders(::benchmark::State& state) {
  const bool use_oghttp2 = state.range(0) != 0;
  const int num_headers = state.range(1);
  const int header_value_size = state.range(2);

  CodecBenchmarkFixture fixture;
  fixture.initialize(use_oghttp2);

  for (auto _ : state) { // NOLINT
    state.PauseTiming();
    auto request_headers = makeRequestHeaders(num_headers, header_value_size);
    RequestEncoder* request_encoder = &fixture.client_->newStream(fixture.response_decoder_);
    state.ResumeTiming();

    request_encoder->encodeHeaders(request_headers, true).IgnoreError();
    fixture.driveToCompletion();

    state.PauseTiming();
    TestResponseHeaderMapImpl response_headers{{":status", "200"}};
    fixture.response_encoder_->encodeHeaders(response_headers, true);
    fixture.driveToCompletion();
    state.ResumeTiming();
  }
}
BENCHMARK(benchmarkEncodeHeaders)
    ->Args({0, 5, 32})
    ->Args({1, 5, 32})
    ->Args({0, 15, 64})
    ->Args({1, 15, 64})
    ->Args({0, 50, 128})
    ->Args({1, 50, 128})
    ->Unit(::benchmark::kMicrosecond);

// Header decoding only (server-side HPACK decompression).
// Args: codec (0=nghttp2, 1=oghttp2), num_headers, header_value_size
void benchmarkDecodeHeaders(::benchmark::State& state) {
  const bool use_oghttp2 = state.range(0) != 0;
  const int num_headers = state.range(1);
  const int header_value_size = state.range(2);

  CodecBenchmarkFixture fixture;
  fixture.initialize(use_oghttp2);

  for (auto _ : state) { // NOLINT
    state.PauseTiming();
    auto request_headers = makeRequestHeaders(num_headers, header_value_size);
    RequestEncoder* request_encoder = &fixture.client_->newStream(fixture.response_decoder_);

    request_encoder->encodeHeaders(request_headers, true).IgnoreError();
    fixture.client_wrapper_->driveDispatch();
    state.ResumeTiming();

    fixture.server_wrapper_->driveDispatch();

    state.PauseTiming();
    TestResponseHeaderMapImpl response_headers{{":status", "200"}};
    fixture.response_encoder_->encodeHeaders(response_headers, true);
    fixture.driveToCompletion();
    state.ResumeTiming();
  }
}
BENCHMARK(benchmarkDecodeHeaders)
    ->Args({0, 5, 32})
    ->Args({1, 5, 32})
    ->Args({0, 15, 64})
    ->Args({1, 15, 64})
    ->Args({0, 50, 128})
    ->Args({1, 50, 128})
    ->Unit(::benchmark::kMicrosecond);

// Data frame transfer with a pre-established stream.
// Args: codec (0=nghttp2, 1=oghttp2), body_size
void benchmarkDataTransfer(::benchmark::State& state) {
  const bool use_oghttp2 = state.range(0) != 0;
  const int body_size = state.range(1);

  CodecBenchmarkFixture fixture;
  fixture.initialize(use_oghttp2);

  for (auto _ : state) { // NOLINT
    state.PauseTiming();
    RequestEncoder* request_encoder = &fixture.client_->newStream(fixture.response_decoder_);
    TestRequestHeaderMapImpl request_headers;
    HttpTestUtility::addDefaultHeaders(request_headers);
    request_headers.setMethod("POST");
    request_encoder->encodeHeaders(request_headers, false).IgnoreError();
    fixture.driveToCompletion();

    TestResponseHeaderMapImpl response_headers{{":status", "200"}};
    fixture.response_encoder_->encodeHeaders(response_headers, false);
    fixture.driveToCompletion();
    state.ResumeTiming();

    Buffer::OwnedImpl request_body(std::string(body_size, 'b'));
    request_encoder->encodeData(request_body, true);
    fixture.driveToCompletion();

    Buffer::OwnedImpl response_body(std::string(body_size, 'b'));
    fixture.response_encoder_->encodeData(response_body, true);
    fixture.driveToCompletion();
  }
}
BENCHMARK(benchmarkDataTransfer)
    ->Args({0, 1024})
    ->Args({1, 1024})
    ->Args({0, 65536})
    ->Args({1, 65536})
    ->Unit(::benchmark::kMicrosecond);

} // namespace
} // namespace Http2
} // namespace Http
} // namespace Envoy
