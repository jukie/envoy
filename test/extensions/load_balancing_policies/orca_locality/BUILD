load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "orca_locality_manager_test",
    srcs = ["orca_locality_manager_test.cc"],
    extension_names = ["envoy.load_balancing_policies.orca_locality"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/load_balancing_policies/common:orca_weight_manager_lib",
        "//source/extensions/load_balancing_policies/orca_locality:orca_locality_lb_lib",
        "//test/mocks:common_lib",
        "//test/mocks/event:event_mocks",
        "//test/mocks/upstream:host_mocks",
        "//test/mocks/upstream:host_set_mocks",
        "//test/mocks/upstream:priority_set_mocks",
        "//test/test_common:simulated_time_system_lib",
    ],
)

envoy_extension_cc_test(
    name = "orca_locality_lb_test",
    srcs = ["orca_locality_lb_test.cc"],
    extension_names = ["envoy.load_balancing_policies.orca_locality"],
    rbe_pool = "6gig",
    deps = [
        "//source/common/upstream:upstream_lib",
        "//source/extensions/load_balancing_policies/common:orca_weight_manager_lib",
        "//source/extensions/load_balancing_policies/orca_locality:orca_locality_lb_lib",
        "//test/mocks:common_lib",
        "//test/mocks/event:event_mocks",
        "//test/mocks/thread_local:thread_local_mocks",
        "//test/mocks/upstream:cluster_info_mocks",
        "//test/mocks/upstream:host_mocks",
        "//test/mocks/upstream:host_set_mocks",
        "//test/mocks/upstream:priority_set_mocks",
        "//test/mocks/upstream:thread_aware_load_balancer_mocks",
        "//test/test_common:simulated_time_system_lib",
    ],
)

envoy_extension_cc_test(
    name = "config_test",
    srcs = ["config_test.cc"],
    extension_names = ["envoy.load_balancing_policies.orca_locality"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/load_balancing_policies/client_side_weighted_round_robin:config",
        "//source/extensions/load_balancing_policies/orca_locality:config",
        "//source/extensions/load_balancing_policies/round_robin:config",
        "//test/mocks/server:factory_context_mocks",
        "//test/mocks/upstream:cluster_info_mocks",
        "//test/mocks/upstream:priority_set_mocks",
        "@envoy_api//envoy/config/core/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/load_balancing_policies/orca_locality/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/load_balancing_policies/round_robin/v3:pkg_cc_proto",
    ],
)
