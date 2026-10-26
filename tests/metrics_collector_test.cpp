#include <gtest/gtest.h>
#include "monitoring/metrics_collector.hpp"
#include "network/server/session_manager.hpp"
#include "network/server/user_session.hpp"

#include <boost/asio/io_context.hpp>

namespace
{

class MockUserSession : public run::server::session::UserSession
{
private:
    uint64_t m_id;
    uint64_t m_run_id{0};
    std::unordered_set<uint64_t> m_zones;
    boost::asio::io_context m_ioc;
    bool m_closed{false};

public:
    explicit MockUserSession(uint64_t id) : m_id(id) {}

    boost::asio::any_io_executor GetExecutor() override
    {
        return m_ioc.get_executor();
    }

    uint64_t GetId() const noexcept override
    {
        return m_id;
    }

    void Send(std::string) override {}
    void SendProtobuf(const run::proto::Envelope&) override {}
    void RequestClose() override { m_closed = true; }
    void Close() override { m_closed = true; }

    void UpdateSubscribedZones(const std::vector<uint64_t>& zones) override
    {
        m_zones.clear();
        m_zones.insert(zones.begin(), zones.end());
    }

    const std::unordered_set<uint64_t>& GetSubscribedZones() const noexcept override
    {
        return m_zones;
    }

    uint64_t GetActiveRunId() const noexcept override { return m_run_id; }
    void SetActiveRunId(uint64_t run_id) noexcept override { m_run_id = run_id; }
    bool IsClosed() const noexcept { return m_closed; }
};

} // namespace

TEST(MetricsCollectorTest, ActiveSocketsGaugeUpdatesWithSessionManager)
{
    auto session_manager = std::make_shared<run::server::session::SessionManager>();

    auto s1 = std::make_shared<MockUserSession>(101);
    auto s2 = std::make_shared<MockUserSession>(102);

    session_manager->AddSession(101, s1);
    EXPECT_EQ(session_manager->GetActiveSessionsCount(), 1);

    std::string text = run::monitoring::MetricsCollector::Instance().Collect();
    EXPECT_NE(text.find("run_active_sockets 1"), std::string::npos);

    session_manager->AddSession(102, s2);
    EXPECT_EQ(session_manager->GetActiveSessionsCount(), 2);

    text = run::monitoring::MetricsCollector::Instance().Collect();
    EXPECT_NE(text.find("run_active_sockets 2"), std::string::npos);

    session_manager->RemoveSession(101, s1.get());
    EXPECT_EQ(session_manager->GetActiveSessionsCount(), 1);

    text = run::monitoring::MetricsCollector::Instance().Collect();
    EXPECT_NE(text.find("run_active_sockets 1"), std::string::npos);

    session_manager->CloseAllSessions();
    EXPECT_EQ(session_manager->GetActiveSessionsCount(), 0);

    text = run::monitoring::MetricsCollector::Instance().Collect();
    EXPECT_NE(text.find("run_active_sockets 0"), std::string::npos);
}

TEST(MetricsCollectorTest, NetworkMetricsCollection)
{
    auto& collector = run::monitoring::MetricsCollector::Instance();

    collector.IncrementConnectionsAccepted(false);
    collector.IncrementConnectionsAccepted(true);
    collector.IncrementHttpRequests("GET", "/metrics", 200);
    collector.IncrementWsEnvelopes("LocationBatch");
    collector.ObserveWsDispatchDuration(150.0);

    std::string text = collector.Collect();

    EXPECT_NE(text.find("run_connections_accepted_total"), std::string::npos);
    EXPECT_NE(text.find("run_http_requests_total"), std::string::npos);
    EXPECT_NE(text.find("run_ws_envelopes_total"), std::string::npos);
    EXPECT_NE(text.find("run_ws_dispatch_duration_microseconds"), std::string::npos);
}
