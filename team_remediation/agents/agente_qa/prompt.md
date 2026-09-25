# QA — Fase 7: QA, Seguridad y Documentación

## Rol
Garantizar que ctest completo pase, latencia dentro del +10% de baseline, sin secretos en el repo, y documentación alineada.

## Branch
```
remediation/70-qa-docs
```

## Dependencia
Fases 1-6 completadas.

## Tareas

### T7-1: ctest completo verde + tests unitarios
- **Criterio**: Cobertura de cada bloque de riesgo con caso positivo y negativo
- **Acción**:
  - Ejecutar `ctest --output-on-failure`
  - Verificar tests: rate_limiter, risk_engine, order_manager, fee_model, compliance_guard, telemetry, position_tracker
  - Cada módulo debe tener tests PASS y FAIL

### T7-2: Regresión de latencia
- **Criterio**: Hot path P50/P99 dentro del +10% de la línea base
- **Acción**:
  - Ejecutar `./bin/latency_bench`
  - Comparar contra baseline en `docs/PERF_METRICS.md`
  - Registrar resultados actualizados

### T7-3: Escaneo de secretos
- **Criterio**: Sin secretos en commits; `git diff` limpio
- **Acción**:
  - `grep -rn "YOUR_API_KEY\|private_key\|secret\|0x[a-f0-9]\{64\}" core/ alpha/ --include="*.cpp" --include="*.hpp"`
  - Verificar `.env` ignorado: `git check-ignore .env`
  - Verificar `.gitignore` incluye `secrets/`, `*.env*`, `core/build/`

### T7-4: Actualizar docs/STATUS.md
- **Criterio**: STATUS coherente con el código
- **Acción**: Marcar resueltos los open items #2 (tick size), #3 (balance), #5 (fills lifecycle) si procede

### T7-5: Actualizar README
- **Criterio**: Documentación alineada
- **Acción**: Añadir sección de cumplimiento y límites de riesgo; wallet de producción vs pruebas; recomendación de vault/secret manager

### T7-6: Revisión adversarial final
- **Criterio**: Checklist firmada por QA
- **Acción**: Verificar que ningún control nuevo puede ser evadido por una señal de CrowdIntel (las señales externas nunca deben saltarse el risk engine)

## Checklist de Revisión Adversarial
- [ ] ¿Las señales de CrowdIntel pasan siempre por el RiskEngine?
- [ ] ¿El kill switch es atómico y no puede ser activado por señales externas?
- [ ] ¿Todas las constantes de riesgo son env-driven con defaults conservadores?
- [ ] ¿El hot path tiene cero I/O (std::cout, malloc, new)?
- [ ] ¿Todas las órdenes se rastrean por client_order_id?
- [ ] ¿No hay reintentos ciegos tras timeout de red?
- [ ] ¿La fórmula de comisiones está verificada contra docs oficiales?
- [ ] ¿El tick size es dinámico por mercado?
- [ ] ¿Ningún secreto en logs?

## Deliverables
1. ctest verde (todos los tests)
2. `docs/PERF_METRICS.md` actualizado con latencia post-remediación
3. `docs/STATUS.md` actualizado
4. `README.md` actualizado (sección cumplimiento + límites)
5. Checklist adversarial firmada
