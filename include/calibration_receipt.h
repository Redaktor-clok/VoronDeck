// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "observer_factory.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <limits>
#include <string>

namespace helix {

// A UI-thread receipt for an explicit attempt, never a cached result screen.
// Tickets captured by asynchronous callbacks cannot complete a later attempt.
class CalibrationReceipt {
  public:
    enum class Phase { Idle, Running, Succeeded, Cancelled };
    struct Ticket {
        uint64_t attempt = 0;
        std::string printer;
    };
    struct Event {
        Ticket ticket;
        Phase phase = Phase::Idle;
    };

    Ticket begin(const std::string& printer) {
        event_ = {{++generation_, printer}, Phase::Running};
        notify();
        return event_.ticket;
    }
    Ticket ticket() const {
        return event_.ticket;
    }
    bool current(const Ticket& ticket, const std::string& printer) const {
        return event_.phase == Phase::Running && ticket.attempt != 0 &&
               ticket.attempt == event_.ticket.attempt && ticket.printer == event_.ticket.printer &&
               ticket.printer == printer;
    }
    bool succeed(const Ticket& ticket, const std::string& printer) {
        if (!current(ticket, printer)) {
            if (ticket.attempt == event_.ticket.attempt && ticket.printer != printer)
                cancel();
            return false;
        }
        event_.phase = Phase::Succeeded;
        notify();
        return true;
    }
    void cancel() {
        if (event_.phase != Phase::Running)
            return;
        event_.phase = Phase::Cancelled;
        notify();
    }
    void reset() {
        ++generation_;
        event_ = {};
        notify();
    }
    void deinit() {
        ++generation_;
        event_ = {};
        subjects_.deinit_all();
        initialized_ = false;
    }
    const Event& event() const {
        return event_;
    }

    template <typename Owner, typename Handler>
    ObserverGuard observe(Owner* owner, Handler handler) const {
        initialize();
        return ui::observe<int>(
            &changed_, owner, [this, handler](Owner* self, int) mutable { handler(self, event_); },
            subjects_.get_subjects_lifetime(), ui::Dispatch::Immediate);
    }

  private:
    void initialize() const {
        if (initialized_)
            return;
        lv_subject_init_int(&changed_, 0);
        subjects_.register_subject(&changed_);
        initialized_ = true;
    }
    void notify() {
        initialize();
        const int previous = lv_subject_get_int(&changed_);
        lv_subject_set_int(&changed_,
                           previous == std::numeric_limits<int>::max() ? 0 : previous + 1);
    }
    uint64_t generation_ = 0;
    Event event_;
    mutable bool initialized_ = false;
    mutable lv_subject_t changed_{};
    mutable SubjectManager subjects_;
};
} // namespace helix
