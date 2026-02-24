#include <cstdint>
#include <memory>

#include "envoy/extensions/load_balancing_policies/round_robin/v3/round_robin.pb.h"

#include "source/extensions/load_balancing_policies/wrr_locality/wrr_locality_lb.h"

#include "test/extensions/load_balancing_policies/common/load_balancer_impl_base_test.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/thread_local/mocks.h"

#include "gmock/gmock.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {
namespace {

using ::Envoy::Upstream::HostVector;
using ::Envoy::Upstream::LoadBalancerTestBase;
using ::Envoy::Upstream::makeTestHost;
using ::testing::_;

class WrrLocalityLoadBalancerTest : public LoadBalancerTestBase {
public:
  void init() {
    // MockTimer auto-captures the createTimer_ callback from OrcaWeightManager.
    timer_ = new Event::MockTimer(&mock_dispatcher_);
    // The timer's enableTimer is called by initialize() and again after each invokeCallback().
    EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(1000), _))
        .Times(testing::AnyNumber());

    // Build a RoundRobinConfig with locality_weighted_lb_config set.
    RoundRobinConfig rr_config;
    rr_config.mutable_locality_lb_config();

    // Create the WrrLocalityLbConfig with default ORCA parameters.
    lb_config_ = std::make_unique<WrrLocalityLbConfig>(
        /*metric_names_for_computing_utilization=*/std::vector<std::string>{},
        /*error_utilization_penalty=*/0.0,
        /*blackout_period=*/std::chrono::milliseconds(10000),
        /*weight_expiration_period=*/std::chrono::milliseconds(180000),
        /*weight_update_period=*/std::chrono::milliseconds(1000), std::move(rr_config),
        mock_dispatcher_, mock_tls_);

    // Create the thread-aware load balancer.
    thread_aware_lb_ = std::make_shared<WrrLocalityLoadBalancer>(*lb_config_, *info_, priority_set_,
                                                                 runtime_, random_, simTime());
    ASSERT_EQ(thread_aware_lb_->initialize(), absl::OkStatus());

    // Create worker-local LB through the factory.
    auto factory = thread_aware_lb_->factory();
    worker_lb_ = factory->create({priority_set_, nullptr});
  }

  // Inject an ORCA load report into a host.
  void injectOrcaReport(const Upstream::HostSharedPtr& host, double rps, double utilization) {
    xds::data::orca::v3::OrcaLoadReport report;
    report.set_rps_fractional(rps);
    report.set_application_utilization(utilization);
    Envoy::StreamInfo::MockStreamInfo mock_stream_info;
    ASSERT_TRUE(host->lbPolicyData().has_value());
    EXPECT_EQ(host->lbPolicyData()->onOrcaLoadReport(report, mock_stream_info), absl::OkStatus());
  }

  NiceMock<Event::MockDispatcher> mock_dispatcher_;
  NiceMock<Envoy::ThreadLocal::MockInstance> mock_tls_;
  Event::MockTimer* timer_{};
  std::unique_ptr<WrrLocalityLbConfig> lb_config_;
  std::shared_ptr<Upstream::ThreadAwareLoadBalancer> thread_aware_lb_;
  Upstream::LoadBalancerPtr worker_lb_;
};

// Proves the full wiring: ORCA reports flow through OrcaWeightManager to update host weights.
TEST_P(WrrLocalityLoadBalancerTest, OrcaReportsUpdateHostWeights) {
  hostSet().healthy_hosts_ = {
      makeTestHost(info_, "tcp://127.0.0.1:80"),
      makeTestHost(info_, "tcp://127.0.0.1:81"),
      makeTestHost(info_, "tcp://127.0.0.1:82"),
  };
  hostSet().hosts_ = hostSet().healthy_hosts_;
  init();
  hostSet().runCallbacks({}, {});

  // Inject ORCA reports at time 5s.
  simTime().advanceTimeWait(std::chrono::seconds(5));
  for (const auto& host : hostSet().hosts_) {
    injectOrcaReport(host, 1000, 0.5);
  }

  // Advance past blackout: non_empty_since=5s, now=20s, max_non_empty_since=20-10=10s > 5s.
  simTime().advanceTimeWait(std::chrono::seconds(15));

  // Fire the weight-update timer.
  timer_->invokeCallback();

  // weight = rps / utilization = 1000 / 0.5 = 2000
  for (const auto& host : hostSet().hosts_) {
    EXPECT_EQ(host->weight(), 2000);
  }
}

// Verifies that different utilizations produce correct weight ratios.
TEST_P(WrrLocalityLoadBalancerTest, DifferentUtilizationsProduceCorrectWeights) {
  hostSet().healthy_hosts_ = {
      makeTestHost(info_, "tcp://127.0.0.1:80"),
      makeTestHost(info_, "tcp://127.0.0.1:81"),
      makeTestHost(info_, "tcp://127.0.0.1:82"),
  };
  hostSet().hosts_ = hostSet().healthy_hosts_;
  init();
  hostSet().runCallbacks({}, {});

  // Inject reports at time 5s.
  simTime().advanceTimeWait(std::chrono::seconds(5));
  injectOrcaReport(hostSet().hosts_[0], 1000, 0.5);
  injectOrcaReport(hostSet().hosts_[1], 1000, 0.25);
  injectOrcaReport(hostSet().hosts_[2], 1000, 1.0);

  // Advance past blackout.
  simTime().advanceTimeWait(std::chrono::seconds(15));
  timer_->invokeCallback();

  EXPECT_EQ(hostSet().hosts_[0]->weight(), 2000); // 1000 / 0.5
  EXPECT_EQ(hostSet().hosts_[1]->weight(), 4000); // 1000 / 0.25
  EXPECT_EQ(hostSet().hosts_[2]->weight(), 1000); // 1000 / 1.0
}

// Verifies that hosts without ORCA reports get the median weight.
TEST_P(WrrLocalityLoadBalancerTest, HostsWithoutOrcaReportsGetMedianWeight) {
  hostSet().healthy_hosts_ = {
      makeTestHost(info_, "tcp://127.0.0.1:80"),
      makeTestHost(info_, "tcp://127.0.0.1:81"),
      makeTestHost(info_, "tcp://127.0.0.1:82"),
  };
  hostSet().hosts_ = hostSet().healthy_hosts_;
  init();
  hostSet().runCallbacks({}, {});

  // Inject reports on only the first two hosts at time 5s.
  simTime().advanceTimeWait(std::chrono::seconds(5));
  injectOrcaReport(hostSet().hosts_[0], 1000, 0.5);  // weight = 2000
  injectOrcaReport(hostSet().hosts_[1], 1000, 0.25); // weight = 4000

  // Advance past blackout.
  simTime().advanceTimeWait(std::chrono::seconds(15));
  timer_->invokeCallback();

  EXPECT_EQ(hostSet().hosts_[0]->weight(), 2000);
  EXPECT_EQ(hostSet().hosts_[1]->weight(), 4000);
  // Median of {2000, 4000} with even count = (2000 + 4000) / 2 = 3000
  EXPECT_EQ(hostSet().hosts_[2]->weight(), 3000);
}

// Proves that updated weights affect host picking distribution through the worker LB.
TEST_P(WrrLocalityLoadBalancerTest, WeightUpdateAffectsHostPicking) {
  hostSet().healthy_hosts_ = {
      makeTestHost(info_, "tcp://127.0.0.1:80"),
      makeTestHost(info_, "tcp://127.0.0.1:81"),
  };
  hostSet().hosts_ = hostSet().healthy_hosts_;
  init();
  hostSet().runCallbacks({}, {});

  // Inject reports at time 5s.
  simTime().advanceTimeWait(std::chrono::seconds(5));
  injectOrcaReport(hostSet().hosts_[0], 1000, 0.25); // weight=4000
  injectOrcaReport(hostSet().hosts_[1], 1000, 1.0);  // weight=1000

  // Advance past blackout.
  simTime().advanceTimeWait(std::chrono::seconds(15));
  timer_->invokeCallback();

  EXPECT_EQ(hostSet().hosts_[0]->weight(), 4000);
  EXPECT_EQ(hostSet().hosts_[1]->weight(), 1000);

  // Trigger the worker LB to rebuild its EDF scheduler with the new weights.
  hostSet().runCallbacks({}, {});

  // Call chooseHost many times and verify the distribution is roughly 4:1.
  const int total_picks = 5000;
  int host0_count = 0;
  int host1_count = 0;
  for (int i = 0; i < total_picks; ++i) {
    auto selected = worker_lb_->chooseHost(nullptr).host;
    if (selected == hostSet().hosts_[0]) {
      ++host0_count;
    } else if (selected == hostSet().hosts_[1]) {
      ++host1_count;
    }
  }

  // Expected ratio is 4:1 (host0:host1). Allow tolerance.
  double ratio = static_cast<double>(host0_count) / host1_count;
  EXPECT_NEAR(ratio, 4.0, 0.5);
  EXPECT_EQ(host0_count + host1_count, total_picks);
}

INSTANTIATE_TEST_SUITE_P(PrimarySuite, WrrLocalityLoadBalancerTest,
                         testing::Values(Upstream::LoadBalancerTestParam{true},
                                         Upstream::LoadBalancerTestParam{false}));

} // namespace
} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
