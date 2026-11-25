// SPDX-License-Identifier: GPL-3.0-or-later
#include "plr_backend.h"

#include "json_utils.h"
#include "moonraker_subscription_merge.h"
#include "printer_discovery.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <array>

#include "hv/json.hpp"

namespace helix {

namespace {

/// Characters that would end the gcode command or break out of the quoted
/// `FILENAME="<f>"` value. Klipper tokenizes extended parameters with `shlex`
/// in POSIX mode, so `"` closes the value early and `\` is an escape character;
/// `;`/`#` are shlex comment introducers and `*` is a checksum marker, all of
/// which older regex-path forks strip before shlex ever runs. Spaces are NOT
/// here — that is exactly what the quoting buys us.
constexpr const char* kGcodeParamBreakers = "\n\r;#*=\"\\";

using json = nlohmann::json;

/// One recovery backend: which status fields it reads on a discovered printer,
/// which markers select it, and its availability and recovery-file rules.
/// The table order is the selection precedence.
struct PlrProvider {
    PlrBackendType backend;
    const char* name;
    /// Subscription objects map; an empty object when this printer cannot
    /// carry the backend.
    json (*status_fields)(const PrinterDiscovery& hw);
    bool (*selects)(const PlrCapabilitySignals& caps);
    bool (*available)(const PlrCapabilitySignals& caps, const PlrProbeSession& probe);
    std::string (*recovery_file)(const PlrCapabilitySignals& caps, const PlrProbeSession& probe);
};

json no_status_fields(const PrinterDiscovery& /*hw*/) {
    return json::object();
}

bool plugin_ready_with(const PlrCapabilitySignals& caps, PlrPluginBackend backend) {
    return caps.plr_feature == PluginFeatureState::Ready && caps.plugin_backend == backend;
}

bool plugin_offers(const PlrCapabilitySignals& caps, const PlrProbeSession& /*probe*/) {
    // The plugin decided availability itself: an interrupted newest job, a
    // snapshot for it, an unchanged file, Klipper ready.
    return caps.plugin_offer_available;
}

std::string plugin_recovery_file(const PlrCapabilitySignals& caps,
                                 const PlrProbeSession& /*probe*/) {
    return caps.plugin_offer_available ? caps.plugin_recovery_file : std::string();
}

const std::array<PlrProvider, 1>& providers() {
    static const std::array<PlrProvider, 1> kProviders = {{
        // The plugin's own snapshot and resume, last of all, so it never
        // displaces a firmware's own recovery.
        {PlrBackendType::GENERIC, "plugin", no_status_fields,
         [](const PlrCapabilitySignals& caps) {
             return plugin_ready_with(caps, PlrPluginBackend::Generic);
         },
         plugin_offers, plugin_recovery_file},
    }};
    return kProviders;
}

const PlrProvider* select_provider(const PlrCapabilitySignals& caps) {
    for (const PlrProvider& p : providers()) {
        if (p.selects(caps)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

PlrBackendType plr_select_backend(const PlrCapabilitySignals& caps) {
    // Passive markers first: choosing them never costs a side-effectful probe.
    // The markers come from different firmware forks and should never coexist,
    // but the table order makes the tie deterministic.
    const PlrProvider* p = select_provider(caps);
    return p != nullptr ? p->backend : PlrBackendType::NONE;
}

PlrOfferAvailability plr_offer_availability(const PlrCapabilitySignals& caps,
                                            const PlrProbeSession& probe) {
    PlrOfferAvailability out;
    const PlrProvider* p = select_provider(caps);
    if (p == nullptr) {
        return out;
    }
    out.backend = p->backend;
    out.available = p->available(caps, probe);
    out.recovery_file = p->recovery_file(caps, probe);
    return out;
}

const char* plr_backend_name(PlrBackendType backend) {
    switch (backend) {
    case PlrBackendType::GENERIC:
        return "plugin";
    default:
        break;
    }
    return "none";
}

bool plr_reoffers_after_job_drop(PlrBackendType backend) {
    // The plugin offers only when Moonraker's history calls the newest job
    // interrupted. The firmware flags can lag the standby frame of a print that
    // finished, so they never offer from this edge.
    return backend == PlrBackendType::GENERIC;
}

bool plr_resume_macro_present(const PrinterDiscovery& /*unused*/) {
    return false;
}

nlohmann::json plr_required_status_fields(const PrinterDiscovery& hw) {
    json fields = json::object();
    for (const PlrProvider& p : providers()) {
        fields = merge_subscription_objects(fields, p.status_fields(hw));
    }
    return fields;
}

int plr_parse_snapshot_valid(const nlohmann::json& /*unused*/) {
    return -1;
}

const std::string* plr_parse_recovery_file(const nlohmann::json& status) {
    const json* sdcard = json_util::find_member(status, "virtual_sdcard");
    if (sdcard == nullptr) {
        return nullptr;
    }
    const json* path = json_util::find_member(*sdcard, "file_path");
    if (path == nullptr || !path->is_string()) {
        return nullptr;
    }
    return &path->get_ref<const std::string&>();
}

int plr_parse_interrupted_flag(const nlohmann::json& /*unused*/) {
    return -1;
}

bool plr_parse_power_loss_signal(const nlohmann::json& /*unused*/) {
    return false;
}

bool plr_probe_recovery_available(const PlrDetectResult& r) {
    // `completed` is load-bearing, not a redundancy check: it certifies that the
    // probe ran this connection, which is what set print_stats.power_loss=1 in
    // firmware. See PlrDetectResult and docs/devel/POWER_LOSS_RECOVERY.md.
    return r.completed && r.file_state && r.eeprom_state;
}

bool plr_parse_check_continue_response(const nlohmann::json& /*response*/,
                                       PlrDetectResult& /*out*/) {
    return false;
}

bool plr_is_safe_recovery_filename(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    return name.find_first_of(kGcodeParamBreakers) == std::string::npos;
}

PlrRecoveryPlan plr_build_plan(PlrBackendType backend, const std::string& recovery_file,
                               const PlrDetectResult& /*detect*/) {
    PlrRecoveryPlan plan;
    plan.backend = backend;
    plan.recovery_file = recovery_file;

    switch (backend) {
    case PlrBackendType::GENERIC:
        // The plugin renders and sends the resume itself, step by step, after
        // the wizard's confirmations; the app only opens that flow. Discard
        // deletes the plugin's snapshot and touches no motion.
        plan.resume_rpc = kPlrPlanRpc;
        plan.discard_rpc_method = kPlrDiscardRpc;
        break;

    default:
        break;
    }
    return plan;
}

std::string plr_read_probe_recovery_filename() {
    return {};
}

} // namespace helix
