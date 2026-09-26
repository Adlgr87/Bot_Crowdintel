# 📊 Análisis de Especialización de Mercados — Bot CrowdIntel

> Basado en auditorías técnicas y documentación oficial de Polymarket CLOB V2  
> Fecha: 2025-09-25  
> Especialista: AGENTE_INVESTIGADOR  

---

## 🎯 Resumen Ejecutivo

El bot CrowdIntel, dada su arquitectura de ultra-baja latencia (C++20, hot path ~45μs P50), 
está mejor posicionado para **mercados de alta volatilidad con liquidez moderada** donde:
- Los movimientos de precio son rápidos y predecibles
- La competencia de market makers institucionales es menor
- Los spreads son estrechos (favorece la ejecución)
- El tick size es pequeño (más granularidad para arbitraje)

---

## 📋 Tipos de Mercados en Polymarket

### Categorías Principales

1. **Política / Eventos Geopolíticos**
   - Elecciones, acuerdos internacionales, decisiones regulatorias
   - Alta volatilidad, liquidez variable
   - Buenas oportunidades de arbitraje en horarios de eventos

2. **Deportes**
   - Partidos, torneos, carreras
   - Liquidez alta durante eventos activos
   - Tick size pequeño (>100)
   - Predicción más determinista

3. **Criptomonedas**
   - Precio de Bitcoin, Ethereum, eventos DeFi
   - Alta volatilidad, liquidez alta
   - Tick size pequeño
   - Competencia institucional alta

4. **Eventos Actuales / Virales**
   - Temas de tendencias sociales, eventos inesperados
   - Muy alta volatilidad
   - Liquidez impredecible
   - Alto riesgo/recompensa

5. **Ciencia / Tecnología**
   - Lanzamientos de productos, misiones espaciales
   - Liquidez moderada
   - Predicción científica más estable

---

## 🎯 Mercados Recomendados (Top 10)

| # | Categoría | Liquidez | Spread | Tick Size | Volatilidad | Competencia | Viabilidad para Bot |
|---|---|---|---|---|---|---|---|
| 1 | **Política (Elecciones)** | Alta | Estrecho | 0.0001-0.001 | Muy Alta | Media | ⭐⭐⭐⭐⭐ |
| 2 | **Deportes (NFL/NBA)** | Alta | Estrecho | 0.0001-0.01 | Alta | Media-Alta | ⭐⭐⭐⭐ |
| 3 | **Criptomonedas (BTC)** | Muy Alta | Muy estrecho | 0.001-0.01 | Alta | Alta | ⭐⭐⭐ |
| 4 | **Eventos Virales** | Variable | Amplio-estrecho | 0.01-0.1 | Muy Alta | Baja-Media | ⭐⭐⭐⭐ |
| 5 | **Política (Regulación)** | Media-Alta | Estrecho | 0.0001-0.01 | Alta | Baja-Media | ⭐⭐⭐⭐⭐ |
| 6 | **Deportes (MLB)** | Media | Estrecho | 0.0001-0.01 | Media-Alta | Baja | ⭐⭐⭐⭐ |
| 7 | **Tecnología (Lanzamientos)** | Media | Estrecho | 0.0001-0.01 | Alta | Baja | ⭐⭐⭐⭐ |
| 8 | **Criptomonedas (ALT coins)** | Media-Alta | Amplio | 0.01-0.1 | Muy Alta | Media | ⭐⭐⭐ |
| 9 | **Eventos Internacionales** | Variable | Variable | 0.001-0.1 | Alta | Baja | ⭐⭐⭐⭐ |
| 10 | **Ciencia (Misiones)** | Baja-Media | Amplio | 0.01-0.1 | Baja-Alta | Baja | ⭐⭐⭐ |

---

## 🎯 Recomendación de Especialización

### **Mercado Prioritario: Política / Eventos Geopolíticos**

**Justificación técnica:**
- **Tick size pequeño (0.0001)**: Permite mayor granularidad para el algoritmo de arbitraje
- **Alta volatilidad**: Movimientos rápidos crean oportunidades frecuentes
- **Liquidez moderada-alta**: Suficiente para operar sin impactar demasiado el precio
- **Competencia media**: Menos market makers institucionales que en criptomonedas
- **Eventos event-driven**: Los movimientos de precio son predecibles alrededor de eventos

**Estrategia de trading:**
- Detectar movimientos de precio > 2 ticks en < 100ms
- Ejecutar órdenes de market making con spread ajustado
- Cancelar órdenes rápidamente si la probabilidad cambia

### **Mercado Secundario: Deportes (NFL/NBA durante partidos)**

**Justificación técnica:**
- **Tick size pequeño**: Similar a política
- **Liquidez alta durante eventos**: Buena profundidad en el order book
- **Eventos programados**: Permite preparación anticipada
- **Resultados deterministas**: Menos riesgo de resolución ambigua

### **Mercado Terciario: Criptomonedas (BTC - Eventos Específicos)**

**Justificación:**
- Solo mercados específicos (ej: "BTC > $100K para fin de año")
- Tick size pequeño
- Evitar mercados generales con alta competencia institional

---

## 📐 Análisis Técnico para el Bot C++20

### Características del Bot que Favorecen Determinados Mercados

1. **Ultra-baja latencia (~45μs P50)**:
   - Ideal para mercados con movimientos rápidos (< 100ms)
   - Menos efectivo en mercados estables con pocos movimientos

2. **Zero-alloc hot path**:
   - Permite operar a alta frecuencia sin degradación
   - Ideal para mercados con alta frecuencia de actualizaciones

3. **Risk engine integrado**:
   - Necesario para mercados volátiles con alto riesgo
   - Control de exposición por mercado crítico

### Parámetros Recomendados por Tipo de Mercado

| Tipo de Mercado | Tick Size | Min Order Size | Max Exposure/Market | Max Orders/min |
|----------------|-----------|---------------|---------------------|----------------|
| Política | 0.0001-0.001 | 5-50 shares | $5,000-$10,000 | 10-20 |
| Deportes | 0.0001-0.01 | 10-100 shares | $3,000-$5,000 | 15-30 |
| Criptomonedas | 0.001-0.01 | 100-1000 shares | $2,000-$5,000 | 20-50 |
| Eventos Virales | 0.01-0.1 | 5-25 shares | $1,000-$3,000 | 5-15 |

---

## 🔧 Requisitos Técnicos Específicos

### Para Mercados de Política (Prioridad #1):

1. **API Integration**:
   - Endpoint: `GET /v2/market/{token_id}` para tick size y min order size
   - Endpoint: `GET /v2/book/{market}` para L2 order book
   - WSS: `wss://ws-subscriptions-clob.polymarket.com/ws/user` para fills

2. **Configuration**:
   ```
   BOT_TICK_SIZE_AUTO=1          # Fetch tick size dynamically
   BOT_MIN_ORDER_SIZE_AUTO=1     # Fetch min order size from API
   RISK_MAX_EXPOSURE_PER_MARKET=10000  # Higher for volatile markets
   RISK_MAX_DAILY_LOSS_USD=250   # Conservative for volatility
   ```

3. **Strategy**:
   - Kelly Criterion con ajuste de volatilidad
   - Filtro de net EV con comisiones dinámicas
   - Market making con spread = 3 × tick_size
   - Cancelación automática si tick size cambia

### Para Mercados Deportivos:

1. **Timing**:
   - Activar 1h antes del evento
   - Desactivar inmediatamente después del resultado
   - Tick size mayor (0.01) → menos granularidad

2. **Risk Management**:
   - Exposure más conservador durante eventos vivos
   - Timeout de órdenes < 30s (eventos cambian rápido)

---

## 📊 Viabilidad de Implementación

| Componente | Estado Actual | Requerimiento | Prioridad |
|------------|--------------|--------------|-----------|
| Dynamic tick size | Mock (hardcoded 100) | API fetch real | HIGH |
| Min order size | Default 5 | API fetch | MEDIUM |
| Market metadata cache | Implementado (mock) | Real API integration | HIGH |
| User channel WSS | No implementado | WebSocket autenticado | HIGH |
| Order lifecycle | Partial | Full tracking | HIGH |
| Balance/allowance | Partial (stale defaults) | Real API queries | MEDIUM |

---

## 📋 Conclusión

**Recomendación Principal**: Especializar el bot en **mercados de política y eventos geopolíticos** debido a:
1. Tick size pequeño (mayor granularidad para arbitraje)
2. Alta frecuencia de movimientos de precio
3. Competencia institucional moderada
4. Eventos predecibles que generan volatilidad

**Mercado Secundario**: Especializar en **deportes** (NFL/NBA) como segunda prioridad, con configuración de riesgo más conservadora.

**Evitar**: Mercados de criptomonedas generales debido a alta competencia institional y spreads demasiado estrechos para rentabilidad de bots no institucionales.

---

## 📝 Notas sobre Seguridad y Cumplimiento

- Todos los mercados deben verificarse contra listas restringidas (COMPLIANCE_RESTRICTED_TOKENS)
- La jurisdicción del operador debe validarse (US, CA, GB con recomendación)
- Los mercados deben estar en estado "active" antes de trading
- Tick size debe obtenerse de la API, no hardcoded
- Min order size debe verificarse antes de enviar órdenes