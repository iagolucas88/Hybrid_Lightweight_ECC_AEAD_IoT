#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="${1:-3}"
BENCH_CMD="${BENCH_CMD:-qemu-arm ./build-arm/benchmark_suite}"

if ! [[ "$RUNS" =~ ^[0-9]+$ ]] || [[ "$RUNS" -le 0 ]]; then
    echo "Uso: ./run_all.sh [N]"
    echo "  N: numero de rodadas (inteiro positivo). Exemplo: ./run_all.sh 5"
    exit 1
fi

if [[ -x "$ROOT_DIR/benchmarks/resultados/bin/python" ]]; then
    PYTHON_BIN="$ROOT_DIR/benchmarks/resultados/bin/python"
else
    PYTHON_BIN="python3"
fi

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    echo "[ERRO] Python nao encontrado: $PYTHON_BIN"
    exit 1
fi

if [[ ! -f "$ROOT_DIR/benchmarks/plot_results.py" ]]; then
    echo "[ERRO] Script de plot nao encontrado em benchmarks/plot_results.py"
    exit 1
fi

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_BASE="$ROOT_DIR/benchmark_runs/$STAMP"
mkdir -p "$OUT_BASE"

printf "\n=== Benchmark em lote ===\n"
printf "Raiz: %s\n" "$ROOT_DIR"
printf "Rodadas: %s\n" "$RUNS"
printf "Comando benchmark: %s\n" "$BENCH_CMD"
printf "Python: %s\n" "$PYTHON_BIN"
printf "Saida: %s\n\n" "$OUT_BASE"

SUCCESS_RUNS=0

for i in $(seq 1 "$RUNS"); do
    RUN_ID="run_$(printf "%02d" "$i")"
    RUN_DIR="$OUT_BASE/$RUN_ID"
    mkdir -p "$RUN_DIR"

    printf "[%s/%s] Executando benchmark...\n" "$i" "$RUNS"

    if (cd "$ROOT_DIR" && bash -lc "$BENCH_CMD") >"$RUN_DIR/benchmark_stdout.log" 2>&1; then
        :
    else
        echo "  [FALHA] Benchmark falhou nesta rodada. Veja $RUN_DIR/benchmark_stdout.log"
        continue
    fi

    if [[ ! -f "$ROOT_DIR/benchmark_results.csv" ]]; then
        echo "  [FALHA] benchmark_results.csv nao foi gerado."
        continue
    fi

    cp "$ROOT_DIR/benchmark_results.csv" "$RUN_DIR/benchmark_results.csv"

    printf "[%s/%s] Gerando graficos da rodada...\n" "$i" "$RUNS"
    if (
        cd "$RUN_DIR"
        "$PYTHON_BIN" "$ROOT_DIR/benchmarks/plot_results.py" "$RUN_DIR/benchmark_results.csv"
    ) >"$RUN_DIR/plot_stdout.log" 2>&1; then
        SUCCESS_RUNS=$((SUCCESS_RUNS + 1))
        echo "  [OK] Rodada concluida: $RUN_DIR"
    else
        echo "  [FALHA] Plotagem falhou nesta rodada. Veja $RUN_DIR/plot_stdout.log"
    fi

done

echo
if [[ "$SUCCESS_RUNS" -eq 0 ]]; then
    echo "Nenhuma rodada bem-sucedida. Encerrando sem comparativo final."
    exit 1
fi

echo "Rodadas bem-sucedidas: $SUCCESS_RUNS/$RUNS"

echo "Gerando comparativo final..."
COMP_DIR="$OUT_BASE/comparison"
mkdir -p "$COMP_DIR"

"$PYTHON_BIN" - "$OUT_BASE" "$COMP_DIR" <<'PYEOF'
import sys
from pathlib import Path
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

out_base = Path(sys.argv[1])
comp_dir = Path(sys.argv[2])
comp_dir.mkdir(parents=True, exist_ok=True)

csv_files = sorted(out_base.glob("run_*/benchmark_results.csv"))
if not csv_files:
    raise SystemExit("Sem CSVs para comparar.")

frames = []
for f in csv_files:
    run_name = f.parent.name
    df = pd.read_csv(f)
    df["label"] = df["label"].astype(str).str.strip().str.strip('"')
    df["run"] = run_name
    frames.append(df)

all_df = pd.concat(frames, ignore_index=True)

summary = (
    all_df.groupby("label", as_index=False)
          .agg(
              runs=("run", "nunique"),
              mean_of_means_us=("mean_us", "mean"),
              std_of_means_us=("mean_us", "std"),
              min_mean_us=("mean_us", "min"),
              max_mean_us=("mean_us", "max"),
              mean_p95_us=("p95_us", "mean"),
              mean_p99_us=("p99_us", "mean"),
          )
          .sort_values("mean_of_means_us", ascending=False)
)
summary.to_csv(comp_dir / "comparison_summary.csv", index=False)

# 1) Full protocol trend across runs
proto = all_df[all_df["label"].str.contains("Protocolo|FullProto", regex=True)]
if not proto.empty:
    proto = proto.sort_values("run")
    plt.figure(figsize=(8, 4.5))
    plt.plot(proto["run"], proto["mean_us"] / 1e3, marker="o", linewidth=2, color="#1565C0", label="Media")
    plt.plot(proto["run"], proto["p95_us"] / 1e3, marker="s", linestyle="--", color="#EF6C00", label="P95")
    plt.plot(proto["run"], proto["p99_us"] / 1e3, marker="^", linestyle=":", color="#C62828", label="P99")
    plt.title("Protocolo Hibrido Completo - Variacao entre Rodadas")
    plt.xlabel("Rodada")
    plt.ylabel("Tempo (ms)")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend()
    plt.tight_layout()
    plt.savefig(comp_dir / "compare_full_protocol_trend.png", dpi=150)
    plt.close()

# 2) Montgomery levels comparison
mont = all_df[all_df["label"].str.contains("Curve25519", regex=False)].copy()
if not mont.empty:
    mont["field_bits"] = pd.to_numeric(
        mont["label"].str.extract(r"/\s*(\d+)-bit field\)\s*(?:\[real\])?")[0],
        errors="coerce"
    )
    mont = mont.dropna(subset=["field_bits"]).sort_values(["run", "field_bits"])
    plt.figure(figsize=(8.5, 5))
    for run_name, grp in mont.groupby("run"):
        plt.plot(grp["field_bits"], grp["mean_us"], marker="o", linewidth=2, label=run_name)
    plt.xscale("log", base=2)
    plt.yscale("log")
    plt.title("Escada de Montgomery - Comparacao entre Rodadas")
    plt.xlabel("Tamanho do campo (bits)")
    plt.ylabel("Tempo medio (us)")
    plt.grid(True, which="both", linestyle="--", alpha=0.35)
    plt.legend()
    plt.tight_layout()
    plt.savefig(comp_dir / "compare_montgomery_runs.png", dpi=150)
    plt.close()

# 3) Heatmap mean_us by label and run
pivot = all_df.pivot_table(index="label", columns="run", values="mean_us", aggfunc="mean")
if not pivot.empty:
    pivot = pivot.sort_values(by=list(pivot.columns), ascending=False)
    fig_h = max(5, 0.35 * len(pivot))
    plt.figure(figsize=(10, fig_h))
    im = plt.imshow(pivot.values, aspect="auto")
    plt.yticks(np.arange(len(pivot.index)), pivot.index, fontsize=7)
    plt.xticks(np.arange(len(pivot.columns)), pivot.columns, rotation=30, ha="right")
    plt.title("Heatmap de Tempo Medio (us) por Teste e Rodada")
    cbar = plt.colorbar(im)
    cbar.set_label("mean_us")
    plt.tight_layout()
    plt.savefig(comp_dir / "compare_heatmap_mean_us.png", dpi=150)
    plt.close()

# 4) Ranking de estabilidade (coeficiente de variacao)
stability = (
    all_df.groupby("label", as_index=False)
          .agg(mean_us=("mean_us", "mean"), std_us=("mean_us", "std"))
)
stability["cv_pct"] = (stability["std_us"] / stability["mean_us"]) * 100.0
stability = stability.sort_values("cv_pct", ascending=False)
stability.to_csv(comp_dir / "stability_cv.csv", index=False)

plt.figure(figsize=(10, max(5, 0.3 * len(stability))))
plt.barh(stability["label"], stability["cv_pct"], color="#8E24AA", edgecolor="black", linewidth=0.4)
plt.gca().invert_yaxis()
plt.xlabel("Coeficiente de variacao (%)")
plt.title("Estabilidade por Teste entre Rodadas (CV%)")
plt.grid(axis="x", linestyle="--", alpha=0.4)
plt.tight_layout()
plt.savefig(comp_dir / "compare_stability_cv.png", dpi=150)
plt.close()

print(f"Comparativo final salvo em: {comp_dir}")
PYEOF

echo "[OK] Comparativo final gerado em: $COMP_DIR"
echo

echo "Arquivos principais:"
echo "  - Pasta das rodadas: $OUT_BASE"
echo "  - Resumo comparativo: $COMP_DIR/comparison_summary.csv"
echo "  - Estabilidade (CV): $COMP_DIR/stability_cv.csv"
echo

echo "Exemplos de uso:"
echo "  ./run_all.sh 5"
echo "  BENCH_CMD='qemu-arm ./build-arm/benchmark_suite' ./run_all.sh 3"
