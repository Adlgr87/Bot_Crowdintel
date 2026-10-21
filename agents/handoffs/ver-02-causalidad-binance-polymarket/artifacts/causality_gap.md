# Causality Gap Analysis — VER-02

## Missing analysis

No Granger causality, lead-lag, or cross-correlation analysis exists between Binance BTC/USDT and Polymarket contract prices.

## Required data

- Binance BTC/USDT 1m candles (historical)
- Polymarket BTC contract mid prices (historical)
- Cross-correlation at lags 0-10 minutes
- Granger causality test (statsmodels)

## Required methodology

```python
# Lead-lag cross-correlation
from statsmodels.tsa.stattools import grangercausalitytests
import numpy as np

# Lag 0-10 minutes cross-correlation
for lag in range(0, 10):
    cc = np.corrcoef(poli_prices[lag:], binance_prices[:-lag])

# Granger causality
max_lag = 5
result = grangercausalitytests(
    np.column_stack([poli_returns, binance_returns]),
    max_lag, verbose=True
)
```

## Required thresholds

- Granger p-value < 0.01 at consistent lag
- Lead-lag peak correlation at positive lag (Binance → Polymarket)
- Robust regression controlling for hour-of-day and regime

## Current state

- **No historical data** available in repo (only 50 sample ticks in sample_ticks.csv)
- No analysis scripts
- Binance weight hardcoded at 0.45 without justification

## Risk

Without causality evidence, the Binance feed integration may add:
- Noise (if correlation is contemporaneous, not predictive)
- Latency (without predictive benefit)
- False edge (if reverse causality)
