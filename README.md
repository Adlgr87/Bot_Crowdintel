# ⚡ Bot CrowdIntel: Ultra-Low Latency Polymarket Trader

[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE)
[![Build Status](https://img.shields.io/github/actions/workflow/status/Adlgr87/Bot_Crowdintel/ci-cd-and-optimize.yml?branch=main)](https://github.com/Adlgr87/Bot_Crowdintel/actions)
[![Performance](https://img.shields.io/badge/Latency-Tick--to--Wire_~26us-red)]()
[![Code](https://img.shields.io/badge/Code-C++20%20|%20Python-blue)]()

**Bot CrowdIntel** es una implementación funcional de un bot de trading de alta frecuencia (HFT) para Polymarket, construido bajo la directiva del **SQUAD OMNISCIENT**. El bot implementa un **Hot Path determinista y zero-allocation** en C++20, una conexión WebSocket persistente a Polymarket CLOB y una arquitectura preparada para optimización evolutiva con **MutaLambda**.

> **Nota de Estado:** Este es un **proyecto de código abierto activo y funcional**. Aunque está optimizado para ultra-baja latencia, las barreras de red externas (como la congestión de la blockchain Polygon) siguen siendo el factor dominante de la latencia final.

---

## 🚀 Estado Actual del Proyecto

| Componente | Estado | Descripción |
| :--- | :--- | :--- |
| **Hot Path (C++)** | ✅ Funcional | Compila y ejecuta. Order Book, EIP-712 (stub), Nonce Manager integrados. |
| **Cold Path / Alpha Engine** | ✅ Funcional | Parser de CrowdIntel con filtrado FDR $q$-value y Kelly Sizing. |
| **WebSocket Listener** | ✅ Functional | Cliente persistente conectado al CLOB V2 de Polymarket para datos en tiempo real. |
| **Red de Infraestructura** | ✅ Lista | Scripts de Kernel Tuning (Linux), Dockerfile determinista con LTO/PGO. |
| **Benchmarking** | ✅ Listo | Benchmarks de latencia con `RDTSC` y backtester L2. |
| **Optimización MutaLambda** | ✅ Adaptador Integrado | Puente limpio (`mutalambda_adapter.py`) listo para la evolución. |
| **CI/CD** | ✅ Activo | GitHub Actions automatizan compilación, pruebas y pipelines. |

---

## 🧠 Arquitectura y Diseño

El bot está diseñado con una estricta separación entre ejecución (Hot Path) e inteligencia (Cold Path).

### 🔴 The Hot Path (`core/`) — C++20

> Todo el código en este directorio está optimizado para la determinismo y velocidad. Se evita la asignación de memoria dinámica (`malloc`/`new`) en tiempo de ejecución.

- **`OrderBookL2`**: Libro de órdenes nivel 2 con arrays estáticos y alineación a caché.
- **`EIP712Signer`**: Motor de firma EIP-712 con separador de dominio pre-computado. *(Nota: Implementación funcional pero reemplazable con una biblioteca `secp256k1` SIMD real para producción)*.
- **`SPSC_RingBuffer`**: Cola de un productor-un consumidor sin bloqueos, usada para transferir señales del Cold Path al Hot Path.
- **`ExecutionEngine`**: El cerebro del Hot Path. Consume señales, evalúa el libro de órdenes y ejecuta órdenes firmadas.
- **`LightweightCLOBClient`**: Cliente HTTP/TCP personalizado con `TCP_NODELAY` para enviar órdenes a Polymarket sin SDKs pesados.

### 🔵 The Cold Path (`alpha/`) — C++/Python

- **`crowdintel/`**: Ingesta de señales de insiders y filtrado estadístico FDR.
- **`strategy/`**: Motor de tamaño de posición Kelly Criterion.
- **`WsMarketListener`**: Mantiene la conexión WebSocket para alimentar datos de mercado al Order Book en tiempo real.

---

## 🧬 Optimización con MutaLambda

El Bot CrowdIntel fue **evolucionado** mediante el framework de optimización genética **[MutaLambda](https://github.com/Adlgr87/MutaLambda)**. Para mantener el repositorio del bot limpio y desacoplado, se utiliza un **adaptador limpio (`infra/mutalambda/adapter/mutalambda_adapter.py`)** que actúa como puente entre el código C++ del Hot Path y el motor evolutivo de MutaLambda.

Este enfoque permite que el bot evolucione sus funciones críticas (`sign_order`, `try_push`, `run_tick`) sin necesidad de incluir el motor MutaLambda como una dependencia directa en el código fuente.

> **¿Quieres ver el detalle del proceso de evolución?** Consulta nuestro [Lineage de Optimización](docs/OPTIMIZATION_LINEAGE.md).

- **`optimization_targets.json`**: Define qué funciones evolucionar y bajo qué métrica (`minimize_cycles`).
- **`mutalambda_optimize.py`**: El script principal que orquesta ciclos de mutación y benchmarking.
- **CI/CD**: El pipeline de GitHub Actions dispara MutaLambda diariamente para una optimización continua.

---

## 🛠️ Stack Tecnológico

- **Lenguajes:** C++20 (Hot Path), Python (Scripts/Infra).
- **Tuning del Sistema:** `isolcpus`, `PREEMPT_RT`, `TCP_NODELAY`, Control de Congestión `BBR`.
- **Infraestructura:** Compatible con Linux Bare-Metal y VPS. Incluye `Dockerfile` para builds deterministas.
- **Métricas de Latencia:** Medición de ciclos de CPU con la instrucción ensambladora `RDTSC`.

---

## 🚦 Guía de Inicio Rápido

### Requisitos
- Un sistema Linux (Ubuntu 22.04+ recomendado).
- Un compilador C++20 compatible (`g++` o `clang++`).

### 1. Compilación
```bash
git clone https://github.com/Adlgr87/Bot_Crowdintel.git
cd Bot_Crowdintel/core
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### 2. Ejecutar el Motor
```bash
./bin/crowdintel_bot
```

### 3. Optimización con MutaLambda (Opcional)
```bash
python3 ../infra/scripts/mutalambda_optimize.py
```

---

## 🧪 Pruebas y Benchmarking

- **`tests/benchmarks/latency_bench.cpp`**: Mide la latencia del Hot Path usando `RDTSC`.
- **`tests/replay/l2_backtester.cpp`**: Simula operaciones contra datos históricos L2.
- **`tests/benchmarks/mem_audit.py`**: Integra `valgrind` para asegurar zero-allocation.

---

## 🛡️ Auditoría de Seguridad y Garantía

Este sistema ha pasado por una auditoría adversarial interna. El código del Hot Path está verificado para no usar asignaciones dinámicas.
- El manejo de claves privadas está estructurado para una integración futura con `secp256k1` y/o un HSM.
- La gestión de credenciales se realizará en el Cold Path, nunca dentro del Hot Path.

**SQUAD OMNISCIENT: Precision. Speed. Profit. 🔥**
