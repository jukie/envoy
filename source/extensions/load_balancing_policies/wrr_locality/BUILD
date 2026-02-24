load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        ":wrr_locality_lb_lib",
        "//source/common/protobuf:utility_lib",
        "//source/extensions/load_balancing_policies/client_side_weighted_round_robin:config",
        "//source/extensions/load_balancing_policies/common:factory_base",
        "@envoy_api//envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "wrr_locality_lb_lib",
    srcs = ["wrr_locality_lb.cc"],
    hdrs = ["wrr_locality_lb.h"],
    deps = [
        "//envoy/thread_local:thread_local_interface",
        "//source/common/common:callback_impl_lib",
        "//source/common/protobuf:utility_lib",
        "//source/extensions/load_balancing_policies/common:factory_base",
        "//source/extensions/load_balancing_policies/common:orca_weight_manager_lib",
        "//source/extensions/load_balancing_policies/round_robin:round_robin_lb_lib",
        "@envoy_api//envoy/extensions/load_balancing_policies/round_robin/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/load_balancing_policies/wrr_locality/v3:pkg_cc_proto",
    ],
)
