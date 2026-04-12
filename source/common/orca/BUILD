load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_library(
    name = "orca_parser",
    srcs = ["orca_parser.cc"],
    hdrs = ["orca_parser.h"],
    deps = [
        "//envoy/common:exception_lib",
        "//envoy/http:header_map_interface",
        "//source/common/common:base64_lib",
        "//source/common/http:header_utility_lib",
        "//source/common/protobuf:utility_lib_header",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@fmt",
        "@xds//xds/data/orca/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "orca_load_metrics_lib",
    srcs = ["orca_load_metrics.cc"],
    hdrs = ["orca_load_metrics.h"],
    deps = [
        ":orca_parser",
        "//envoy/http:header_map_interface",
        "//source/common/http:header_utility_lib",
        "//source/common/protobuf:utility_lib_header",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@fmt",
        "@xds//xds/data/orca/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "orca_oob_client_lib",
    srcs = ["orca_oob_client.cc"],
    hdrs = ["orca_oob_client.h"],
    deps = [
        "//envoy/common:backoff_strategy_interface",
        "//envoy/common:random_generator_interface",
        "//envoy/config:subscription_factory_interface",
        "//envoy/event:dispatcher_interface",
        "//envoy/event:timer_interface",
        "//envoy/grpc:async_client_interface",
        "//envoy/stats:stats_interface",
        "//envoy/stats:stats_macros",
        "//envoy/upstream:load_balancer_interface",
        "//source/common/common:assert_lib",
        "//source/common/common:backoff_lib",
        "//source/common/common:logger_lib",
        "//source/common/grpc:common_lib",
        "//source/common/grpc:typed_async_client_lib",
        "//source/common/protobuf:utility_lib_header",
        "@abseil-cpp//absl/strings",
        "@xds//xds/data/orca/v3:pkg_cc_proto",
        "@xds//xds/service/orca/v3:pkg_cc_proto",
    ],
)
