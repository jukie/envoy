#pragma once

// WrrLocality (Weighted Round Robin Locality) load balancing policy.
// This policy performs locality-weighted load balancing with dynamic endpoint weights
// based on ORCA (Open Request Cost Aggregation) load reports from backends.
// It combines round-robin locality selection with weighted endpoint selection within
// each locality, where weights are computed from backend utilization metrics.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3/client_side_weighted_round_robin.pb.h"
#include "envoy/extensions/load_balancing_policies/common/v3/common.pb.h"
#include "envoy/extensions/load_balancing_policies/wrr_locality/v3/wrr_locality.pb.h"
#include "envoy/server/factory_context.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/upstream.h"
#include "envoy/runtime/runtime.h"
#include "envoy/common/time.h"
#include "envoy/common/random_generator.h"

#include "source/common/common/callback_impl.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/load_balancing_policies/round_robin/round_robin_lb.h"

#include "absl/status/status.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {

using OrcaLoadReportProto = xds::data::orca::v3::OrcaLoadReport;

// Default configuration values for weight update timing.
constexpr std::chrono::milliseconds kDefaultBlackoutPeriod{10000};
constexpr std::chrono::milliseconds kDefaultWeightExpirationPeriod{180000};
constexpr std::chrono::milliseconds kDefaultWeightUpdatePeriod{1000};
constexpr double kDefaultErrorUtilizationPenalty = 1.0;
constexpr uint32_t kMinimumHostWeight = 1;

/**
 * Load balancer config used to wrap the WRR locality proto.
 */
class WrrLocalityLbConfig : public Upstream::LoadBalancerConfig {
public:
  WrrLocalityLbConfig(
      const envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality& lb_config,
      Server::Configuration::ServerFactoryContext& context);

  const envoy::extensions::load_balancing_policies::round_robin::v3::RoundRobin&
  roundRobinConfig() const {
    return round_robin_config_;
  }

  const std::vector<std::string>& metricNamesForComputingUtilization() const {
    return metric_names_for_computing_utilization_;
  }
  double errorUtilizationPenalty() const { return error_utilization_penalty_; }

  std::chrono::milliseconds blackoutPeriod() const { return blackout_period_; }
  std::chrono::milliseconds weightExpirationPeriod() const { return weight_expiration_period_; }
  std::chrono::milliseconds weightUpdatePeriod() const { return weight_update_period_; }

  Event::Dispatcher& mainThreadDispatcher() const { return main_thread_dispatcher_; }
  ThreadLocal::SlotAllocator& tlsSlotAllocator() const { return tls_slot_allocator_; }

private:
  // Member variables ordered to match initialization order in constructor
  Event::Dispatcher& main_thread_dispatcher_;
  ThreadLocal::SlotAllocator& tls_slot_allocator_;

  envoy::extensions::load_balancing_policies::round_robin::v3::RoundRobin round_robin_config_;

  std::vector<std::string> metric_names_for_computing_utilization_;
  double error_utilization_penalty_{kDefaultErrorUtilizationPenalty};

  std::chrono::milliseconds blackout_period_{kDefaultBlackoutPeriod};
  std::chrono::milliseconds weight_expiration_period_{kDefaultWeightExpirationPeriod};
  std::chrono::milliseconds weight_update_period_{kDefaultWeightUpdatePeriod};
};

/**
 * WRR locality load balancer. Locality selection is driven by RoundRobinLoadBalancer, while
 * endpoint weights are updated from ORCA load reports similar to
 * ClientSideWeightedRoundRobin load balancer.
 */
class WrrLocalityLoadBalancer : public Upstream::ThreadAwareLoadBalancer,
                                protected Logger::Loggable<Logger::Id::upstream> {
public:
  class OrcaLoadReportHandler;
  using OrcaLoadReportHandlerSharedPtr = std::shared_ptr<OrcaLoadReportHandler>;

  struct HostLbPolicyData : public Upstream::HostLbPolicyData {
    explicit HostLbPolicyData(OrcaLoadReportHandlerSharedPtr handler)
        : report_handler_(std::move(handler)) {}

    absl::Status onOrcaLoadReport(const Upstream::OrcaLoadReport& report,
                                  const StreamInfo::StreamInfo& stream_info) override;

    // Update the weight and timestamps for first and last update time.
    // Uses relaxed memory ordering for weight storage to maximize routing performance.
    // Weights are hints for load distribution; brief visibility delays are acceptable
    // since weights are periodically refreshed (weight_update_period). Timestamps use
    // acquire/release semantics for correct expiration and blackout period enforcement.
    void updateWeightNow(uint32_t weight, const MonotonicTime& now) {
      weight_.store(weight, std::memory_order_relaxed);
      last_update_time_.store(now, std::memory_order_release);
      
      // Use compare-exchange to atomically set non_empty_since_ only on first update.
      // Success ordering is release (to sync with acquire loads in getWeightIfValid).
      // Failure ordering is relaxed since we don't need synchronization if CAS fails.
      MonotonicTime expected = kDefaultNonEmptySince;
      non_empty_since_.compare_exchange_strong(expected, now, 
                                              std::memory_order_release, 
                                              std::memory_order_relaxed);
    }

    // Get the weight if it was updated within valid time bounds, otherwise return nullopt.
    // Thread-safe: can be called concurrently with updateWeightNow().
    //
    // IMPORTANT: This method resets non_empty_since_ on expiration and MUST only be called
    // from the main thread (via updateWeightsOnHosts). Calling from worker threads would
    // cause data races. This constraint is enforced by the calling pattern: only the main
    // thread's periodic timer invokes updateWeightsOnMainThread -> updateWeightsOnHosts.
    absl::optional<uint32_t> getWeightIfValid(MonotonicTime max_non_empty_since,
                                              MonotonicTime min_last_update_time) {
      // Load atomics once to avoid races and redundant loads
      const MonotonicTime non_empty = non_empty_since_.load(std::memory_order_acquire);
      const MonotonicTime last_update = last_update_time_.load(std::memory_order_acquire);
      
      // Check if weight is still in blackout period (too recent)
      if (max_non_empty_since < non_empty) {
        return absl::nullopt;
      }
      // Check if weight has expired (too old)
      if (last_update < min_last_update_time) {
        // Reset non_empty_since_ so the blackout period starts fresh on next weight update.
        // Safe because this method is only called from the main thread.
        non_empty_since_.store(kDefaultNonEmptySince, std::memory_order_release);
        return absl::nullopt;
      }
      return weight_.load(std::memory_order_relaxed);
    }

    OrcaLoadReportHandlerSharedPtr report_handler_;

    std::atomic<uint32_t> weight_{1};
    std::atomic<MonotonicTime> non_empty_since_{kDefaultNonEmptySince};
    std::atomic<MonotonicTime> last_update_time_{kDefaultLastUpdateTime};

    static constexpr MonotonicTime kDefaultNonEmptySince = MonotonicTime::max();
    static constexpr MonotonicTime kDefaultLastUpdateTime = MonotonicTime::min();
  };

  class OrcaLoadReportHandler {
  public:
    OrcaLoadReportHandler(const WrrLocalityLbConfig& config, TimeSource& time_source)
        : metric_names_for_computing_utilization_(config.metricNamesForComputingUtilization()),
          error_utilization_penalty_(config.errorUtilizationPenalty()), time_source_(time_source) {}

    absl::Status updateHostDataFromOrcaLoadReport(const OrcaLoadReportProto& report,
                                                  HostLbPolicyData& host_data);

    static double getUtilizationFromOrcaReport(
        const OrcaLoadReportProto& report,
        const std::vector<std::string>& metric_names_for_computing_utilization);

    static absl::StatusOr<uint32_t> calculateWeightFromOrcaReport(
        const OrcaLoadReportProto& report,
        const std::vector<std::string>& metric_names_for_computing_utilization,
        double error_utilization_penalty);

  private:
    const std::vector<std::string> metric_names_for_computing_utilization_;
    const double error_utilization_penalty_;
    TimeSource& time_source_;
  };

  class ThreadLocalShim : public ThreadLocal::ThreadLocalObject {
  public:
    Common::CallbackManager<void> refresh_cb_helper_;
  };

  class WorkerLocalLb : public Upstream::RoundRobinLoadBalancer {
  public:
    WorkerLocalLb(const WrrLocalityLbConfig& config, const Upstream::PrioritySet& priority_set,
                  const Upstream::PrioritySet* local_priority_set, Upstream::ClusterLbStats& stats,
                  Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source,
                  OptRef<ThreadLocalShim> tls_shim, uint32_t healthy_panic_threshold);

    void refreshAll();

  private:
    Common::CallbackHandlePtr refresh_cb_handle_;
  };

  class WorkerLocalLbFactory : public Upstream::LoadBalancerFactory {
  public:
    WorkerLocalLbFactory(const WrrLocalityLbConfig& config, const Upstream::ClusterInfo& cluster_info,
                         Runtime::Loader& runtime, Random::RandomGenerator& random,
                         TimeSource& time_source, ThreadLocal::SlotAllocator& tls);

    Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override;

    bool recreateOnHostChange() const override { return false; }

    void refreshAllWorkers();

  private:
    const WrrLocalityLbConfig& config_;
    const Upstream::ClusterInfo& cluster_info_;
    Runtime::Loader& runtime_;
    Random::RandomGenerator& random_;
    TimeSource& time_source_;
    std::unique_ptr<ThreadLocal::TypedSlot<ThreadLocalShim>> tls_slot_;
  };

  WrrLocalityLoadBalancer(OptRef<const Upstream::LoadBalancerConfig> lb_config,
                          const Upstream::ClusterInfo& cluster_info,
                          const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
                          Random::RandomGenerator& random, TimeSource& time_source);

  Upstream::LoadBalancerFactorySharedPtr factory() override { return factory_; }
  absl::Status initialize() override;

private:
  void initFromConfig(const WrrLocalityLbConfig& config);

  void addHostLbPolicyData(const Upstream::HostVector& hosts);
  bool updateWeightsOnHosts(const Upstream::HostVector& hosts);
  void updateWeightsOnMainThread();

  static absl::optional<uint32_t>
  getWeightIfValidFromHost(const Upstream::Host& host, MonotonicTime max_non_empty_since,
                           MonotonicTime min_last_update_time);

  OrcaLoadReportHandlerSharedPtr orca_report_handler_;
  std::shared_ptr<WorkerLocalLbFactory> factory_;

  const Upstream::PrioritySet& priority_set_;
  TimeSource& time_source_;

  std::chrono::milliseconds blackout_period_;
  std::chrono::milliseconds weight_expiration_period_;
  std::chrono::milliseconds weight_update_period_;

  Event::TimerPtr weight_update_timer_;
  Common::CallbackHandlePtr priority_update_cb_;
};

} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
