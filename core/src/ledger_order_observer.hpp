#ifndef LEDGER_ORDER_OBSERVER_HPP
#define LEDGER_ORDER_OBSERVER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// The production bridge between order egress and the persistent ledger.
//
// Extracted from main so that the integration tests exercise *this* code rather
// than a copy of it.  Two rules are structural here:
//   1. no durable submission ticket → the order is never handed to the transport;
//   2. an ambiguous outcome → the order goes to UNKNOWN, is never retried, and
//      trading is disabled until a reconciliation resolves it.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "../include/event_ledger.hpp"
#include "market_config.hpp"
#include "order_recorder.hpp"
#include "polymarket_order.hpp"

namespace egress {

class LedgerOrderObserver : public SubmitObserver {
public:
    LedgerOrderObserver(ledger::EventLedger* ledger, const MarketConfig& config,
                        std::atomic<bool>& trading_enabled)
        : ledger_(ledger), config_(config), trading_enabled_(trading_enabled) {
        if (ledger_)
            recorder_ = std::make_unique<recorder::OrderRecorder>(*ledger_, config_);
    }

    bool available() const noexcept { return ledger_ != nullptr; }

    // Live egress overrides this to expose the parsed POST /order response
    // (amounts and trade ids) so a FAK partial fill is recorded immediately.
    virtual const clob::OrderPostResult* pending_detail() const noexcept {
        return nullptr;
    }

    bool on_before_egress(const WireBody& body) override {
        if (!ledger_) return true;  // no ledger: offline/paper path without state
        recorder::WireOrder wire{};
        if (!recorder::parse_wire_body(body.buf, body.len, config_.neg_risk, wire)) {
            disable("wire_body_unparseable");
            return false;
        }
        char error[192]{};
        if (!recorder_->record_submission(wire, error, sizeof(error))) {
            std::fprintf(stderr, "LEDGER: submission ticket failed: %s\n", error);
            disable("ledger_write_failed");
            return false;  // no durable record → no send
        }
        last_ticket_[0] = '\0';
        std::snprintf(last_ticket_, sizeof(last_ticket_), "%s", wire.ticket_id);
        return true;
    }

    void on_after_egress(const WireBody& body, const SubmitResult& result) override {
        if (!ledger_) return;
        recorder::WireOrder wire{};
        if (!recorder::parse_wire_body(body.buf, body.len, config_.neg_risk, wire)) {
            disable("wire_body_unparseable");
            return;
        }
        char error[192]{};
        if (!recorder_->record_result(wire, result, result.ambiguous, pending_detail(),
                                      error, sizeof(error))) {
            std::fprintf(stderr, "LEDGER: result record failed: %s\n", error);
            disable("ledger_write_failed");
            return;
        }
        if (result.ambiguous) {
            std::fprintf(stderr,
                         "UNKNOWN: order outcome is ambiguous (%s); not retrying, "
                         "trading disabled until reconciliation resolves it\n",
                         result.error);
            disable("ambiguous_order_outcome");
        }
        outcomes_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t outcomes() const noexcept {
        return outcomes_.load(std::memory_order_relaxed);
    }
    uint64_t disables() const noexcept {
        return disables_.load(std::memory_order_relaxed);
    }
    const char* last_ticket() const noexcept { return last_ticket_; }
    const recorder::OrderRecorder* recorder() const noexcept { return recorder_.get(); }

protected:
    void disable(const char* reason) {
        trading_enabled_.store(false, std::memory_order_release);
        disables_.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "TRADING DISABLED: %s\n", reason);
    }

    ledger::EventLedger* ledger_;
    const MarketConfig& config_;
    std::atomic<bool>& trading_enabled_;
    std::unique_ptr<recorder::OrderRecorder> recorder_;
    char last_ticket_[80]{};
    std::atomic<uint64_t> outcomes_{0};
    std::atomic<uint64_t> disables_{0};
};

}  // namespace egress

#endif  // LEDGER_ORDER_OBSERVER_HPP
