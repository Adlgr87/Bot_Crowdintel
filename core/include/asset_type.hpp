#ifndef ASSET_TYPE_HPP
#define ASSET_TYPE_HPP

// Balance/allowance asset classes accepted by CLOB GET /balance-allowance.
//
// Verified 2026-10-03:
//  * Polymarket/clob-client src/types.ts:  enum AssetType { COLLATERAL, CONDITIONAL }
//  * Polymarket/py-sdk _internal/actions/orders/allowance.py:
//    resolve_order_balance_allowance_target() returns "COLLATERAL" for BUY and
//    "CONDITIONAL" (or "CONDITIONAL-V2" for protocol-v2 position ids) for SELL.
//  * Query parameters: asset_type, signature_type, token_id
//    (py-sdk _internal/actions/account.py:190-205).
//
// Shared by the ledger and the venue adapters, so it lives in its own header to
// avoid a dependency cycle between them.

#include <cstdint>

namespace venue {

enum class AssetType : uint8_t {
    COLLATERAL = 0,       // pUSD
    CONDITIONAL = 1,      // CTF outcome tokens
    CONDITIONAL_V2 = 2    // protocol-v2 position ids (PositionManager)
};

inline const char* asset_type_name(AssetType type) noexcept {
    switch (type) {
        case AssetType::COLLATERAL: return "COLLATERAL";
        case AssetType::CONDITIONAL: return "CONDITIONAL";
        case AssetType::CONDITIONAL_V2: return "CONDITIONAL-V2";
    }
    return "COLLATERAL";
}

inline AssetType asset_type_from_u8(uint8_t value) noexcept {
    switch (value) {
        case 1: return AssetType::CONDITIONAL;
        case 2: return AssetType::CONDITIONAL_V2;
        case 0:
        default: return AssetType::COLLATERAL;
    }
}

}  // namespace venue

#endif  // ASSET_TYPE_HPP
