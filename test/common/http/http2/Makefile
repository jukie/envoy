# HTTP/2 Codec Benchmark: oghttp2 vs nghttp2
#
# Usage:
#   make build       # build the benchmark binary (-c opt)
#   make quick       # round-trip only, ~15s — fast feedback loop
#   make all         # run all benchmarks, save results to JSON
#   make compare     # show nghttp2 vs oghttp2 comparison table
#   make raw         # show raw benchmark output from results.json
#
# Context: https://github.com/envoyproxy/envoy/issues/40070

BAZEL_TARGET := //test/common/http/http2:codec_speed_test
BAZEL_FLAGS  := -c opt
RESULTS_DIR  := /tmp/h2-codec-bench
RESULTS_JSON := $(RESULTS_DIR)/results.json
MIN_TIME     ?= 1s
BENCH_FLAGS  := --benchmark_min_time=$(MIN_TIME)

.PHONY: build all quick compare raw clean help

help: ## show this help
	@grep -E '^[a-z][a-z_-]+:.*## ' $(MAKEFILE_LIST) | \
		awk -F ':.*## ' '{printf "  %-14s %s\n", $$1, $$2}'

build: ## build the benchmark binary (optimized)
	bazel build $(BAZEL_FLAGS) $(BAZEL_TARGET)

all: build $(RESULTS_DIR) ## run all benchmarks, save results to JSON
	bazel run $(BAZEL_FLAGS) $(BAZEL_TARGET) -- \
		--benchmark_out=$(RESULTS_JSON) --benchmark_out_format=json \
		$(BENCH_FLAGS)
	@echo ""
	@echo "Run 'make compare' to see the comparison table."

quick: build $(RESULTS_DIR) ## round-trip only (~15s), saves JSON
	bazel run $(BAZEL_FLAGS) $(BAZEL_TARGET) -- \
		--benchmark_filter='benchmarkRoundTrip' --benchmark_min_time=0.5s \
		--benchmark_out=$(RESULTS_JSON) --benchmark_out_format=json

$(RESULTS_DIR):
	mkdir -p $(RESULTS_DIR)

COMPARE_JQ := $(dir $(lastword $(MAKEFILE_LIST)))compare.jq

compare: ## show nghttp2 vs oghttp2 comparison table
	@test -f $(RESULTS_JSON) || { echo "No results. Run 'make all' or 'make quick' first."; exit 1; }
	@jq -rf $(COMPARE_JQ) $(RESULTS_JSON)

raw: ## show raw benchmark output from results.json
	@test -f $(RESULTS_JSON) || { echo "No results. Run 'make all' or 'make quick' first."; exit 1; }
	@jq -r '.benchmarks[] | "\(.name)\t\(.real_time | .*10|round/10) us\t\(.cpu_time | .*10|round/10) us\t\(.iterations)"' $(RESULTS_JSON) | \
		(echo "Benchmark\tTime\tCPU\tIterations" && cat) | column -t -s'	'

clean: ## remove saved results
	rm -rf $(RESULTS_DIR)
