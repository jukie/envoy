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
    name = "brotli_decompressor_impl_test",
    srcs = ["brotli_decompressor_impl_test.cc"],
    extension_names = ["envoy.compression.brotli.decompressor"],
    rbe_pool = "6gig",
    deps = [
        "//source/common/stats:isolated_store_lib",
        "//source/extensions/compression/brotli/compressor:compressor_lib",
        "//source/extensions/compression/brotli/decompressor:config",
        "//test/mocks/server:factory_context_mocks",
        "//test/test_common:utility_lib",
    ],
)
