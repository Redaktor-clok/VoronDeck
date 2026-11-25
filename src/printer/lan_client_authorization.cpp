// SPDX-License-Identifier: GPL-3.0-or-later

#include "lan_client_authorization.h"

#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <array>
#include <iterator>

namespace helix::lan_auth {
namespace {

/// One firmware that brokers LAN client authorization.
struct Provider {
    const char* name;
    /// Moonraker notification the firmware broadcasts to the screen.
    const char* notification;
    /// JSON-RPC method that carries the screen's answer back.
    const char* decision_method;
    /// Pull a request out of the notification's params payload.
    std::optional<PendingRequest> (*parse)(const nlohmann::json& params);
    /// Build the decision params for a request this provider raised.
    nlohmann::json (*decision_params)(const PendingRequest& req, bool approve);
};

/// The notification's payload object, or nullptr.
///
/// Moonraker wraps every notification's event args in an array, so the object
/// is params[0] and not params itself.
const nlohmann::json* notification_payload(const nlohmann::json& msg) {
    if (!msg.is_object()) {
        return nullptr;
    }
    auto params = msg.find("params");
    if (params == msg.end() || !params->is_array() || params->empty()) {
        return nullptr;
    }
    const nlohmann::json& payload = (*params)[0];
    return payload.is_object() ? &payload : nullptr;
}

constexpr std::array<Provider, 0> kProviders{};

const Provider* provider_for_notification(const std::string& method) {
    for (const Provider& p : kProviders) {
        if (method == p.notification) {
            return &p;
        }
    }
    return nullptr;
}

const Provider* provider_by_name(const std::string& name) {
    for (const Provider& p : kProviders) {
        if (name == p.name) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

const std::vector<std::string>& notification_methods() {
    static const std::vector<std::string> methods = [] {
        std::vector<std::string> m;
        m.reserve(std::size(kProviders));
        for (const Provider& p : kProviders) {
            m.emplace_back(p.notification);
        }
        return m;
    }();
    return methods;
}

std::optional<PendingRequest> parse_request(const std::string& method, const nlohmann::json& msg) {
    const Provider* provider = provider_for_notification(method);
    if (!provider) {
        return std::nullopt;
    }
    const nlohmann::json* payload = notification_payload(msg);
    if (!payload) {
        spdlog::warn("[LanAuth] {} notification carried no payload object", provider->name);
        return std::nullopt;
    }
    std::optional<PendingRequest> req = provider->parse(*payload);
    if (!req) {
        spdlog::warn("[LanAuth] {} authorization request was malformed, ignoring", provider->name);
        return std::nullopt;
    }
    req->provider = provider->name;
    return req;
}

std::optional<Decision> build_decision(const PendingRequest& req, bool approve) {
    const Provider* provider = provider_by_name(req.provider);
    if (!provider) {
        return std::nullopt;
    }
    return Decision{provider->decision_method, provider->decision_params(req, approve)};
}

bool suppressed_by_denial(
    const std::string& client_id,
    const std::unordered_map<std::string, std::chrono::steady_clock::time_point>& denied_clients,
    std::chrono::steady_clock::time_point now) {
    auto it = denied_clients.find(client_id);
    if (it == denied_clients.end()) {
        return false;
    }
    return now - it->second < denial_suppression_window;
}

} // namespace helix::lan_auth
