#include "source/common/orca/orca_load_metrics.h"
#include "source/common/upstream/upstream_impl.h"

#include "test/test_common/status_utility.h"
#include "test/test_common/utility.h"

using ::Envoy::Upstream::LoadMetricStats;
using ::testing::DoubleEq;
using ::testing::Field;

namespace Envoy {
namespace Orca {
namespace {

xds::data::orca::v3::OrcaLoadReport makeOrcaReport() {
  xds::data::orca::v3::OrcaLoadReport report;
  report.mutable_named_metrics()->insert({"nm_foo", 0.1});
  report.mutable_named_metrics()->insert({"nm_bar", 0.2});
  report.mutable_request_cost()->insert({"rc_foo", 0.4});
  report.mutable_request_cost()->insert({"rc_bar", 0.5});
  report.mutable_utilization()->insert({"ut_foo", 0.6});
  report.mutable_utilization()->insert({"ut_bar", 0.7});
  report.set_application_utilization(0.8);
  report.set_cpu_utilization(0.9);
  report.set_mem_utilization(1.0);
  report.set_eps(10);
  report.set_rps_fractional(11);
  return report;
}

TEST(OrcaLoadMetricsTest, AddCpuUtilization) {
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("cpu_utilization");

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaLoadReportToLoadMetricStats(metric_names, makeOrcaReport(), stats);
  auto load_stats_map = stats.latch();
  ASSERT_NE(load_stats_map, nullptr);
  EXPECT_EQ(load_stats_map->size(), 1);

  EXPECT_EQ(load_stats_map->at("cpu_utilization").total_metric_value, 0.9);
  EXPECT_EQ(load_stats_map->at("cpu_utilization").num_requests_with_metric, 1);
}

TEST(OrcaLoadMetricsTest, AddSpecificNamedMetrics) {
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("named_metrics.foo");
  metric_names.push_back("named_metrics.not-in-report");

  xds::data::orca::v3::OrcaLoadReport report;
  report.mutable_named_metrics()->insert({"foo", 0.7});
  report.mutable_named_metrics()->insert({"not-in-config", 0.3});

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaLoadReportToLoadMetricStats(metric_names, report, stats);
  auto load_stats_map = stats.latch();
  ASSERT_NE(load_stats_map, nullptr);
  EXPECT_EQ(load_stats_map->size(), 1);
  EXPECT_EQ(load_stats_map->at("named_metrics.foo").total_metric_value, 0.7);
}

TEST(OrcaLoadMetricsTest, AddWildcardUtilization) {
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("utilization.*");

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaLoadReportToLoadMetricStats(metric_names, makeOrcaReport(), stats);
  auto load_stats_map = stats.latch();
  ASSERT_NE(load_stats_map, nullptr);
  EXPECT_THAT(*load_stats_map,
              UnorderedElementsAre(
                  Pair("utilization.ut_foo",
                       AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                             Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.6)))),
                  Pair("utilization.ut_bar",
                       AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                             Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.7))))));
}

TEST(OrcaLoadMetricsTest, AddAllReportedMetrics) {
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("application_utilization");
  metric_names.push_back("cpu_utilization");
  metric_names.push_back("mem_utilization");
  metric_names.push_back("eps");
  metric_names.push_back("rps_fractional");
  metric_names.push_back("named_metrics.*");
  metric_names.push_back("utilization.*");
  metric_names.push_back("request_cost.*");

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaLoadReportToLoadMetricStats(metric_names, makeOrcaReport(), stats);
  auto load_stats_map = stats.latch();
  ASSERT_NE(load_stats_map, nullptr);
  EXPECT_THAT(
      *load_stats_map,
      UnorderedElementsAre(
          Pair("named_metrics.nm_foo",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.1)))),
          Pair("named_metrics.nm_bar",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.2)))),
          Pair("request_cost.rc_foo",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.4)))),
          Pair("request_cost.rc_bar",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.5)))),
          Pair("utilization.ut_foo",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.6)))),
          Pair("utilization.ut_bar",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.7)))),
          Pair("application_utilization",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.8)))),
          Pair("cpu_utilization",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.9)))),
          Pair("mem_utilization",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(1.0)))),
          Pair("eps", AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                            Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(10)))),
          Pair("rps_fractional",
               AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                     Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(11))))));
}

// Tests for addOrcaRequestCostToLoadMetricStats (request_cost-only forwarding for OOB mode).

TEST(OrcaLoadMetricsTest, RequestCostOnly_ForwardsRequestCost) {
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("request_cost.*");

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaRequestCostToLoadMetricStats(metric_names, makeOrcaReport(), stats);
  auto load_stats_map = stats.latch();
  ASSERT_NE(load_stats_map, nullptr);
  EXPECT_THAT(*load_stats_map,
              UnorderedElementsAre(
                  Pair("request_cost.rc_foo",
                       AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                             Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.4)))),
                  Pair("request_cost.rc_bar",
                       AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                             Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.5))))));
}

TEST(OrcaLoadMetricsTest, RequestCostOnly_IgnoresUtilizationAndQps) {
  // Even when all metric types are in the metric_names list,
  // addOrcaRequestCostToLoadMetricStats should only forward request_cost entries.
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("application_utilization");
  metric_names.push_back("cpu_utilization");
  metric_names.push_back("mem_utilization");
  metric_names.push_back("eps");
  metric_names.push_back("rps_fractional");
  metric_names.push_back("named_metrics.*");
  metric_names.push_back("utilization.*");
  metric_names.push_back("request_cost.*");

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaRequestCostToLoadMetricStats(metric_names, makeOrcaReport(), stats);
  auto load_stats_map = stats.latch();
  ASSERT_NE(load_stats_map, nullptr);
  // Only request_cost entries should be present.
  EXPECT_EQ(load_stats_map->size(), 2);
  EXPECT_THAT(*load_stats_map,
              UnorderedElementsAre(
                  Pair("request_cost.rc_foo",
                       AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                             Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.4)))),
                  Pair("request_cost.rc_bar",
                       AllOf(Field(&LoadMetricStats::Stat::num_requests_with_metric, 1),
                             Field(&LoadMetricStats::Stat::total_metric_value, DoubleEq(0.5))))));
}

TEST(OrcaLoadMetricsTest, RequestCostOnly_EmptyRequestCost) {
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("request_cost.*");

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.9);
  // No request_cost entries in the report.

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaRequestCostToLoadMetricStats(metric_names, report, stats);
  auto load_stats_map = stats.latch();
  // No entries should be produced.
  EXPECT_EQ(load_stats_map, nullptr);
}

TEST(OrcaLoadMetricsTest, RequestCostOnly_SpecificRequestCost) {
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("request_cost.rc_foo");

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaRequestCostToLoadMetricStats(metric_names, makeOrcaReport(), stats);
  auto load_stats_map = stats.latch();
  ASSERT_NE(load_stats_map, nullptr);
  EXPECT_EQ(load_stats_map->size(), 1);
  EXPECT_EQ(load_stats_map->at("request_cost.rc_foo").total_metric_value, 0.4);
  EXPECT_EQ(load_stats_map->at("request_cost.rc_foo").num_requests_with_metric, 1);
}

TEST(OrcaLoadMetricsTest, RequestCostOnly_NoRequestCostInMetricNames) {
  // If metric_names only has non-request_cost entries, nothing should be forwarded.
  Envoy::Orca::LrsReportMetricNames metric_names;
  metric_names.push_back("cpu_utilization");
  metric_names.push_back("utilization.*");

  Envoy::Upstream::LoadMetricStatsImpl stats;
  Envoy::Orca::addOrcaRequestCostToLoadMetricStats(metric_names, makeOrcaReport(), stats);
  auto load_stats_map = stats.latch();
  EXPECT_EQ(load_stats_map, nullptr);
}

} // namespace
} // namespace Orca
} // namespace Envoy
