# 🧬 MutaLambda Adapter

This adapter serves as a **clean, decoupled bridge** between the Bot CrowdIntel Hot Path
and the upstream **[MutaLambda](https://github.com/Adlgr87/MutaLambda)** optimization engine.

## Why an Adapter?

Direct dependency management can bloat the bot's repository and create tight coupling.
This adapter abstracts the communication into a simple, testable contract:

- `MutaLambdaAdapter.mutate_function(module_path, function_name, optimization_goal)`
- `MutaLambdaAdapter.benchmark_improvement(pre_cycles, post_cycles)`
- `MutaLambdaAdapter.commit_successful_mutation(module_path, backup_path)`

## How to Use

1. Ensure MutaLambda is installed and its path is set in the `MUTALAMBDA_PATH` environment variable.
2. Run the optimization pipeline:
   ```bash
   python3 ../scripts/mutalambda_optimize.py
   ```

## Upstream

- **MutaLambda Repository**: https://github.com/Adlgr87/MutaLambda
- **Lineage of Evolutions**: See `docs/OPTIMIZATION_LINEAGE.md` in the main project root.
