#include "network/server/session_manager.hpp"
#include "network/server/user_session.hpp"

namespace run::server::session
{

void SessionManager::AddSession(uint64_t user_id, SessionPtr session)
{
    std::unique_lock lock(m_mutex);
    m_user_sessions[user_id].insert(std::move(session));
}

void SessionManager::UpdateViewportSubscription(SessionPtr session, const std::vector<uint64_t>& new_h3_zones)
{
    std::unique_lock lock(m_mutex);
    SessionRawPtr raw_ptr = session.get();

    std::unordered_set<uint64_t> new_zones_set(new_h3_zones.begin(), new_h3_zones.end());
    auto& current_zones = m_session_zones[raw_ptr];

    for (uint64_t old_zone : current_zones)
    {
        if (!new_zones_set.contains(old_zone))
        {
            if (auto it = m_zone_subscribers.find(old_zone); it != m_zone_subscribers.end())
            {
                it->second.erase(session);
                if (it->second.empty())
                {
                    m_zone_subscribers.erase(it);
                }
            }
        }
    }

    for (uint64_t new_zone : new_zones_set)
    {
        if (!current_zones.contains(new_zone))
        {
            m_zone_subscribers[new_zone].insert(session);
        }
    }

    current_zones = std::move(new_zones_set);
}

void SessionManager::RemoveSession(uint64_t user_id, SessionRawPtr raw_ptr)
{
    std::unique_lock lock(m_mutex);

    if (auto it = m_session_zones.find(raw_ptr); it != m_session_zones.end())
    {
        for (uint64_t zone : it->second)
        {
            if (auto zone_it = m_zone_subscribers.find(zone); zone_it != m_zone_subscribers.end())
            {
                std::erase_if(zone_it->second, [raw_ptr](const SessionPtr& s) { return s.get() == raw_ptr; });

                if (zone_it->second.empty())
                {
                    m_zone_subscribers.erase(zone_it);
                }
            }
        }
        m_session_zones.erase(it);
    }

    if (auto it = m_user_sessions.find(user_id); it != m_user_sessions.end())
    {
        std::erase_if(it->second, [raw_ptr](const SessionPtr& s) { return s.get() == raw_ptr; });

        if (it->second.empty())
        {
            m_user_sessions.erase(it);
        }
    }
}

void SessionManager::BroadcastToZone(uint64_t h3_zone, const std::string& payload)
{
    std::vector<SessionPtr> target_sessions;
    {
        std::shared_lock lock(m_mutex);
        if (auto it = m_zone_subscribers.find(h3_zone); it != m_zone_subscribers.end())
        {
            target_sessions.reserve(it->second.size());
            for (const auto& session : it->second)
            {
                target_sessions.push_back(session);
            }
        }
    }

    // Send() is thread-safe (concurrent channel) and never blocks.
    for (const auto& session : target_sessions)
    {
        session->Send(payload);
    }
}

void SessionManager::SendToUser(uint64_t user_id, const std::string& payload)
{
    std::vector<SessionPtr> target_sessions;
    {
        std::shared_lock lock(m_mutex);
        if (auto it = m_user_sessions.find(user_id); it != m_user_sessions.end())
        {
            target_sessions.reserve(it->second.size());
            for (const auto& session : it->second)
            {
                target_sessions.push_back(session);
            }
        }
    }

    for (const auto& session : target_sessions)
    {
        session->Send(payload);
    }
}

void SessionManager::CloseAllSessions()
{
    std::vector<SessionPtr> sessions;
    {
        std::shared_lock lock(m_mutex);
        for (const auto& [user_id, user_sessions] : m_user_sessions)
        {
            sessions.insert(sessions.end(), user_sessions.begin(), user_sessions.end());
        }
    }

    for (const auto& session : sessions)
    {
        session->RequestClose();
    }
}

} // namespace run::server::session