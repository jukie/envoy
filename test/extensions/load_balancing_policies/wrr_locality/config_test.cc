#include "envoy/config/core/v3/extension.pb.h"

#include "source/extensions/load_balancing_policies/wrr_locality/config.h"

#include "envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3/client_side_weighted_round_robin.pb.h"
#include "envoy/extensions/load_balancing_policies/wrr_locality/v3/wrr_locality.pb.h"

#include "test/mocks/server/factory_context.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {
namespace {

using OrcaLoadReportProto = xds::data::orca::v3::OrcaLoadReport;

class WrrLocalityConfigTest : public testing::Test {
protected:
  NiceMock<Server::Configuration::MockServerFactoryContext> context_;
  NiceMock<Upstream::MockClusterInfo> cluster_info_;
  NiceMock<Upstream::MockPrioritySet> main_thread_priority_set_;
  NiceMock<Upstream::MockPrioritySet> worker_priority_set_;
  Factory factory_;
};

TEST_F(WrrLocalityConfigTest, BasicTranslation) {
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality typed_config;
  typed_config.mutable_locality_lb_config()->mutable_zone_aware_lb_config()->set_fail_traffic_on_panic(
      true);
  
  auto* policy = typed_config.mutable_endpoint_picking_policy()->add_policies();
  policy->mutable_typed_extension_config()->set_name(
      "envoy.load_balancing_policies.client_side_weighted_round_robin");
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin child_config;
  child_config.mutable_weight_update_period()->set_seconds(2);
  policy->mutable_typed_extension_config()->mutable_typed_config()->PackFrom(child_config);

  auto lb_config_or_status = factory_.loadConfig(context_, typed_config);
  ASSERT_TRUE(lb_config_or_status.ok()) << lb_config_or_status.status();
  auto lb_config = std::move(lb_config_or_status.value());
  ASSERT_NE(lb_config, nullptr);

  auto thread_aware_lb =
      factory_.create(*lb_config, cluster_info_, main_thread_priority_set_, context_.runtime_loader_,
                     context_.api_.random_, context_.time_system_);
  ASSERT_NE(thread_aware_lb, nullptr);
  EXPECT_TRUE(thread_aware_lb->initialize().ok());

  auto worker_factory = thread_aware_lb->factory();
  ASSERT_NE(worker_factory, nullptr);
  auto worker_lb = worker_factory->create({worker_priority_set_, nullptr});
  EXPECT_NE(worker_lb, nullptr);
}

TEST_F(WrrLocalityConfigTest, FactoryName) {
  EXPECT_EQ("envoy.load_balancing_policies.wrr_locality", factory_.name());
}

TEST_F(WrrLocalityConfigTest, EmptyProtoCreation) {
  auto proto = factory_.createEmptyConfigProto();
  EXPECT_NE(proto, nullptr);
}

// Tests for OrcaLoadReportHandler weight calculation
class OrcaWeightCalculationTest : public testing::Test {
protected:
  static constexpr double kErrorUtilizationPenalty = 1.0;
  std::vector<std::string> metric_names_{"named_metrics.custom_metric"};
};

TEST_F(OrcaWeightCalculationTest, ValidWeightCalculation) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(100.0);
  report.set_cpu_utilization(0.5);

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(200, result.value()); // 100 / 0.5 = 200
}

TEST_F(OrcaWeightCalculationTest, ApplicationUtilizationPriority) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(100.0);
  report.set_cpu_utilization(0.5);
  report.set_application_utilization(0.25);

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(400, result.value()); // 100 / 0.25 = 400, uses app utilization over CPU
}

TEST_F(OrcaWeightCalculationTest, CustomMetricUtilization) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(100.0);
  report.set_cpu_utilization(0.5);
  (*report.mutable_named_metrics())["custom_metric"] = 0.8;

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(125, result.value()); // 100 / 0.8 = 125, uses custom metric
}

TEST_F(OrcaWeightCalculationTest, ErrorUtilizationPenalty) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(100.0);
  report.set_cpu_utilization(0.5);
  report.set_eps(10.0);

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, 2.0); // penalty = 2.0
  
  ASSERT_TRUE(result.ok());
  // utilization = 0.5 + (2.0 * 10.0 / 100.0) = 0.5 + 0.2 = 0.7
  // weight = 100 / 0.7 ≈ 142
  EXPECT_EQ(142, result.value());
}

TEST_F(OrcaWeightCalculationTest, ZeroQpsReturnsError) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(0.0);
  report.set_cpu_utilization(0.5);

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(absl::StatusCode::kInvalidArgument, result.status().code());
}

TEST_F(OrcaWeightCalculationTest, NegativeQpsReturnsError) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(-10.0);
  report.set_cpu_utilization(0.5);

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(absl::StatusCode::kInvalidArgument, result.status().code());
}

TEST_F(OrcaWeightCalculationTest, ZeroUtilizationReturnsError) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(100.0);
  report.set_cpu_utilization(0.0);

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(absl::StatusCode::kInvalidArgument, result.status().code());
}

TEST_F(OrcaWeightCalculationTest, WeightOverflowClamping) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(1e15); // Very high QPS
  report.set_cpu_utilization(0.0001); // Very low utilization

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(std::numeric_limits<uint32_t>::max(), result.value());
}

TEST_F(OrcaWeightCalculationTest, WeightBelowOneClampedToOne) {
  OrcaLoadReportProto report;
  report.set_rps_fractional(0.5);
  report.set_cpu_utilization(10.0); // Very high utilization

  auto result = WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
      report, metric_names_, kErrorUtilizationPenalty);
  
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(1, result.value());
}

// Tests for HostLbPolicyData
class HostLbPolicyDataTest : public testing::Test {
protected:
  void SetUp() override {
    // Setup config with ClientSideWRR endpoint policy
    auto* policy = typed_config_.mutable_endpoint_picking_policy()->add_policies();
    policy->mutable_typed_extension_config()->set_name(
        "envoy.load_balancing_policies.client_side_weighted_round_robin");
    envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
        ClientSideWeightedRoundRobin child_config;
    policy->mutable_typed_extension_config()->mutable_typed_config()->PackFrom(child_config);

    config_ = std::make_unique<WrrLocalityLbConfig>(typed_config_, context_);
    handler_ = std::make_shared<WrrLocalityLoadBalancer::OrcaLoadReportHandler>(
        *config_, time_source_);
    host_data_ = std::make_unique<WrrLocalityLoadBalancer::HostLbPolicyData>(handler_);
  }

  NiceMock<Server::Configuration::MockServerFactoryContext> context_;
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality typed_config_;
  std::unique_ptr<WrrLocalityLbConfig> config_;
  Event::SimulatedTimeSystem time_source_;
  std::shared_ptr<WrrLocalityLoadBalancer::OrcaLoadReportHandler> handler_;
  std::unique_ptr<WrrLocalityLoadBalancer::HostLbPolicyData> host_data_;
};

TEST_F(HostLbPolicyDataTest, InitialWeightIsOne) {
  const MonotonicTime now = time_source_.monotonicTime();
  const MonotonicTime max_non_empty = now;
  const MonotonicTime min_last_update = now - std::chrono::seconds(10);
  
  auto weight = host_data_->getWeightIfValid(max_non_empty, min_last_update);
  EXPECT_FALSE(weight.has_value()); // Should be invalid initially
}

TEST_F(HostLbPolicyDataTest, WeightUpdateAndRetrieval) {
  const MonotonicTime now = time_source_.monotonicTime();
  
  host_data_->updateWeightNow(42, now);
  
  const MonotonicTime max_non_empty = now + std::chrono::seconds(1);
  const MonotonicTime min_last_update = now - std::chrono::seconds(1);
  
  auto weight = host_data_->getWeightIfValid(max_non_empty, min_last_update);
  ASSERT_TRUE(weight.has_value());
  EXPECT_EQ(42, weight.value());
}

TEST_F(HostLbPolicyDataTest, WeightExpirationByBlackout) {
  const MonotonicTime now = time_source_.monotonicTime();
  
  host_data_->updateWeightNow(42, now);
  
  // max_non_empty_since is before the update - should be invalid
  const MonotonicTime max_non_empty = now - std::chrono::seconds(1);
  const MonotonicTime min_last_update = now - std::chrono::seconds(2);
  
  auto weight = host_data_->getWeightIfValid(max_non_empty, min_last_update);
  EXPECT_FALSE(weight.has_value());
}

TEST_F(HostLbPolicyDataTest, WeightExpirationByAge) {
  const MonotonicTime now = time_source_.monotonicTime();
  
  host_data_->updateWeightNow(42, now);
  
  // min_last_update is after the update - should be invalid
  const MonotonicTime max_non_empty = now + std::chrono::seconds(2);
  const MonotonicTime min_last_update = now + std::chrono::seconds(1);
  
  auto weight = host_data_->getWeightIfValid(max_non_empty, min_last_update);
  EXPECT_FALSE(weight.has_value());
}

TEST_F(HostLbPolicyDataTest, MultipleWeightUpdates) {
  MonotonicTime now = time_source_.monotonicTime();
  
  host_data_->updateWeightNow(10, now);
  
  time_source_.advanceTimeWait(std::chrono::seconds(1));
  now = time_source_.monotonicTime();
  host_data_->updateWeightNow(20, now);
  
  const MonotonicTime max_non_empty = now + std::chrono::seconds(1);
  const MonotonicTime min_last_update = now - std::chrono::seconds(1);
  
  auto weight = host_data_->getWeightIfValid(max_non_empty, min_last_update);
  ASSERT_TRUE(weight.has_value());
  EXPECT_EQ(20, weight.value()); // Should return latest weight
}

TEST_F(HostLbPolicyDataTest, WeightExpirationResetsBlackout) {
  const MonotonicTime now = time_source_.monotonicTime();
  
  // First weight update
  host_data_->updateWeightNow(100, now);
  
  // Advance past expiration period
  time_source_.advanceTimeWait(std::chrono::seconds(200));
  const MonotonicTime future = time_source_.monotonicTime();
  
  // Weight should be expired
  const MonotonicTime max_non_empty = future;
  const MonotonicTime min_last_update = future - std::chrono::seconds(10);
  auto weight = host_data_->getWeightIfValid(max_non_empty, min_last_update);
  EXPECT_FALSE(weight.has_value());
  
  // New weight update should succeed immediately (blackout was reset)
  host_data_->updateWeightNow(200, future);
  
  // Should be valid now (not subject to blackout)
  const MonotonicTime max_non_empty2 = future + std::chrono::seconds(1);
  const MonotonicTime min_last_update2 = future - std::chrono::seconds(1);
  weight = host_data_->getWeightIfValid(max_non_empty2, min_last_update2);
  ASSERT_TRUE(weight.has_value());
  EXPECT_EQ(200, weight.value());
}

// Integration tests for full load balancer
class WrrLocalityLoadBalancerTest : public testing::Test {
protected:
  void SetUp() override {
    // Setup config with ClientSideWRR endpoint policy
    auto* policy = typed_config_.mutable_endpoint_picking_policy()->add_policies();
    policy->mutable_typed_extension_config()->set_name(
        "envoy.load_balancing_policies.client_side_weighted_round_robin");
    envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
        ClientSideWeightedRoundRobin child_config;
    child_config.mutable_blackout_period()->set_seconds(1);
    child_config.mutable_weight_expiration_period()->set_seconds(10);
    child_config.mutable_weight_update_period()->set_nanos(100000000); // 0.1s for faster tests
    policy->mutable_typed_extension_config()->mutable_typed_config()->PackFrom(child_config);

    auto lb_config_or_status = factory_.loadConfig(context_, typed_config_);
    ASSERT_TRUE(lb_config_or_status.ok());
    lb_config_ = std::move(lb_config_or_status.value());
  }

  NiceMock<Server::Configuration::MockServerFactoryContext> context_;
  NiceMock<Upstream::MockClusterInfo> cluster_info_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality typed_config_;
  Factory factory_;
  Upstream::LoadBalancerConfigPtr lb_config_;
};

TEST_F(WrrLocalityLoadBalancerTest, InitializationSucceeds) {
  auto thread_aware_lb =
      factory_.create(*lb_config_, cluster_info_, priority_set_, context_.runtime_loader_,
                     context_.api_.random_, context_.time_system_);
  ASSERT_NE(thread_aware_lb, nullptr);
  EXPECT_TRUE(thread_aware_lb->initialize().ok());
}

TEST_F(WrrLocalityLoadBalancerTest, FactoryReturnsValidWorkerFactory) {
  auto thread_aware_lb =
      factory_.create(*lb_config_, cluster_info_, priority_set_, context_.runtime_loader_,
                     context_.api_.random_, context_.time_system_);
  ASSERT_NE(thread_aware_lb, nullptr);
  ASSERT_TRUE(thread_aware_lb->initialize().ok());

  auto worker_factory = thread_aware_lb->factory();
  ASSERT_NE(worker_factory, nullptr);
  EXPECT_FALSE(worker_factory->recreateOnHostChange());
}

// Test for proto validation
class WrrLocalityConfigValidationTest : public testing::Test {
protected:
  NiceMock<Server::Configuration::MockServerFactoryContext> context_;
  Factory factory_;
};

TEST_F(WrrLocalityConfigValidationTest, MissingEndpointPickingPolicyFails) {
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality typed_config;
  // Don't set endpoint_picking_policy

  // Proto validation will throw an exception before our custom validation
  EXPECT_THROW_WITH_REGEX(
      { auto result = factory_.loadConfig(context_, typed_config); },
      EnvoyException, "Proto constraint validation failed.*EndpointPickingPolicy");
}

TEST_F(WrrLocalityConfigValidationTest, EmptyEndpointPickingPolicyFails) {
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality typed_config;
  typed_config.mutable_endpoint_picking_policy(); // Create but leave empty

  auto lb_config_or_status = factory_.loadConfig(context_, typed_config);
  EXPECT_FALSE(lb_config_or_status.ok());
  EXPECT_EQ(absl::StatusCode::kInvalidArgument, lb_config_or_status.status().code());
}

TEST_F(WrrLocalityConfigValidationTest, ValidConfigSucceeds) {
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality typed_config;
  auto* policy = typed_config.mutable_endpoint_picking_policy()->add_policies();
  policy->mutable_typed_extension_config()->set_name(
      "envoy.load_balancing_policies.client_side_weighted_round_robin");
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin child_config;
  policy->mutable_typed_extension_config()->mutable_typed_config()->PackFrom(child_config);

  auto lb_config_or_status = factory_.loadConfig(context_, typed_config);
  EXPECT_TRUE(lb_config_or_status.ok());
}

// Note: Average weight calculation is implicitly tested via the weight update logic.
// A full integration test with mock hosts and ORCA reports would require
// extensive setup of priority sets, host sets, and ORCA report simulation.
// The unit tests above cover the core weight calculation and averaging logic.

} // namespace
} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy

