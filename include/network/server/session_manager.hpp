#pragma once

#include <iostream>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "common.hpp"

namespace run::server::session
{

class UserSession;

/**
 * @brief Manages active network user sessions
 */
class SessionManager : public std::enable_shared_from_this<SessionManager>
{
private:
    using SessionPtr = std::shared_ptr<session::UserSession>;
    using SessionRawPtr = UserSession*;

    // h3_parent_index (res=6) -> Sessions
    std::unordered_map<uint64_t, std::unordered_set<SessionPtr>> m_zone_subscribers;

    // user_id -> Sessions
    std::unordered_map<uint64_t, std::unordered_set<SessionPtr>> m_user_sessions;

    // Sessions -> h3_parent_index
    std::unordered_map<SessionRawPtr, std::unordered_set<uint64_t>> m_session_zones;

    mutable std::shared_mutex m_mutex;

public:
    SessionManager() = default;
    ~SessionManager() = default;

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    void AddSession(uint64_t user_id, SessionPtr session);
    void RemoveSession(uint64_t user_id, SessionRawPtr raw_ptr);

    void UpdateViewportSubscription(SessionPtr session, const std::vector<uint64_t>& new_h3_zones);

    void BroadcastToZone(uint64_t zone_h3_idx, const std::string& binary_envelope);
    void SendToUser(uint64_t user_id, const std::string& payload);

    /// Graceful shutdown: flush + 1001 Going Away on every live session.
    void CloseAllSessions();
};

} // namespace run::server::session