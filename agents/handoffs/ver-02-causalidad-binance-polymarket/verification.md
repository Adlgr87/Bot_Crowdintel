# VERIFICATION — VER-02: Causalidad Binance BTC ↔ Polymarket

**Verificador:** AUD-STAT
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** Python 3.x, statsmodels, pandas, numpy

---

## Verdict: VERIFIED_FAIL — No causality analysis exists

### Finding

The observation **CONFIRMS** that there is no Granger causality analysis, lead-lag analysis, or cross-correlation study between Binance BTC/USDT returns and Polymarket contract prices.

### Evidence

**Code analysis:**

The codebase uses Binance as an external alpha source:

```cpp
// market_config.hpp línea 112:
double binance_weight = 0.45;  // Binance feed weight in fusion
double pyth_weight = 0.55;     // Pyth feed weight in fusion
```

```cpp
// binance_ws_client.hpp línea 58-60:
namespace SOURCE {
    inline constexpr uint32_t BINANCE_OFI = 0x02;
}
// Registered with Bayesian engine as source 0x02 with weight 0.85
```

**But:** The OFI from Binance is fed into `BayesianEngine::update_count()` or `update_lr()`, which is then combined with market price in `compute_conviction()`:

```cpp
// engine_extensions.hpp líneas 89-108:
conviction = w_bb * bb_p_up + w_ofi * ofi_signal + w_twap * twap_signal
```

**No causality analysis exists:**

```
$ grep -r "Granger\|causalidad\|lead.lag\|cross.correlation\|causal" .
# Resultado: 0 matches
```

**No data for analysis:**

```
$ find . -name "*.parquet" -o -name "*.csv" -path "*/historical*"
# Resultado: tests/replay/sample_ticks.csv (50 ticks, insufficiente)
```

### What exists vs. what's needed

| Analysis component | Exists? | Location |
|-------------------|---------|----------|
| Binance WS client implementation | ✅ | binance_ws_client.hpp |
| OFI calculation from Binance | ✅ | ofi_calculator.hpp, ofi_linear_filter.hpp |
| Binance → BayesianEngine integration | ✅ | engine_extensions.hpp |
| Granger causality test | ❌ | No existe |
| Lead-lag cross-correlation | ❌ | No existe |
| Robust regression control | ❌ | No existe |
| Historical dataset Binance + Polymarket | ❌ | Solo sample_ticks.csv (50 rows) |

### Risk

The strategy assigns `binance_weight = 0.45` and `binance_binance_weight = 0.85` to the Binance feed, but **no evidence exists** that Binance BTC price **predicts** (Granger-causes) Polymarket prices. The correlation could be:
- Contemporaneous (no predictive value)
- Reverse causality (Polymarket moves Binance)
- Spurious (both driven by external event)

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Granger causality (p < 0.01) con lead consistente | ❌ | No analysis exists |
| Lead-lag cross-correlation | ❌ | No analysis exists |
| Regresión robusta controlada | ❌ | No analysis exists |
| ADR documenting limitation if no causality | ❌ | No existe adr/ |
| Historical data Binance + Polymarket | ❌ | Solo 50 sample ticks |

## Recomendación

1. **Recolectar datos históricos**: Binance BTC/USDT 1m candles + Polymarket BTC 5m prices
2. **Análisis de Granger**: `statsmodels.tsa.stattools.grangercausalitytests()`
3. **Lead-lag cross-correlation**: `numpy.correlate()` con desfases 0-10 minutos
4. **Regresión robusta**: Controlar por hora del día, volatilidad, régimen
5. **ADR**: Documentar si es correlación no causal y ajustar sizing

## Artefactos

- `agents/handoffs/ver-02-causalidad-binance-polymarket/artifacts/causality_gap.md`
- `reports/stat/ver-02-no-causality-analysis.md`

## Signoff