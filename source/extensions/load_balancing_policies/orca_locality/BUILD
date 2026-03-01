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
        ":orca_locality_lb_lib",
        "//source/common/common:minimal_logger_lib",
        "//source/common/config:utility_lib",
        "//source/common/upstream:load_balancer_context_base_lib",
        "//source/extensions/load_balancing_policies/common:factory_base",
        "@envoy_api//envoy/extensions/load_balancing_policies/orca_locality/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "orca_locality_lb_lib",
    srcs = [
        "orca_locality_lb.cc",
        "orca_locality_manager.cc",
    ],
    hdrs = [
        "orca_locality_lb.h",
        "orca_locality_manager.h",
    ],
    deps = [
        "//envoy/thread_local:thread_local_interface",
        "//source/common/upstream:upstream_lib",
        "//source/extensions/load_balancing_policies/common:load_balancer_lib",
        "//source/extensions/load_balancing_policies/common:orca_weight_manager_lib",
        "@envoy_api//envoy/extensions/load_balancing_policies/orca_locality/v3:pkg_cc_proto",
    ],
)
