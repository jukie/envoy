#pragma once

#include <bitset>
#include <cmath>
#include <cstdint>
#include <memory>
#include <queue>
#include <set>
#include <vector>

#include "envoy/common/callback.h"
#include "envoy/common/random_generator.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/load_balancing_policies/least_request/v3/least_request.pb.h"
#include "envoy/extensions/load_balancing_policies/random/v3/random.pb.h"
#include "envoy/extensions/load_balancing_policies/round_robin/v3/round_robin.pb.h"
#include "envoy/runtime/runtime.h"
#include "envoy/stream_info/stream_info.h"
#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/upstream.h"

#include "source/common/protobuf/utility.h"
#include "source/common/runtime/runtime_features.h"
#include "source/common/runtime/runtime_protos.h"
#include "source/common/upstream/edf_scheduler.h"
#include "source/common/upstream/load_balancer_context_base.h"
#include "source/extensions/load_balancing_policies/common/locality_wrr.h"

#include "absl/types/optional.h"
#include "absl/status/status.h"

namespace Envoy {
namespace Upstream {

inline bool tooManyPreconnects(size_t num_preconnect_picks, uint32_t healthy_hosts) {
  // Currently we only allow the number of preconnected connections to equal the
  // number of healthy hosts.
  return num_preconnect_picks >= healthy_hosts;
}

// Distributes load between priorities based on the per priority availability and the normalized
// total availability. Load is assigned to each priority according to how available each priority is
// adjusted for the normalized total availability.
//
// @param per_priority_load vector of loads that should be populated.
// @param per_priority_availability the percentage availability of each priority, used to determine
// how much load each priority can handle.
// @param total_load the amount of load that may be distributed. Will be updated with the amount of
// load remaining after distribution.
// @param normalized_total_availability the total availability, up to a max of 100. Used to
// scale the load when the total availability is less than 100%.
// @return the first available priority and the remaining load
std::pair<int32_t, size_t> distributeLoad(PriorityLoad& per_priority_load,
                                          const PriorityAvailability& per_priority_availability,
                                          size_t total_load, size_t normalized_total_availability);

class LoadBalancerConfigHelper {
public:
  template <class Proto>
  static absl::optional<envoy::extensions::load_balancing_policies::common::v3::SlowStartConfig>
  slowStartConfigFromProto(const Proto& proto_config) {
    if (!proto_config.has_slow_start_config()) {
      return {};
    }
    return proto_config.slow_start_config();
  }

  template <class Proto>
  static absl::optional<envoy::extensions::load_balancing_policies::common::v3::LocalityLbConfig>
  localityLbConfigFromProto(const Proto& proto_config) {
    if (!proto_config.has_locality_lb_config()) {
      return {};
    }
    return proto_config.locality_lb_config();
  }

  template <class TargetProto>
  static void
  convertHashLbConfigTo(const envoy::config::cluster::v3::Cluster::CommonLbConfig& source,
                        TargetProto& target) {
    if (source.has_consistent_hashing_lb_config()) {
      target.mutable_consistent_hashing_lb_config()->set_use_hostname_for_hashing(
          source.consistent_hashing_lb_config().use_hostname_for_hashing());

      if (source.consistent_hashing_lb_config().has_hash_balance_factor()) {
        *target.mutable_consistent_hashing_lb_config()->mutable_hash_balance_factor() =
            source.consistent_hashing_lb_config().hash_balance_factor();
      }
    }
  }

  template <class TargetProto>
  static void
  convertLocalityLbConfigTo(const envoy::config::cluster::v3::Cluster::CommonLbConfig& source,
                            TargetProto& target) {
    if (source.has_locality_weighted_lb_config()) {
      target.mutable_locality_lb_config()->mutable_locality_weighted_lb_config();
    } else if (source.has_zone_aware_lb_config()) {
      auto& zone_aware_lb_config =
          *target.mutable_locality_lb_config()->mutable_zone_aware_lb_config();
      const auto& legacy = source.zone_aware_lb_config();

      zone_aware_lb_config.set_fail_traffic_on_panic(legacy.fail_traffic_on_panic());

      if (legacy.has_routing_enabled()) {
        *zone_aware_lb_config.mutable_routing_enabled() = legacy.routing_enabled();
      }
      if (legacy.has_min_cluster_size()) {
        *zone_aware_lb_config.mutable_min_cluster_size() = legacy.min_cluster_size();
      }
    }
  }

  template <class SourceProto, class TargetProto>
  static void convertSlowStartConfigTo(const SourceProto& source, TargetProto& target) {
    if (!source.has_slow_start_config()) {
      return;
    }

    auto& slow_start_config = *target.mutable_slow_start_config();
    const auto& legacy = source.slow_start_config();

    if (legacy.has_slow_start_window()) {
      *slow_start_config.mutable_slow_start_window() = legacy.slow_start_window();
    }
    if (legacy.has_aggression()) {
      *slow_start_config.mutable_aggression() = legacy.aggression();
    }
    if (legacy.has_min_weight_percent()) {
      *slow_start_config.mutable_min_weight_percent() = legacy.min_weight_percent();
    }
  }
};

/**
 * Base class for all LB implementations.
 */
class LoadBalancerBase : public LoadBalancer, protected Logger::Loggable<Logger::Id::upstream> {
public:
  enum class HostAvailability { Healthy, Degraded };

  // A utility function to chose a priority level based on a precomputed hash and
  // two PriorityLoad vectors, one for healthy load and one for degraded.
  //
  // Returns the priority, a number between 0 and per_priority_load.size()-1 as well as which host
  // availability level was chosen.
  static std::pair<uint32_t, HostAvailability>
  choosePriority(uint64_t hash, const HealthyLoad& healthy_per_priority_load,
                 const DegradedLoad& degraded_per_priority_load);

  // Pool selection not implemented.
  absl::optional<Upstream::SelectedPoolAndConnection>
  selectExistingConnection(Upstream::LoadBalancerContext* /*context*/,
                           const Upstream::Host& /*host*/,
                           std::vector<uint8_t>& /*hash_key*/) override {
    return absl::nullopt;
  }
  // Lifetime tracking not implemented.
  OptRef<Envoy::Http::ConnectionPool::ConnectionLifetimeCallbacks> lifetimeCallbacks() override {
    return {};
  }

protected:
  /**
   * For the given host_set @return if we should be in a panic mode or not. For example, if the
   * majority of hosts are unhealthy we'll be likely in a panic mode. In this case we'll route
   * requests to hosts regardless of whether they are healthy or not.
   */
  bool isHostSetInPanic(const HostSet& host_set) const;

  /**
   * Method is called when all host sets are in panic mode.
   * In such state the load is distributed based on the number of hosts
   * in given priority regardless of their health.
   */
  void recalculateLoadInTotalPanic();

  LoadBalancerBase(const PrioritySet& priority_set, ClusterLbStats& stats, Runtime::Loader& runtime,
                   Random::RandomGenerator& random, uint32_t healthy_panic_threshold);

  // Choose host set randomly, based on the healthy_per_priority_load_ and
  // degraded_per_priority_load_. per_priority_load_ is consulted first, spilling over to
  // degraded_per_priority_load_ if necessary. When a host set is selected based on
  // degraded_per_priority_load_, only degraded hosts should be selected from that host set.
  //
  // @return host set to use and which availability to target.
  std::pair<HostSet&, HostAvailability> chooseHostSet(LoadBalancerContext* context,
                                                      uint64_t hash) const;

  uint32_t percentageLoad(uint32_t priority) const {
    return per_priority_load_.healthy_priority_load_.get()[priority];
  }
  uint32_t percentageDegradedLoad(uint32_t priority) const {
    return per_priority_load_.degraded_priority_load_.get()[priority];
  }
  bool isInPanic(uint32_t priority) const { return per_priority_panic_[priority]; }
  uint64_t random(bool peeking);

  ClusterLbStats& stats_;
  Runtime::Loader& runtime_;
  std::deque<uint64_t> stashed_random_;
  Random::RandomGenerator& random_;
  const uint32_t default_healthy_panic_percent_;
  // The priority-ordered set of hosts to use for load balancing.
  const PrioritySet& priority_set_;

public:
  // Called when a host set at the given priority level is updated. This updates
  // per_priority_health for that priority level, and may update per_priority_load for all
  // priority levels.
  void static recalculatePerPriorityState(uint32_t priority, const PrioritySet& priority_set,
                                          HealthyAndDegradedLoad& priority_load,
                                          HealthyAvailability& per_priority_health,
                                          DegradedAvailability& per_priority_degraded,
                                          uint32_t& total_healthy_hosts);
  void recalculatePerPriorityPanic();

protected:
  // Method calculates normalized total availability.
  //
  // The availability of a priority is ratio of available (healthy/degraded) hosts over the total
  // number of hosts multiplied by 100 and the overprovisioning factor. The total availability is
  // the sum of the availability of each priority, up to a maximum of 100.
  //
  // For example, using the default overprovisioning factor of 1.4, a if priority A has 4 hosts,
  // of which 1 is degraded and 1 is healthy, it will have availability of 2/4 * 100 * 1.4 = 70.
  //
  // Assuming two priorities with availability 60 and 70, the total availability would be 100.
  static uint32_t
  calculateNormalizedTotalAvailability(HealthyAvailability& per_priority_health,
                                       DegradedAvailability& per_priority_degraded) {
    const auto health =
        std::accumulate(per_priority_health.get().begin(), per_priority_health.get().end(), 0);
    const auto degraded =
        std::accumulate(per_priority_degraded.get().begin(), per_priority_degraded.get().end(), 0);

    return std::min<uint32_t>(health + degraded, 100);
  }

  // The percentage load (0-100) for each priority level when targeting healthy hosts and
  // the percentage load (0-100) for each priority level when targeting degraded hosts.
  HealthyAndDegradedLoad per_priority_load_;
  // The health percentage (0-100) for each priority level.
  HealthyAvailability per_priority_health_;
  // The degraded percentage (0-100) for each priority level.
  DegradedAvailability per_priority_degraded_;
  // Levels which are in panic
  std::vector<bool> per_priority_panic_;
  // The total count of healthy hosts across all priority levels.
  uint32_t total_healthy_hosts_;

private:
  Common::CallbackHandlePtr priority_update_cb_;
};

/**
 * Base class for zone aware load balancers
 */
class ZoneAwareLoadBalancerBase : public LoadBalancerBase {
public:
  using LocalityLbConfig = envoy::extensions::load_balancing_policies::common::v3::LocalityLbConfig;

  HostSelectionResponse chooseHost(LoadBalancerContext* context) override;

protected:
  // Both priority_set and local_priority_set if non-null must have at least one host set.
  ZoneAwareLoadBalancerBase(const PrioritySet& priority_set, const PrioritySet* local_priority_set,
                            ClusterLbStats& stats, Runtime::Loader& runtime,
                            Random::RandomGenerator& random, uint32_t healthy_panic_threshold,
                            const absl::optional<LocalityLbConfig> locality_config,
                            TimeSource& time_source);

  // When deciding which hosts to use on an LB decision, we need to know how to index into the
  // priority_set. This priority_set cursor is used by ZoneAwareLoadBalancerBase subclasses, e.g.
  // RoundRobinLoadBalancer, to index into auxiliary data structures specific to the LB for
  // a given host set selection.
  struct HostsSource {
    enum class SourceType : uint8_t {
      // All hosts in the host set.
      AllHosts,
      // All healthy hosts in the host set.
      HealthyHosts,
      // All degraded hosts in the host set.
      DegradedHosts,
      // Healthy hosts for locality @ locality_index.
      LocalityHealthyHosts,
      // Degraded hosts for locality @ locality_index.
      LocalityDegradedHosts,
    };

    HostsSource() = default;

    // TODO(kbaichoo): plumb the priority parameter as uint8_t all the way from
    // the config.
    HostsSource(uint32_t priority, SourceType source_type)
        : priority_(static_cast<uint8_t>(priority)), source_type_(source_type) {
      ASSERT(priority <= 128, "Priority out of bounds.");
      ASSERT(source_type == SourceType::AllHosts || source_type == SourceType::HealthyHosts ||
             source_type == SourceType::DegradedHosts);
    }

    // TODO(kbaichoo): plumb the priority parameter as uint8_t all the way from
    // the config.
    HostsSource(uint32_t priority, SourceType source_type, uint32_t locality_index)
        : priority_(static_cast<uint8_t>(priority)), source_type_(source_type),
          locality_index_(locality_index) {
      ASSERT(priority <= 128, "Priority out of bounds.");
      ASSERT(source_type == SourceType::LocalityHealthyHosts ||
             source_type == SourceType::LocalityDegradedHosts);
    }

    // Priority in PrioritySet.
    uint8_t priority_{};

    // How to index into HostSet for a given priority.
    SourceType source_type_{};

    // Locality index into HostsPerLocality for SourceType::LocalityHealthyHosts.
    uint32_t locality_index_{};

    bool operator==(const HostsSource& other) const {
      return priority_ == other.priority_ && source_type_ == other.source_type_ &&
             locality_index_ == other.locality_index_;
    }
  };

  struct HostsSourceHash {
    size_t operator()(const HostsSource& hs) const {
      // This is only used for absl::flat_hash_map keys, so we don't need a deterministic hash.
      size_t hash = std::hash<uint32_t>()(hs.priority_);
      hash = 37 * hash + std::hash<size_t>()(static_cast<std::size_t>(hs.source_type_));
      hash = 37 * hash + std::hash<uint32_t>()(hs.locality_index_);
      return hash;
    }
  };

  /**
   * By implementing this method instead of chooseHost, host selection will
   * be subject to host filters specified by LoadBalancerContext.
   *
   * Host selection will be retried up to the number specified by
   * hostSelectionRetryCount on LoadBalancerContext, and if no hosts are found
   * within the allowed attempts, the host that was selected during the last
   * attempt will be returned.
   *
   * If host selection is idempotent (i.e. retrying will not change the outcome),
   * sub classes should override chooseHost to avoid the unnecessary overhead of
   * retrying host selection.
   */
  virtual HostConstSharedPtr chooseHostOnce(LoadBalancerContext* context) PURE;

  /**
   * Pick the host source to use, doing zone aware routing when the hosts are sufficiently healthy.
   * If no host is chosen (due to fail_traffic_on_panic being set), return absl::nullopt.
   */
  absl::optional<HostsSource> hostSourceToUse(LoadBalancerContext* context, uint64_t hash) const;

  /**
   * Index into priority_set via hosts source descriptor.
   */
  const HostVector& hostSourceToHosts(HostsSource hosts_source) const;

private:
  enum class LocalityRoutingState {
    // Locality based routing is off.
    NoLocalityRouting,
    // All queries can be routed to the local locality.
    LocalityDirect,
    // The local locality can not handle the anticipated load. Residual load will be spread across
    // various other localities.
    LocalityResidual
  };

  struct LocalityPercentages {
    // The percentage of local hosts in a specific locality.
    // Percentage is stored as integer number and scaled by 10000 multiplier for better precision.
    // If upstream_percentage is 0, local_percentage may not be representative
    // of the actual percentage and will be set to 0.
    uint64_t local_percentage;
    // The percentage of upstream hosts in a specific locality.
    // Percentage is stored as integer number and scaled by 10000 multiplier for better precision.
    uint64_t upstream_percentage;
  };

  /**
   * Increase per_priority_state_ to at least priority_set.hostSetsPerPriority().size()
   */
  void resizePerPriorityState();

  /**
   * @return decision on quick exit from locality aware routing based on cluster configuration.
   * This gets recalculated on update callback.
   */
  bool earlyExitNonLocalityRouting();

protected:
  /**
   * Try to select upstream hosts from the same locality.
   * @param host_set the last host set returned by chooseHostSet()
   */
  uint32_t tryChooseLocalLocalityHosts(const HostSet& host_set) const;

  /**
   * @return combined per-locality information about percentages of local/upstream hosts in each
   * upstream locality. See LocalityPercentages for more details. The ordering of localities
   * matches the ordering of upstream localities in the input upstream_hosts_per_locality.
   */
  absl::FixedArray<LocalityPercentages>
  calculateLocalityPercentages(const HostsPerLocality& local_hosts_per_locality,
                               const HostsPerLocality& upstream_hosts_per_locality);

  /**
   * Calculate locality percentages for ORCA_LOAD mode.
   * This uses a differential routing approach where routing decisions are based purely on
   * upstream utilization to equalize load across zones.
   */
  absl::FixedArray<LocalityPercentages>
  calculateLocalityPercentagesOrcaLoad(const HostsPerLocality& local_hosts_per_locality,
                                       const HostsPerLocality& upstream_hosts_per_locality);

  /**
   * Regenerate locality aware routing structures for fast decisions on upstream locality selection.
   */
  void regenerateLocalityRoutingStructures();

  HostSet& localHostSet() const { return *local_priority_set_->hostSetsPerPriority()[0]; }

  static absl::optional<HostsSource::SourceType>
  localitySourceType(HostAvailability host_availability) {
    switch (host_availability) {
    case HostAvailability::Healthy:
      return absl::make_optional<HostsSource::SourceType>(
          HostsSource::SourceType::LocalityHealthyHosts);
    case HostAvailability::Degraded:
      return absl::make_optional<HostsSource::SourceType>(
          HostsSource::SourceType::LocalityDegradedHosts);
    }
    IS_ENVOY_BUG("unexpected locality source type enum");
    return absl::nullopt;
  }

  static absl::optional<HostsSource::SourceType> sourceType(HostAvailability host_availability) {
    switch (host_availability) {
    case HostAvailability::Healthy:
      return absl::make_optional<HostsSource::SourceType>(HostsSource::SourceType::HealthyHosts);
    case HostAvailability::Degraded:
      return absl::make_optional<HostsSource::SourceType>(HostsSource::SourceType::DegradedHosts);
    }

    IS_ENVOY_BUG("unexpected source type enum");
    return absl::nullopt;
  }

  absl::optional<uint32_t> chooseHealthyLocality(HostSet& host_set) const {
    ASSERT(per_priority_state_[host_set.priority()]->locality_wrr_);
    return per_priority_state_[host_set.priority()]->locality_wrr_->chooseHealthyLocality();
  };

  absl::optional<uint32_t> chooseDegradedLocality(HostSet& host_set) const {
    ASSERT(per_priority_state_[host_set.priority()]->locality_wrr_);
    return per_priority_state_[host_set.priority()]->locality_wrr_->chooseDegradedLocality();
  };

  // ORCA-based zone metrics support
  // Cached zone-level metrics aggregated from host ORCA reports
  struct CachedZoneMetrics {
    // Locality weight calculated from backend load (inverse of utilization)
    // Higher weight = more capacity = route more traffic here
    uint64_t locality_weight{0};

    // Average utilization across reporting hosts in this zone (0.0-1.0)
    double avg_utilization{0.0};

    // Average CPU utilization across reporting hosts (0.0-1.0)
    double avg_cpu{0.0};

    // Number of hosts that reported ORCA data for this zone
    uint32_t reporting_hosts{0};

    // When these metrics were last updated
    MonotonicTime last_updated;

    // Whether these metrics are valid (enough hosts reporting)
    bool is_valid{false};
  };

  // Host-attached LB policy data to receive ORCA reports directly from the router and cache
  // the latest utilization values per host. This provides a push path complementing
  // LoadMetricStats (which is pull-based and may be empty when first read).
  struct OrcaHostLbPolicyData : public HostLbPolicyData {
    absl::Status onOrcaLoadReport(const Upstream::OrcaLoadReport& report) override {
      // Update atomics with latest values. Prefer application_utilization if > 0.
      double app = report.application_utilization();
      double cpu = report.cpu_utilization();
      if (app > 0) {
        last_app_utilization.store(app, std::memory_order_relaxed);
      }
      if (cpu > 0) {
        last_cpu_utilization.store(cpu, std::memory_order_relaxed);
      }
      // Bump update marker to indicate we have received fresh ORCA data.
      last_update_ns.fetch_add(1, std::memory_order_relaxed);
      return absl::OkStatus();
    }

    // Update OOB reporting state when a periodic report is received
    void updateOobReportingState(bool is_oob_report) {
      if (is_oob_report) {
        // Convert current time to nanoseconds for atomic storage
        const uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        oob_last_update_ns.store(now_ns, std::memory_order_relaxed);
        oob_reporting_active.store(true, std::memory_order_relaxed);
      }
    }

    absl::optional<double> app() const {
      const double v = last_app_utilization.load(std::memory_order_relaxed);
      return v > 0 ? absl::make_optional(v) : absl::nullopt;
    }
    absl::optional<double> cpu() const {
      const double v = last_cpu_utilization.load(std::memory_order_relaxed);
      return v > 0 ? absl::make_optional(v) : absl::nullopt;
    }

    std::atomic<double> last_cpu_utilization{0.0};
    std::atomic<double> last_app_utilization{0.0};
    std::atomic<uint64_t> last_update_ns{0};

    // Out-of-band (OOB) ORCA reporting fields
    //
    // These fields track the state of periodic OORCA reports when OOB reporting is enabled.
    // They allow the load balancer to prefer fresh OOB data over per-request metrics
    // while gracefully falling back when OOB data becomes stale.

    /// Timestamp of the last OOB report received (nanoseconds since epoch)
    /// Stored as atomic for thread-safe access between load balancer threads
    /// Used to determine if OOB data has expired based on oob_expiration_period
    std::atomic<uint64_t> oob_last_update_ns{0};

    /// Whether this host has active OOB reporting enabled
    /// - true: Host is sending OOB reports and data is considered fresh
    /// - false: OOB reporting not active or data has expired
    /// Updated atomically when OOB reports are received or expire
    std::atomic<bool> oob_reporting_active{false};
  };

  // Update zone metrics for all host sets (called on-demand when stale)
  void updateZoneMetricsOnMainThread();

  // Check if zone metrics need updating and refresh if stale (lazy update)
  void maybeUpdateZoneMetrics();

  // OOB reporting timer management
  void startOobExpirationTimer();
  void onOobExpirationTimer();
  void checkExpiredOobReports();

  // OOB-first zone metrics cache operations
  // Get zone metrics using OOB-first approach (prioritizes cached OOB data)
  absl::optional<CachedZoneMetrics> getOobFirstZoneMetrics(
      const envoy::config::core::v3::Locality& locality);

  // Update OOB-first cache when new OOB data is received
  void updateOobFirstCache(const envoy::config::core::v3::Locality& locality,
                          const CachedZoneMetrics& metrics);

  // Check if cross-zone probing is needed based on OOB-first strategy
  bool needsCrossZoneProbing(const envoy::config::core::v3::Locality& locality,
                           const CachedZoneMetrics& current_metrics);

  // Perform intelligent cross-zone probe with rate limiting
  bool performIntelligentProbe(const envoy::config::core::v3::Locality& locality,
                              const HostVector& hosts);

  // Aggregate OOB metrics at zone level (if enabled)
  CachedZoneMetrics aggregateOobMetricsForZone(
      const envoy::config::core::v3::Locality& locality,
      const HostVector& hosts);

  // Apply fallback strategy when OOB data is unavailable
  absl::optional<CachedZoneMetrics> applyFallbackStrategy(
      const envoy::config::core::v3::Locality& locality,
      const HostVector& hosts);

  // Reset probe rate limiting counters (called periodically)
  void resetProbeRateCounters();

  // Aggregate ORCA metrics for a specific host set
  void aggregateZoneMetricsForHostSet(const HostSet& host_set);

  // Calculate zone metrics from a vector of hosts in a locality
  CachedZoneMetrics
  calculateZoneMetricsFromHosts(const HostVector& hosts,
                                const envoy::config::core::v3::Locality& locality);

  // Get cached ORCA-based locality weight (hot-path, O(1) lookup)
  uint64_t getCachedOrcaBasedLocalityWeight(const HostVector& locality_hosts,
                                            const envoy::config::core::v3::Locality& locality);

  // Extract utilization metric from a host's ORCA data (if available)
  // Returns nullopt if the host doesn't have valid ORCA data
  // Uses runtime type checking to avoid circular dependencies
  absl::optional<double> getUtilizationFromHost(const Host& host) const;

  // Calculate fallback weight when ORCA data is insufficient
  uint64_t calculateFallbackLocalityWeight(const HostVector& locality_hosts,
                                           const envoy::config::core::v3::Locality& locality) const;

  // Helper functions for calculateLocalityPercentagesOrcaLoad

  // Structure to hold utilization information for a locality
  struct LocalityUtilizationInfo {
    double utilization; // 0.0 to 1.0
    double capacity;    // 1.0 - utilization
    size_t num_hosts;
    bool is_local;
    bool is_valid; // true if we have valid ORCA data
    const envoy::config::core::v3::Locality* locality;
  };

  // Collect utilization information for all upstream localities
  std::vector<LocalityUtilizationInfo>
  collectLocalityUtilizationInfo(const HostsPerLocality& upstream_hosts_per_locality,
                                 const envoy::config::core::v3::Locality* local_locality_ptr,
                                 double& local_utilization, double& min_other_utilization,
                                 bool& found_local);

  // Calculate routing weights based on inverse utilization for equal distribution
  void
  calculateInverseUtilizationWeights(const std::vector<LocalityUtilizationInfo>& locality_utils,
                                     double& total_routing_weight, size_t& valid_localities);

  // Generate final locality percentages from utilization information and routing weights
  absl::FixedArray<LocalityPercentages>
  generateLocalityPercentages(const std::vector<LocalityUtilizationInfo>& locality_utils,
                              const HostsPerLocality& upstream_hosts_per_locality,
                              double total_routing_weight, size_t valid_localities,
                              double local_utilization, double min_other_utilization,
                              bool found_local);

  // The set of local Envoy instances which are load balancing across priority_set_.
  const PrioritySet* local_priority_set_;

  struct PerPriorityState {
    // The percent of requests which can be routed to the local locality.
    uint64_t local_percent_to_route_{};
    // Tracks the current state of locality based routing.
    LocalityRoutingState locality_routing_state_{LocalityRoutingState::NoLocalityRouting};
    // When locality_routing_state_ == LocalityResidual this tracks the capacity
    // for each of the non-local localities to determine what traffic should be
    // routed where.
    std::vector<uint64_t> residual_capacity_;

    // Locality Weighted Round Robin config.
    std::unique_ptr<LocalityWrr> locality_wrr_;
  };
  using PerPriorityStatePtr = std::unique_ptr<PerPriorityState>;

  void rebuildLocalityWrrForPriority(uint32_t priority);

  // Routing state broken out for each priority level in priority_set_.
  std::vector<PerPriorityStatePtr> per_priority_state_;
  Common::CallbackHandlePtr priority_update_cb_;
  Common::CallbackHandlePtr local_priority_set_member_update_cb_handle_;

  // Config for zone aware routing.
  const uint64_t min_cluster_size_;
  const absl::optional<uint32_t> force_local_zone_min_size_;
  // Keep small members (bools and enums) at the end of class, to reduce alignment overhead.
  const uint32_t routing_enabled_;
  const LocalityLbConfig::ZoneAwareLbConfig::LocalityBasis locality_basis_;
  const bool fail_traffic_on_panic_ : 1;

  // If locality weight aware routing is enabled.
  const bool locality_weighted_balancing_ : 1;

  // ORCA-based zone metrics cache and configuration
  // Map from locality to cached zone metrics (updated on-demand when stale)
  absl::flat_hash_map<envoy::config::core::v3::Locality, CachedZoneMetrics, LocalityHash,
                      LocalityEqualTo>
      cached_zone_metrics_;

  // OOB-first zone metrics cache with intelligent probing
  // This cache prioritizes OOB data and only falls back to cross-zone probing when necessary
  struct OobFirstZoneMetrics {
    // Latest OOB metrics received for this zone
    CachedZoneMetrics oob_metrics;

    // Last time we received OOB data for this zone
    MonotonicTime last_oob_update{MonotonicTime::min()};

    // Whether we have valid OOB data for this zone
    bool has_valid_oob_data{false};

    // Last time we had to cross-zone probe this zone
    MonotonicTime last_probe_time{MonotonicTime::min()};

    // Number of cross-zone probes sent to this zone (rate limiting)
    uint32_t probes_sent_this_minute{0};

    // Whether we're in probe cooldown after recent probing
    bool in_probe_cooldown{false};
  };

  // OOB-first cache mapping
  absl::flat_hash_map<envoy::config::core::v3::Locality, OobFirstZoneMetrics, LocalityHash,
                      LocalityEqualTo>
      oob_first_zone_cache_;

  // Last time zone metrics were refreshed (for lazy updates)
  MonotonicTime zone_metrics_last_refresh_{MonotonicTime::min()};

  // How often to check if zone metrics need updating (default: 1 second)
  // Metrics are refreshed on-demand in the hot path when stale
  std::chrono::milliseconds zone_metrics_update_interval_{1000};

  // Minimum hosts reporting ORCA before trusting zone metrics
  uint32_t min_orca_reporting_hosts_{3};

  // ORCA utilization metrics configuration (3-tier hierarchy like CSWRR)
  std::vector<std::string> orca_utilization_metric_names_;
  bool orca_use_application_utilization_{true};
  bool orca_use_cpu_utilization_{true};

  // Pre-computed metric keys to avoid string allocations in hot path
  std::vector<std::string> orca_utilization_metric_keys_;

  // Error utilization penalty multiplier (aligns with CSWRR)
  double orca_error_utilization_penalty_{1.0};

  // Fallback locality basis when ORCA data insufficient
  LocalityLbConfig::ZoneAwareLbConfig::LocalityBasis orca_fallback_basis_{
      LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_NUM};

  // Threshold for LocalityDirect routing with ORCA (percentage points, 0-100)
  // Allows small differences in ORCA metrics without triggering cross-zone routing
  uint32_t locality_direct_threshold_pct_{5}; // Default: 5%

  // Exponential moving average smoothing factor for zone metrics (0.0-1.0)
  // Higher = more responsive, Lower = more stable
  // Default: 0.2 (20% new data, 80% history)
  double zone_metrics_smoothing_factor_{0.2};

  // Blackout period for zone routing weights (aligns with CSWRR)
  // Prevents rapid changes in routing decisions for stability
  std::chrono::milliseconds zone_routing_blackout_period_{10000}; // 10 seconds (same as CSWRR)

  // Out-of-band (OOB) ORCA reporting configuration
  //
  // When enabled, backends can proactively send metrics on a periodic basis rather
  // than only per-request. This reduces per-request overhead and provides more
  // timely load information for zone-aware routing decisions.
  //
  // OOB reporting complements the standard per-request LoadMetricStats:
  // - OOB data: Preferred when available and fresh (lower overhead)
  // - Per-request: Fallback when OOB unavailable or expired

  /// Whether OOB reporting is enabled via configuration
  bool oob_enabled_{false};

  /// How often to request OOB reports from backends (default: 10s, same as CSWRR)
  /// Backends may not respect this exact interval but should report frequently
  std::chrono::milliseconds oob_reporting_period_{10000};

  /// How long before OOB metrics are considered stale (default: 3min, same as CSWRR)
  /// After this period, zone routing falls back to per-request metrics
  std::chrono::milliseconds oob_expiration_period_{180000};

  /// Timer for periodic OOB metric expiration checks
  /// TODO: Implementation pending dispatcher access in base class
  Event::TimerPtr oob_expiration_timer_;

  // OOB-first mode configuration
  //
  // When enabled, the load balancer prioritizes cached OOB metrics over cross-zone probing,
  // only sending probes when absolutely necessary. This dramatically reduces cross-zone
  // traffic and improves latency for zone-aware routing.

  /// Whether OOB-first mode is enabled via configuration
  bool oob_first_enabled_{false};

  /// Strategy for handling missing or stale OOB data in OOB-first mode
  LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::OobFallbackStrategy
      oob_fallback_strategy_{LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::PROBE_FALLBACK};

  /// Trigger conditions for cross-zone probing in OOB-first mode
  LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::ProbeTriggers oob_probe_triggers_;

  /// Zone-level aggregation configuration for OOB metrics
  LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::ZoneAggregationConfig
      oob_zone_aggregation_config_;

  // Always-probe: if enabled, send a constant, low-percentage trickle of traffic cross-zone even
  // when all zones have ORCA data and LocalityDirect would otherwise route 100% local. This keeps
  // ORCA fresh and can help detect rapid capacity changes. Use sparingly in production.
  // Controlled via runtime keys:
  //  - upstream.zone_routing.orca_probe_always.enabled (default: 0)
  //  - upstream.zone_routing.orca_probe_always.percent (0-100, default: 0)
  bool orca_probe_always_enabled_{true};
  uint32_t orca_probe_always_permyriad_{100};

  // Time source for ORCA zone metrics timestamps and lazy updates
  TimeSource& time_source_;

  friend class TestZoneAwareLoadBalancer;
  friend class TestOrcaZoneAwareLoadBalancer;
};

/**
 * Base implementation of LoadBalancer that performs weighted RR selection across the hosts in the
 * cluster. This scheduler respects host weighting and utilizes an EdfScheduler to achieve O(log
 * n) pick and insertion time complexity, O(n) memory use. The key insight is that if we schedule
 * with 1 / weight deadline, we will achieve the desired pick frequency for weighted RR in a given
 * interval. Naive implementations of weighted RR are either O(n) pick time or O(m * n) memory use,
 * where m is the weight range. We also explicitly check for the unweighted special case and use a
 * simple index to achieve O(1) scheduling in that case.
 * TODO(htuch): We use EDF at Google, but the EDF scheduler may be overkill if we don't want to
 * support large ranges of weights or arbitrary precision floating weights, we could construct an
 * explicit schedule, since m will be a small constant factor in O(m * n). This
 * could also be done on a thread aware LB, avoiding creating multiple EDF
 * instances.
 *
 * This base class also supports unweighted selection which derived classes can use to customize
 * behavior. Derived classes can also override how host weight is determined when in weighted mode.
 */
class EdfLoadBalancerBase : public ZoneAwareLoadBalancerBase {
public:
  using SlowStartConfig = envoy::extensions::load_balancing_policies::common::v3::SlowStartConfig;

  EdfLoadBalancerBase(const PrioritySet& priority_set, const PrioritySet* local_priority_set,
                      ClusterLbStats& stats, Runtime::Loader& runtime,
                      Random::RandomGenerator& random, uint32_t healthy_panic_threshold,
                      const absl::optional<LocalityLbConfig> locality_config,
                      const absl::optional<SlowStartConfig> slow_start_config,
                      TimeSource& time_source);

  // Upstream::ZoneAwareLoadBalancerBase
  HostConstSharedPtr peekAnotherHost(LoadBalancerContext* context) override;
  HostConstSharedPtr chooseHostOnce(LoadBalancerContext* context) override;

protected:
  struct Scheduler {
    // EdfScheduler for weighted LB. The edf_ is only created when the original
    // host weights of 2 or more hosts differ. When not present, the
    // implementation of chooseHostOnce falls back to unweightedHostPick.
    std::unique_ptr<EdfScheduler<Host>> edf_;
  };

  void initialize();

  virtual void refresh(uint32_t priority);

  bool isSlowStartEnabled() const;
  bool noHostsAreInSlowStart() const;

  virtual void recalculateHostsInSlowStart(const HostVector& hosts_added);

  // Seed to allow us to desynchronize load balancers across a fleet. If we don't
  // do this, multiple Envoys that receive an update at the same time (or even
  // multiple load balancers on the same host) will send requests to
  // backends in roughly lock step, causing significant imbalance and potential
  // overload.
  const uint64_t seed_;

  double applySlowStartFactor(double host_weight, const Host& host) const;

private:
  friend class EdfLoadBalancerBasePeer;
  virtual void refreshHostSource(const HostsSource& source) PURE;
  virtual double hostWeight(const Host& host) const PURE;
  virtual HostConstSharedPtr unweightedHostPeek(const HostVector& hosts_to_use,
                                                const HostsSource& source) PURE;
  virtual HostConstSharedPtr unweightedHostPick(const HostVector& hosts_to_use,
                                                const HostsSource& source) PURE;

  // Scheduler for each valid HostsSource.
  absl::flat_hash_map<HostsSource, Scheduler, HostsSourceHash> scheduler_;
  Common::CallbackHandlePtr priority_update_cb_;
  Common::CallbackHandlePtr member_update_cb_;

protected:
  // Slow start related config
  const std::chrono::milliseconds slow_start_window_;
  const absl::optional<Runtime::Double> aggression_runtime_;
  TimeSource& time_source_;
  MonotonicTime latest_host_added_time_;
  const double slow_start_min_weight_percent_;
};

} // namespace Upstream
} // namespace Envoy
