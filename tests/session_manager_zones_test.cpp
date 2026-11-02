// Zone routing index of SessionManager: full-set subscription semantics
// (idempotent replace), delivery scoped to the subscribed zones, clean
// removal. This index is what carries radar pings and hexagon capture
// events to viewer sessions.

#include "network/server/session_manager.hpp"
#include "network/server/user_session.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace
{

using run::server::session::SessionManager;
using run::server::session::UserSession;

class FakeSession final : public UserSession
{
public:
    explicit FakeSession(uint64_t id) : m_id(id)
    {
    }

    net::any_io_executor GetExecutor() override
    {
        return net::any_io_executor();
    }

    uint64_t GetId() const noexcept override
    {
        return m_id;
    }

    void Send(std::string bytes) override
    {
        sent_count.fetch_add(1, std::memory_order_relaxed);
        last_payload = std::move(bytes);
    }

    void SendProtobuf(const run::proto::Envelope& envelope) override
    {
        Send(envelope.SerializeAsString());
    }

    void RequestClose(int = 1000) override
    {
        closed = true;
    }

    void Close() override
    {
    }

    void UpdateSubscribedZones(const std::vector<uint64_t>& zones) override
    {
        m_subscribed_zones.clear();
        m_subscribed_zones.insert(zones.begin(), zones.end());
    }

    const std::unordered_set<uint64_t>& GetSubscribedZones() const noexcept override
    {
        return m_subscribed_zones;
    }

    uint64_t GetActiveRunId() const noexcept override
    {
        return 0;
    }

    void SetActiveRunId(uint64_t) noexcept override
    {
    }

    std::atomic<int> sent_count{0};
    std::atomic<bool> closed{false};
    std::string last_payload;

private:
    uint64_t m_id;
    int64_t m_last_frame_ms{0};
    std::unordered_set<uint64_t> m_subscribed_zones;
};

constexpr uint64_t kZoneA = 0x8611a0000000001ull;
constexpr uint64_t kZoneB = 0x8611b0000000002ull;

} // namespace

TEST(SessionManagerZones, BroadcastReachesOnlySubscribedZone)
{
    SessionManager manager;
    auto viewer_a = std::make_shared<FakeSession>(1);
    auto viewer_b = std::make_shared<FakeSession>(2);

    manager.AddSession(1, viewer_a);
    manager.AddSession(2, viewer_b);
    manager.UpdateViewportSubscription(viewer_a, {kZoneA});
    manager.UpdateViewportSubscription(viewer_b, {kZoneB});

    manager.BroadcastToZone(kZoneA, "ping");

    EXPECT_EQ(viewer_a->sent_count.load(), 1);
    EXPECT_EQ(viewer_b->sent_count.load(), 0);
}

TEST(SessionManagerZones, FullSetReplaceMovesSubscription)
{
    SessionManager manager;
    auto viewer = std::make_shared<FakeSession>(1);
    manager.AddSession(1, viewer);
    manager.UpdateViewportSubscription(viewer, {kZoneA});

    // Idempotent full-set semantics: the same zone again is a no-op.
    manager.UpdateViewportSubscription(viewer, {kZoneA});
    manager.BroadcastToZone(kZoneA, "one");
    EXPECT_EQ(viewer->sent_count.load(), 1);

    // Moving to another zone unsubscribes the old one.
    manager.UpdateViewportSubscription(viewer, {kZoneB});
    manager.BroadcastToZone(kZoneA, "old");
    EXPECT_EQ(viewer->sent_count.load(), 1);
    manager.BroadcastToZone(kZoneB, "new");
    EXPECT_EQ(viewer->sent_count.load(), 2);
}

TEST(SessionManagerZones, MultiZoneAndRemoval)
{
    SessionManager manager;
    auto viewer = std::make_shared<FakeSession>(1);
    manager.AddSession(1, viewer);
    manager.UpdateViewportSubscription(viewer, {kZoneA, kZoneB});

    manager.BroadcastToZone(kZoneA, "a");
    manager.BroadcastToZone(kZoneA, "a2");
    manager.BroadcastToZone(kZoneB, "b");
    EXPECT_EQ(viewer->sent_count.load(), 3);

    manager.RemoveSession(1, viewer.get());
    manager.BroadcastToZone(kZoneA, "after-remove");
    manager.BroadcastToZone(kZoneB, "after-remove");
    EXPECT_EQ(viewer->sent_count.load(), 3); // no delivery after removal
    EXPECT_EQ(manager.GetActiveSessionsCount(), 0);
}