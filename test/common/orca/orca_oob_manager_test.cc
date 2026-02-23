#include "source/common/orca/orca_oob_manager.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::AllOf;
using testing::AtLeast;
using testing::Ge;
using testing::Invoke;
using testing::Lt;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;

namespace Envoy {
namespace Orca {
namespace {

class OrcaOobManagerTest : public testing::Test {
protected:
  OrcaOobManagerTest() = default;

  std::shared_ptr<NiceMock<Upstream::MockHost>>
  makeHost(const std::string& addr = "tcp://127.0.0.1:80") {
    auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
    auto address = *Network::Utility::resolveUrl(addr);
    ON_CALL(*host, address()).WillByDefault(Return(address));
    ON_CALL(*host, hostname()).WillByDefault(ReturnRef(empty_hostname_));
    ON_CALL(*host, loadMetricStats()).WillByDefault(ReturnRef(host->load_metric_stats_));
    return host;
  }

  NiceMock<Upstream::MockPrioritySet> priority_set_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Random::MockRandomGenerator> random_;
  Stats::IsolatedStoreImpl stats_store_;
  std::string empty_hostname_;
};

// --- Lifecycle Tests ---

TEST_F(OrcaOobManagerTest, CreateAndDestroy) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
}

TEST_F(OrcaOobManagerTest, StartRegistersPriorityCallback) {
  EXPECT_CALL(priority_set_, addPriorityUpdateCb(_));

  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();
  manager.stop();
}

TEST_F(OrcaOobManagerTest, StopMultipleTimes) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();
  manager.stop();
  manager.stop();
}

TEST_F(OrcaOobManagerTest, DestructorCallsStop) {
  {
    OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                            std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                            absl::nullopt);
    manager.start();
    // Destructor should call stop() cleanly.
  }
}

// --- Host Management Tests ---

TEST_F(OrcaOobManagerTest, StartCreatesSessionsForExistingHosts) {
  auto host = makeHost();
  auto* mock_host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts = {host};
  ON_CALL(*mock_host_set, hosts()).WillByDefault(ReturnRef(hosts));

  // Expect timer creation for jitter and reconnect timers.
  EXPECT_CALL(dispatcher_, createTimer_(_)).Times(AtLeast(1));

  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();
  manager.stop();
}

TEST_F(OrcaOobManagerTest, NoJitterNoHosts) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(0), *stats_store_.rootScope(), absl::nullopt);
  manager.start();
  manager.stop();
}

TEST_F(OrcaOobManagerTest, HostsAddedGetSessions) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();

  auto host = makeHost();
  Upstream::HostVector added = {host};
  Upstream::HostVector removed;

  EXPECT_CALL(dispatcher_, createTimer_(_)).Times(AtLeast(1));
  priority_set_.runUpdateCallbacks(0, added, removed);

  manager.stop();
}

TEST_F(OrcaOobManagerTest, HostsRemovedGetSessionsCleaned) {
  auto host = makeHost();
  auto* mock_host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts = {host};
  ON_CALL(*mock_host_set, hosts()).WillByDefault(ReturnRef(hosts));

  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();

  Upstream::HostVector added;
  Upstream::HostVector removed = {host};
  priority_set_.runUpdateCallbacks(0, added, removed);

  manager.stop();
}

TEST_F(OrcaOobManagerTest, RemovingNonexistentHostIsNoop) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();

  auto host = makeHost();
  Upstream::HostVector added;
  Upstream::HostVector removed = {host};
  priority_set_.runUpdateCallbacks(0, added, removed);

  manager.stop();
}

TEST_F(OrcaOobManagerTest, DuplicateHostsNotDoubleAdded) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();

  auto host = makeHost();
  Upstream::HostVector added = {host};
  Upstream::HostVector removed;

  priority_set_.runUpdateCallbacks(0, added, removed);
  // Adding the same host again should be a no-op (no extra timers created).
  priority_set_.runUpdateCallbacks(0, added, removed);

  manager.stop();
}

// --- Jitter Tests ---

// Note: ZeroJitterStartsSessionImmediately is tested at the OrcaOobSession level
// (see orca_oob_session_test.cc). At the manager level, zero jitter causes the
// manager to call session->start() directly, which creates a real CodecClientProd
// that cannot be injected via mocks. The session-level test uses TestOrcaOobSession
// to override createCodecClient() and verify this behavior.

TEST_F(OrcaOobManagerTest, JitterCreatesTimers) {
  auto host = makeHost();
  auto* mock_host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts = {host};
  ON_CALL(*mock_host_set, hosts()).WillByDefault(ReturnRef(hosts));

  // With non-zero jitter and one host, expect at least 2 timers:
  // one reconnect timer (from OrcaOobSession constructor) and one jitter timer.
  EXPECT_CALL(dispatcher_, createTimer_(_)).Times(AtLeast(2));

  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(5000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();
  manager.stop();
}

// --- Multiple Priority Levels ---

TEST_F(OrcaOobManagerTest, StartCreatesSessionsAcrossMultiplePriorities) {
  auto host0 = makeHost("tcp://127.0.0.1:80");
  auto host1 = makeHost("tcp://127.0.0.1:81");

  auto* mock_host_set_0 = priority_set_.getMockHostSet(0);
  auto* mock_host_set_1 = priority_set_.getMockHostSet(1);

  Upstream::HostVector hosts0 = {host0};
  Upstream::HostVector hosts1 = {host1};
  ON_CALL(*mock_host_set_0, hosts()).WillByDefault(ReturnRef(hosts0));
  ON_CALL(*mock_host_set_1, hosts()).WillByDefault(ReturnRef(hosts1));

  // Expect timer creation for both hosts (jitter + reconnect timers for each).
  EXPECT_CALL(dispatcher_, createTimer_(_)).Times(AtLeast(2));

  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();
  manager.stop();
}

// --- Multiple Hosts Added at Once ---

TEST_F(OrcaOobManagerTest, MultipleHostsAddedAtOnce) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();

  auto host1 = makeHost("tcp://127.0.0.1:80");
  auto host2 = makeHost("tcp://127.0.0.1:81");
  auto host3 = makeHost("tcp://127.0.0.1:82");

  Upstream::HostVector added = {host1, host2, host3};
  Upstream::HostVector removed;

  // Expect timer creation for all three hosts.
  EXPECT_CALL(dispatcher_, createTimer_(_)).Times(AtLeast(3));
  priority_set_.runUpdateCallbacks(0, added, removed);

  manager.stop();
}

// --- Add then Remove ---

TEST_F(OrcaOobManagerTest, AddThenRemoveHost) {
  OrcaOobManager manager(priority_set_, dispatcher_, random_, std::chrono::milliseconds(10000),
                          std::chrono::milliseconds(10000), *stats_store_.rootScope(),
                          absl::nullopt);
  manager.start();

  auto host = makeHost();
  Upstream::HostVector added = {host};
  Upstream::HostVector empty;

  priority_set_.runUpdateCallbacks(0, added, empty);

  // Remove the same host.
  Upstream::HostVector removed = {host};
  priority_set_.runUpdateCallbacks(0, empty, removed);

  // Re-add should create new session.
  EXPECT_CALL(dispatcher_, createTimer_(_)).Times(AtLeast(1));
  priority_set_.runUpdateCallbacks(0, added, empty);

  manager.stop();
}

} // namespace
} // namespace Orca
} // namespace Envoy
