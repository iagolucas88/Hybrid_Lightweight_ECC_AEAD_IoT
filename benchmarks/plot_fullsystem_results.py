#!/usr/bin/env python3
# =============================================================================
# benchmarks/plot_fullsystem_results.py
# -----------------------------------------------------------------------------
# Script INTERATIVO de plotagem para os resultados da suite full-system
# (benchmark_suite_fullsystem, executada via run_defense_vm.sh).
#
# AO EXECUTAR, o script pergunta:
#   1) o nome da pasta de teste, DENTRO de benchmark_runs/ (ex.: apenas
#      "20260916_152829" — nao precisa digitar o caminho completo)
#   2) quais rodadas plotar (ex.: "1,4,5" ou "1-13" ou "1-3,7,9-10")
#
# SAIDA: <pasta_de_teste>/runs/run_NN/ para graficos individuais e
# <pasta_de_teste>/comparison/ para graficos comparativos.
#   <RR>_<grafico>.png       -> um grafico POR RODADA selecionada (RR = numero
#                               da rodada com 2 digitos, ex.: 01, 04, 13)
#   comp_<NN>_<grafico>.png  -> graficos COMPARATIVOS entre as rodadas
#                               selecionadas (NN = indice sequencial do tipo
#                               de grafico comparativo, ex.: 01, 02, ...)
#
# Os comparativos sao calculados NA HORA a partir dos resultados_benchmark.csv
# das rodadas selecionadas (nao dependem de comparison_summary.csv, que sempre
# agrega TODAS as rodadas de uma execucao — aqui o usuario escolhe o subconjunto).
#
# Sempre que um intervalo de rodadas e plotado, uma tabela condensada com o
# resumo estatistico (pronta para colar na dissertacao) e exportada em
# comparison/analise_intervalo_macro_<inicio>_a_<fim>.csv
#
# USO:
#   python plot_fullsystem_results.py
#   python plot_fullsystem_results.py benchmark_runs/20260916_152829
#   python plot_fullsystem_results.py benchmark_runs/20260916_152829 1-13
#   python plot_fullsystem_results.py benchmark_runs/20260916_152829 --macro-start 1 --macro-end 30
# =============================================================================
import argparse
import os
import re
import sys
import textwrap
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")

# Auto-detecção de ambiente virtual: se executado com um Python sem as bibliotecas,
# reexecuta automaticamente com o virtualenv do projeto (se disponível).
try:
    import matplotlib
    import pandas
except ImportError:
    _root = Path(__file__).resolve().parent.parent
    _candidates = [
        _root / "benchmarks" / "resultados" / "bin" / "python",
        _root / "benchmarks" / "resultados" / "bin" / "python3",
        _root / ".venv" / "bin" / "python",
        _root / "venv" / "bin" / "python",
    ]
    for _py in _candidates:
        if _py.is_file() and os.access(_py, os.X_OK):
            os.execv(str(_py), [str(_py)] + sys.argv)
    sys.exit(
        "\n[ERRO] Bibliotecas necessárias não encontradas (matplotlib, pandas, numpy).\n"
        "Para instalar no seu ambiente Python, execute:\n"
        "    pip install -r benchmarks/requirements.txt\n\n"
        "Ou instale via apt no Ubuntu/Debian:\n"
        "    sudo apt-get install -y python3-matplotlib python3-pandas python3-numpy\n"
    )

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


# ══════════════════════════════════════════════════════════════════════════
# ENTRADA INTERATIVA E PARSING
# ══════════════════════════════════════════════════════════════════════════

# Raiz do repositorio (benchmarks/../) — permite localizar benchmark_runs/
# independente do diretorio de onde o script for chamado.
REPO_ROOT = Path(__file__).resolve().parent.parent
BENCHMARK_RUNS_DIR = REPO_ROOT / "benchmark_runs"


def resolve_test_dir(raw: str) -> Path | None:
    """Aceita caminho completo OU apenas o nome dentro de benchmark_runs/."""
    candidates = [
        Path(raw),
        BENCHMARK_RUNS_DIR / raw,
        Path.cwd() / "benchmark_runs" / raw,
    ]
    for c in candidates:
        if c.is_dir():
            return c
    return None


def ask_test_dir(arg):
    while True:
        raw = arg or input(
            "Pasta de teste dentro de benchmark_runs/ (ex.: 20260916_152829): "
        ).strip()
        arg = None
        test_dir = resolve_test_dir(raw)
        if test_dir is not None:
            return test_dir
        print(f"  [erro] pasta nao encontrada: {raw} (procurei em '.' e em '{BENCHMARK_RUNS_DIR}')")


def discover_runs(test_dir: Path):
    runs = {}
    for p in sorted((test_dir / "runs").glob("run_*")):
        m = re.match(r"run_(\d+)$", p.name)
        if p.is_dir() and m:
            runs[int(m.group(1))] = p
    return runs


def parse_run_spec(spec: str, available: dict):
    """Aceita '1,4,5', '1-13', '1-3,7,9-10', com ou sem zeros a esquerda.
    Se '0' ou 'nenhum', retorna [] (nenhuma rodada)."""
    raw_s = spec.strip().lower()
    if raw_s in ("0", "nenhum", "nenhuma", "none", "nao", "n"):
        return []
    selected = set()
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if token == "0":
            return []
        if "-" in token:
            lo, hi = token.split("-", 1)
            selected.update(range(int(lo), int(hi) + 1))
        else:
            selected.add(int(token))
    missing = sorted(n for n in selected if n not in available)
    if missing:
        print(f"  [aviso] rodadas nao encontradas e ignoradas: {missing}")
    return sorted(n for n in selected if n in available)


def ask_macro_interval(available: dict, cli_start: int | None = None, cli_end: int | None = None, cli_runs: str | None = None) -> list:
    """Determina o intervalo das macro-rodadas (via CLI ou interativo)."""
    if cli_start is not None and cli_end is not None:
        if cli_start > cli_end:
            sys.exit("[ERRO] --macro-start deve ser <= --macro-end.")
        macro = sorted(n for n in range(cli_start, cli_end + 1) if n in available)
        if macro:
            return macro
    if cli_runs is not None and cli_runs.lower() not in ("intervalo", "inter", "i"):
        runs = parse_run_spec(cli_runs, available)
        if runs:
            return runs

    lo_all, hi_all = min(available), max(available)
    print(f"Rodadas disponiveis em benchmark_runs: {lo_all}..{hi_all} (total {len(available)})")
    while True:
        raw = input(f"Defina o intervalo das macros (ex.: {lo_all}-{hi_all}): ").strip()
        if not raw:
            return sorted(available.keys())
        try:
            runs = parse_run_spec(raw, available)
        except ValueError:
            print(f"  [erro] formato invalido. Use algo como '{lo_all}-{hi_all}'.")
            continue
        if runs:
            return runs
        print("  [erro] nenhuma rodada valida encontrada no intervalo, tente novamente.")


def ask_runs_to_plot(available: dict, macro_runs: list, cli_arg: str | None = None) -> list:
    """
    Pergunta quais rodadas (runs) terao seus graficos plotados:
      - 'intervalo' (ou Enter): plota o intervalo das macros
      - '1,4,5' ou '1-5': plota as rodadas especificadas
      - '0': nao plota graficos das runs
    """
    if cli_arg is not None:
        raw = str(cli_arg).strip().lower()
        if raw in ("0", "nenhum", "nenhuma", "none", "nao", "n"):
            return []
        if raw in ("intervalo", "inter", "i"):
            return macro_runs
        runs = parse_run_spec(raw, available)
        if runs:
            return runs
        print(f"  [aviso] argumento de runs '{cli_arg}' invalido. Perguntando interativamente.")

    lo_m, hi_m = (min(macro_runs), max(macro_runs)) if macro_runs else (min(available), max(available))
    print(f"\n--- Selecao das Rodadas (Runs) para Plotar Graficos ---")
    print(f"  - 'intervalo' (ou Enter): usar o intervalo das macros (rodadas {lo_m}..{hi_m})")
    print(f"  - digitar rodadas especificas (ex.: 1,4,5 ou 1-5)")
    print(f"  - '0': nao plotar graficos das runs (opcao 0)")

    while True:
        raw = input("Quais runs plotar? [intervalo / 1,4,5 / 0] (padrao: intervalo): ").strip()
        if not raw or raw.lower() in ("intervalo", "inter", "i"):
            return macro_runs
        if raw.lower() in ("0", "nenhum", "nenhuma", "none", "nao", "n"):
            return []
        try:
            runs = parse_run_spec(raw, available)
        except ValueError:
            print("  [erro] formato invalido. Use 'intervalo', '1,4,5', '1-5' ou '0'.")
            continue
        if runs:
            return runs
        print("  [erro] nenhuma rodada valida selecionada, tente novamente.")


def ask_comparison_scope(available: dict, macro_runs: list, cli_scope: str | None = None) -> list:
    """
    Determina o escopo das rodadas para os graficos de comparacao (comp_*):
      - Opcao 1: Apenas o intervalo das macros
      - Opcao 2: Todas as rodadas do teste
      - Opcao 0: Nao plotar graficos de comparacao
    """
    if cli_scope is not None:
        raw = str(cli_scope).strip().lower()
        if raw in ("0", "nenhum", "none", "nao", "n"):
            return []
        if raw in ("1", "intervalo", "selecionadas", "selecionada", "selected", "s", "sel", "i"):
            return macro_runs if macro_runs else sorted(available.keys())
        if raw in ("2", "todas", "all", "t", "a"):
            return sorted(available.keys())
        print(f"  [aviso] escopo de comparacao '{cli_scope}' nao reconhecido. Perguntando interativamente.")

    lo_all, hi_all = min(available), max(available)
    print("\n--- Definicao dos Graficos de Comparacao (comp_*) ---")
    if macro_runs and sorted(available) != macro_runs:
        lo_m, hi_m = min(macro_runs), max(macro_runs)
        print(f"  1) Usar intervalo das macros: rodadas {lo_m}..{hi_m} (total {len(macro_runs)})")
        print(f"  2) Usar todas as rodadas da pasta: rodadas {lo_all}..{hi_all} (total {len(available)})")
        print("  0) Nao plotar graficos de comparacao (opcao 0)")
        valid_choices = ("0", "1", "2")
        prompt = "Escolha [1/2/0] (padrao: 1): "
    else:
        print(f"  1) Gerar comparativos com todas as rodadas: rodadas {lo_all}..{hi_all} (total {len(available)})")
        print("  0) Nao plotar graficos de comparacao (opcao 0)")
        valid_choices = ("0", "1")
        prompt = "Escolha [1/0] (padrao: 1): "

    while True:
        raw = input(prompt).strip().lower()
        if not raw:
            raw = "1"
        if raw in ("0", "nenhum", "none", "nao", "n"):
            return []
        if raw in ("1", "intervalo", "selecionadas", "selecionada", "s", "sel", "i"):
            return macro_runs if macro_runs else sorted(available.keys())
        if raw in ("2", "todas", "all", "t", "a") and "2" in valid_choices:
            return sorted(available.keys())
        print(f"  [erro] escolha invalida. Digite uma das opcoes: {', '.join(valid_choices)}.")




# ══════════════════════════════════════════════════════════════════════════
# UTILITARIOS DE CARGA E ESTILIZACAO PROFISSIONAL
# ══════════════════════════════════════════════════════════════════════════

# Paleta Coesa Acadêmica / Dissertação
COLOR_PRIMARY = "#1565C0"      # Azul Safira (KeyGen / X25519)
COLOR_SUCCESS = "#2E7D32"      # Verde Esmeralda (Sign / Memoria / OK)
COLOR_ACCENT = "#EF6C00"       # Laranja Âmbar (Verify / P95)
COLOR_DANGER = "#C62828"       # Vermelho Carmim (Handshake / Decrypt / P99)
COLOR_PURPLE = "#6A1B9A"       # Roxo Real (HKDF)
COLOR_SLATE = "#37474F"        # Grafite Escuro
COLOR_BOX_FILL = "#E3F2FD"     # Azul Pastel Claro
COLOR_GRID = "#CFD8DC"         # Cinza Claro para Grade

plt.rcParams.update({
    "font.sans-serif": ["DejaVu Sans", "Helvetica", "Arial", "Liberation Sans"],
    "font.family": "sans-serif",
    "axes.titlesize": 11,
    "axes.titleweight": "bold",
    "axes.labelsize": 9.5,
    "axes.labelweight": "semibold",
    "xtick.labelsize": 8.5,
    "ytick.labelsize": 8.5,
    "legend.fontsize": 8.5,
    "grid.color": COLOR_GRID,
    "grid.linestyle": "--",
    "grid.linewidth": 0.6,
    "grid.alpha": 0.6,
})


def format_bytes_human(b: int) -> str:
    """Converte bytes para string legivel (ex.: 64 B, 1 KB, 64 KB)."""
    if b >= 1024 * 1024:
        return f"{b // (1024 * 1024)} MB"
    elif b >= 1024:
        return f"{b // 1024} KB"
    return f"{b} B"


def beautify_ax(ax, grid="both"):
    """Remove bordas superior e direita e aplica grade suave."""
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color("#90A4AE")
    ax.spines["bottom"].set_color("#90A4AE")
    ax.spines["left"].set_linewidth(0.8)
    ax.spines["bottom"].set_linewidth(0.8)
    if grid == "both":
        ax.grid(True, linestyle="--", alpha=0.5, color=COLOR_GRID)
    elif grid == "y":
        ax.grid(True, axis="y", linestyle="--", alpha=0.5, color=COLOR_GRID)
        ax.grid(False, axis="x")
    elif grid == "x":
        ax.grid(True, axis="x", linestyle="--", alpha=0.5, color=COLOR_GRID)
        ax.grid(False, axis="y")


def load_run_csv(run_dir: Path):
    path = run_dir / "resultados_benchmark.csv"
    if not path.is_file():
        print(f"  [aviso] {path} ausente, pulando rodada.")
        return None
    df = pd.read_csv(path)
    df["label"] = df["label"].astype(str).str.strip().str.strip('"')
    return df


def savefig(fig, out_dir: Path, name: str):
    fig.tight_layout()
    path = out_dir / name
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"  [ok] {path}")


SEC_BITS_RE = r"\((\d+)-bit sec"
SEC_BITS_CONTAINS_RE = r"\(\d+-bit sec"
AEAD_RE = r"CC20-Poly1305 (Enc|Dec) (\d+) B \(throughput\)"
AEAD_CONTAINS_RE = r"CC20-Poly1305 (?:Enc|Dec) \d+ B \(throughput\)"

ED25519_LABELS = ["Ed25519 KeyGen (isolado)", "Ed25519 Sign (isolado)", "Ed25519 Verify (isolado)"]
HKDF_LABEL = "HKDF-SHA512 isolado (32B IKM -> 32B OKM)"
PROTOCOL_LABEL = "Custo amortizado (N=1 msgs)"  # handshake + 1 msg == "protocolo hibrido completo"


# ══════════════════════════════════════════════════════════════════════════
# GRAFICOS POR RODADA (prefixo <RR>_)
# ══════════════════════════════════════════════════════════════════════════

def run_plot_scalability(df: pd.DataFrame, rr: str, out_dir: Path):
    sub = df[df["label"].str.contains(SEC_BITS_CONTAINS_RE, regex=True) & ~df["label"].str.contains("peso Hamming")].copy()
    if sub.empty:
        return
    sub["sec_bits"] = sub["label"].str.extract(SEC_BITS_RE)[0].astype(int)
    sub = sub.sort_values("sec_bits")
    fig, ax = plt.subplots(figsize=(8.5, 5))
    ax.plot(sub["sec_bits"], sub["mean_us"], "o-", linewidth=2.2, color=COLOR_PRIMARY, markersize=7, label="Tempo Médio da Escada (k·P)")
    ax.fill_between(sub["sec_bits"], sub["mean_us"], alpha=0.12, color=COLOR_PRIMARY)
    for _, row in sub.iterrows():
        ax.annotate(f"{row['mean_us']:.1f} µs", (row["sec_bits"], row["mean_us"]),
                    textcoords="offset points", xytext=(0, 7), ha="center", fontsize=8, fontweight="semibold", color="#0D47A1")
    ax.set_xticks(sub["sec_bits"])
    ax.set_xlabel("Nível de Segurança / Tamanho do Campo Escalar (bits)", labelpad=8)
    ax.set_ylabel("Tempo Médio de Execução (µs)", labelpad=8)
    ax.set_title(f"Escalabilidade X25519: Escada de Montgomery vs. Tamanho do Campo — Rodada {rr}", pad=12)
    beautify_ax(ax, "both")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"{rr}_escalabilidade_x25519.png")


def run_plot_ed25519_128bit(df: pd.DataFrame, rr: str, out_dir: Path):
    sub = df[df["label"].isin(ED25519_LABELS)].set_index("label").reindex(ED25519_LABELS).dropna(how="all")
    if sub.empty:
        return
    fig, ax = plt.subplots(figsize=(7.5, 5.2))
    labels_clean = ["KeyGen\n(Geração de Chaves)", "Sign\n(Assinatura)", "Verify\n(Verificação)"]
    bars = ax.bar(labels_clean, sub["mean_us"], color=[COLOR_PRIMARY, COLOR_SUCCESS, COLOR_ACCENT],
                  edgecolor="black", linewidth=0.6, width=0.55)
    for bar in bars:
        h = bar.get_height()
        ax.annotate(f"{h:.1f} µs", (bar.get_x() + bar.get_width() / 2, h),
                    textcoords="offset points", xytext=(0, 5), ha="center", fontsize=9, fontweight="bold", color="#263238")
    ax.set_xlabel("Operação de Assinatura Digital (Ed25519)", labelpad=8)
    ax.set_ylabel("Tempo Médio de Execução (µs)", labelpad=8)
    ax.set_title(f"Ed25519 (RFC 8032 - Nível 128 bits): Latência das Operações — Rodada {rr}", pad=12)
    ax.set_ylim(0, sub["mean_us"].max() * 1.18)
    beautify_ax(ax, "y")
    savefig(fig, out_dir, f"{rr}_ed25519_128bit.png")


def _aead_rows(df: pd.DataFrame) -> pd.DataFrame:
    sub = df[df["label"].str.contains(AEAD_CONTAINS_RE, regex=True)].copy()
    extracted = sub["label"].str.extract(AEAD_RE)
    sub["op"] = extracted[0]
    sub["payload_b"] = extracted[1].astype(int)
    return sub.sort_values(["op", "payload_b"])


def run_plot_aead_time_vs_payload(df: pd.DataFrame, rr: str, out_dir: Path):
    sub = _aead_rows(df)
    if sub.empty:
        return
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    styles = {
        "Enc": {"fmt": "o-", "color": COLOR_PRIMARY, "label": "Cifração + Autenticação (Encrypt)"},
        "Dec": {"fmt": "s--", "color": COLOR_DANGER, "label": "Decifração + Verificação (Decrypt)"},
    }
    payloads = sorted(sub["payload_b"].unique())
    for op in ["Enc", "Dec"]:
        g = sub[sub["op"] == op]
        if not g.empty:
            ax.plot(g["payload_b"], g["mean_us"], styles[op]["fmt"],
                    label=styles[op]["label"], linewidth=2.2, color=styles[op]["color"], markersize=6)
    ax.set_xscale("log", base=2)
    ax.set_xticks(payloads)
    ax.set_xticklabels([format_bytes_human(b) for b in payloads], rotation=30, ha="right")
    ax.set_xlabel("Tamanho do Payload de Dados (Bytes)", labelpad=6)
    ax.set_ylabel("Tempo Médio de Execução (µs)", labelpad=8)
    ax.set_title(f"ChaCha20-Poly1305 (RFC 8439 - AEAD): Latência vs. Tamanho do Payload — Rodada {rr}", pad=12)
    beautify_ax(ax, "both")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"{rr}_aead_tempo_vs_payload.png")


def run_plot_aead_throughput(df: pd.DataFrame, rr: str, out_dir: Path):
    sub = _aead_rows(df)
    sub = sub[sub["mean_us"] > 0].copy()
    if sub.empty:
        return
    sub["mb_per_s"] = (sub["payload_b"] / 1e6) / (sub["mean_us"] / 1e6)
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    styles = {
        "Enc": {"fmt": "o-", "color": COLOR_PRIMARY, "label": "Taxa de Cifração (Encrypt)"},
        "Dec": {"fmt": "s--", "color": COLOR_DANGER, "label": "Taxa de Decifração (Decrypt)"},
    }
    payloads = sorted(sub["payload_b"].unique())
    for op in ["Enc", "Dec"]:
        g = sub[sub["op"] == op]
        if not g.empty:
            ax.plot(g["payload_b"], g["mb_per_s"], styles[op]["fmt"],
                    label=styles[op]["label"], linewidth=2.2, color=styles[op]["color"], markersize=6)
    ax.set_xscale("log", base=2)
    ax.set_xticks(payloads)
    ax.set_xticklabels([format_bytes_human(b) for b in payloads], rotation=30, ha="right")
    ax.set_xlabel("Tamanho do Payload de Dados (Bytes)", labelpad=6)
    ax.set_ylabel("Throughput / Taxa Efetiva de Transferência (MB/s)", labelpad=8)
    ax.set_title(f"ChaCha20-Poly1305 (RFC 8439 - AEAD): Throughput Efetivo vs. Payload — Rodada {rr}", pad=12)
    beautify_ax(ax, "both")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"{rr}_aead_throughput.png")


# --- Extras por rodada: boxplot de timing/Hamming e tabelas de RFC/robustez/memoria.
# Le arquivos diretamente da pasta run_NN (nao vem do resultados_benchmark.csv),
# entao so sao gerados se a rodada tiver sido produzida pela versao atual da
# suite (que exporta timing_hamming.csv/rfc_tests.csv/robustness_tests.csv/memory_usage.csv).

def run_plot_boxplot_hamming(run_dir: Path, rr: str, out_dir: Path):
    path = run_dir / "timing_hamming.csv"
    if not path.is_file():
        return
    df = pd.read_csv(path)
    order = df.drop_duplicates("hamming_weight").sort_values("hamming_weight")["classe"].tolist()
    data = [df[df["classe"] == c]["sample_us"].values for c in order]
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    try:
        ax.boxplot(data, tick_labels=order, patch_artist=True,
                   boxprops={"facecolor": COLOR_BOX_FILL, "edgecolor": COLOR_PRIMARY, "linewidth": 1.2},
                   medianprops={"color": "#0D47A1", "linewidth": 2},
                   whiskerprops={"color": COLOR_PRIMARY, "linewidth": 1.2},
                   capprops={"color": COLOR_PRIMARY, "linewidth": 1.2},
                   flierprops={"marker": "x", "markeredgecolor": COLOR_DANGER, "markersize": 5, "alpha": 0.5})
    except TypeError:
        ax.boxplot(data, labels=order, patch_artist=True,
                   boxprops={"facecolor": COLOR_BOX_FILL, "edgecolor": COLOR_PRIMARY, "linewidth": 1.2},
                   medianprops={"color": "#0D47A1", "linewidth": 2},
                   whiskerprops={"color": COLOR_PRIMARY, "linewidth": 1.2},
                   capprops={"color": COLOR_PRIMARY, "linewidth": 1.2},
                   flierprops={"marker": "x", "markeredgecolor": COLOR_DANGER, "markersize": 5, "alpha": 0.5})
    ax.set_xlabel("Classe de Peso de Hamming do Escalar k (255 bits)", labelpad=8)
    ax.set_ylabel("Tempo da Escada de Montgomery (µs)", labelpad=8)
    ax.set_title(f"Resistência a Canal Lateral de Tempo (Timing Attack): X25519 — Rodada {rr}", pad=12)
    ax.text(0.5, 0.02, "Nota: Dispersões homogêneas e medianas alinhadas evidenciam execução em tempo constante.",
            transform=ax.transAxes, ha="center", fontsize=8, style="italic",
            bbox=dict(boxstyle="round,pad=0.4", facecolor="#FFF9C4", edgecolor="#FBC02D", alpha=0.8))
    beautify_ax(ax, "y")
    savefig(fig, out_dir, f"{rr}_boxplot_timing_hamming.png")


def _table_figure(df: pd.DataFrame, title: str) -> plt.Figure:
    df = df.fillna("")
    cols = list(df.columns)

    if "test" in cols and "status" in cols:
        if "detail" in cols:
            col_widths = [0.55, 0.12, 0.33]
        else:
            col_widths = [0.80, 0.20]
    elif "metric" in cols:
        col_widths = [0.50, 0.25, 0.25]
    else:
        col_widths = [1.0 / len(cols)] * len(cols)

    num_rows = len(df)
    fig_height = max(2.2, 0.42 * (num_rows + 1) + 0.8)
    fig_width = 11.5
    fig, ax = plt.subplots(figsize=(fig_width, fig_height))
    ax.axis("off")
    ax.set_title(title, fontsize=12, pad=14, fontweight="bold", color="#263238")

    tbl = ax.table(cellText=df.values, colLabels=cols, colWidths=col_widths,
                   bbox=[0.02, 0.05, 0.96, 0.82], cellLoc="left")
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(9.5)

    status_col_idx = cols.index("status") if "status" in cols else -1

    for (row, col), cell in tbl.get_celld().items():
        cell.set_edgecolor("#CFD8DC")
        cell.set_linewidth(0.8)
        if row == 0:
            cell.set_facecolor("#0D47A1")
            cell.set_text_props(color="white", weight="bold", ha="center" if col == status_col_idx else "left")
        else:
            cell.set_facecolor("#FFFFFF" if row % 2 != 0 else "#F8F9FA")
            cell.set_text_props(color="#263238")
            if col == status_col_idx and row - 1 < num_rows:
                status = str(df.iloc[row - 1]["status"]).strip().upper()
                if status == "PASS":
                    cell.set_facecolor("#E8F5E9")
                    cell.set_text_props(weight="bold", color="#1B5E20", ha="center")
                elif status == "FAIL":
                    cell.set_facecolor("#FFEBEE")
                    cell.set_text_props(weight="bold", color="#B71C1C", ha="center")
    return fig


def run_plot_table_memoria(run_dir: Path, rr: str, out_dir: Path):
    path = run_dir / "memory_usage.csv"
    if not path.is_file():
        return
    df = pd.read_csv(path)
    fig = _table_figure(df, f"Pico de Memória RAM Ocupada no SO (getrusage / procfs) — Rodada {rr}")
    savefig(fig, out_dir, f"{rr}_table_memoria.png")


def run_plot_table_rfc(run_dir: Path, rr: str, out_dir: Path):
    path = run_dir / "rfc_tests.csv"
    if not path.is_file():
        return
    df = pd.read_csv(path)
    detail = df["detail"].fillna("").astype(str)
    df["detail"] = detail.str.slice(0, 32) + detail.str.len().gt(32).map({True: "...", False: ""})
    fig = _table_figure(df, f"Conformidade com RFCs (RFC 8032 / 7748 / 5869 / 8439) — Rodada {rr}")
    savefig(fig, out_dir, f"{rr}_table_rfc.png")


def run_plot_table_robustez(run_dir: Path, rr: str, out_dir: Path):
    path = run_dir / "robustness_tests.csv"
    if not path.is_file():
        return
    df = pd.read_csv(path)
    fig = _table_figure(df, f"Testes Negativos de Robustez e Rejeição de Entradas Inválidas — Rodada {rr}")
    savefig(fig, out_dir, f"{rr}_table_robustez.png")


def run_plot_handshake_vs_mensagens(run_dir: Path, rr: str, out_dir: Path):
    path = run_dir / "amortized_breakdown.csv"
    if not path.is_file():
        return
    df = pd.read_csv(path).sort_values("n_msgs")
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    x = [f"N={n}" for n in df["n_msgs"]]
    ax.bar(x, df["handshake_us_mean"], label="Handshake Inicial (X25519 + Ed25519 + HKDF)",
           color=COLOR_DANGER, edgecolor="black", linewidth=0.5, width=0.55)
    ax.bar(x, df["msgs_us_mean"], bottom=df["handshake_us_mean"], label="Custo de N Mensagens (ChaCha20-Poly1305)",
           color=COLOR_PRIMARY, edgecolor="black", linewidth=0.5, width=0.55)
    for i, (total, hs) in enumerate(zip(df["total_us_mean"], df["handshake_us_mean"])):
        hs_pct = (hs / total) * 100.0 if total > 0 else 0
        ax.annotate(f"{total:.0f} µs\n(HS: {hs_pct:.0f}%)", (i, total),
                    textcoords="offset points", xytext=(0, 5), ha="center", fontsize=8, fontweight="bold", color="#263238")
    ax.set_xlabel("Volume de Mensagens por Sessão Híbrida (N)", labelpad=8)
    ax.set_ylabel("Tempo Médio Total Acumulado da Sessão (µs)", labelpad=8)
    ax.set_title(f"Decomposição de Custo da Sessão: Handshake vs. Mensagens — Rodada {rr}", pad=12)
    ax.set_ylim(0, df["total_us_mean"].max() * 1.22)
    beautify_ax(ax, "y")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"{rr}_handshake_vs_mensagens.png")


def run_plot_overview_geral(df: pd.DataFrame, rr: str, out_dir: Path):
    sub = df[~df["label"].str.contains("peso Hamming")].sort_values("mean_us", ascending=True)
    if sub.empty:
        return
    fig, ax = plt.subplots(figsize=(10.5, max(6.5, 0.32 * len(sub))))
    bars = ax.barh(sub["label"], sub["mean_us"], color=COLOR_SLATE, edgecolor="black", linewidth=0.4, height=0.65)
    ax.set_xscale("log")
    for bar in bars:
        w = bar.get_width()
        ax.annotate(f" {w:.1f} µs", (w, bar.get_y() + bar.get_height() / 2),
                    va="center", fontsize=8, fontweight="semibold", color="#263238")
    ax.set_xlabel("Tempo Médio de Execução (µs, escala logarítmica)", labelpad=8)
    ax.set_ylabel("Operação Criptográfica Avaliada", labelpad=8)
    ax.set_title(f"Visão Comparativa Geral de Todas as Operações Medidas — Rodada {rr}", pad=14)
    beautify_ax(ax, "x")
    savefig(fig, out_dir, f"{rr}_overview_geral.png")


def run_plot_memoria_por_nivel(run_dir: Path, rr: str, out_dir: Path):
    path = run_dir / "memory_by_level.csv"
    if not path.is_file():
        return
    df = pd.read_csv(path).sort_values("sec_bits")
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.plot(df["sec_bits"], df["vmhwm_kb"], "o-", linewidth=2.2, color=COLOR_SUCCESS, markersize=7)
    for _, row in df.iterrows():
        ax.annotate(f"{row['vmhwm_kb']} KB", (row["sec_bits"], row["vmhwm_kb"]),
                    textcoords="offset points", xytext=(0, 7), ha="center", fontsize=8.5, fontweight="semibold", color="#1B5E20")
    ax.set_xticks(df["sec_bits"])
    ax.set_xlabel("Nível de Segurança / Tamanho do Campo Simulado (bits)", labelpad=8)
    ax.set_ylabel("Pico de Memória RAM Ocupada (KB, VmHWM - procfs)", labelpad=8)
    ax.set_title(f"Consumo de Memória RAM vs. Nível de Segurança da Aritmética — Rodada {rr}", pad=12)
    beautify_ax(ax, "both")
    if df["vmhwm_kb"].max() - df["vmhwm_kb"].min() < 0.05 * df["vmhwm_kb"].mean():
        ax.text(0.5, 0.02, "Nota: Variação desprezível — o footprint de memória é dominado pelo runtime do SO e não pela aritmética GMP.",
                transform=ax.transAxes, ha="center", fontsize=8, style="italic",
                bbox=dict(boxstyle="round,pad=0.4", facecolor="#E8F5E9", edgecolor="#A5D6A7", alpha=0.9))
    savefig(fig, out_dir, f"{rr}_memoria_por_nivel.png")


RUN_PLOTS = [
    run_plot_scalability,
    run_plot_ed25519_128bit,
    run_plot_aead_time_vs_payload,
    run_plot_aead_throughput,
    run_plot_overview_geral,
]
RUN_EXTRA_PLOTS = [
    run_plot_boxplot_hamming,
    run_plot_table_memoria,
    run_plot_table_rfc,
    run_plot_table_robustez,
    run_plot_handshake_vs_mensagens,
    run_plot_memoria_por_nivel,
]



# ══════════════════════════════════════════════════════════════════════════
# GRAFICOS COMPARATIVOS (prefixo comp_<NN>_)
# calculados a partir das rodadas selecionadas (mean/std entre rodadas)
# ══════════════════════════════════════════════════════════════════════════

def comp_plot_protocol_trend(per_run: dict, out_dir: Path, idx: int):
    rows = []
    for run_n, df in per_run.items():
        r = df[df["label"] == PROTOCOL_LABEL]
        if not r.empty:
            rows.append((run_n, r["mean_us"].iloc[0], r["p95_us"].iloc[0], r["p99_us"].iloc[0]))
    if not rows:
        print(f"  [aviso] label '{PROTOCOL_LABEL}' nao encontrada — pulando comp_{idx:02d}_protocolo_hibrido_trend.")
        return
    rows.sort(key=lambda t: t[0])
    runs, means, p95s, p99s = zip(*rows)
    fig, ax = plt.subplots(figsize=(9, 5.2))
    ax.plot(runs, means, "o-", label="Tempo Médio", linewidth=2.2, color=COLOR_PRIMARY, markersize=6)
    ax.plot(runs, p95s, "s--", label="Percentil 95 (P95)", linewidth=2.0, color=COLOR_ACCENT, markersize=5.5)
    ax.plot(runs, p99s, "^:", label="Percentil 99 (P99)", linewidth=2.0, color=COLOR_DANGER, markersize=6)
    overall_mean = sum(means) / len(means)
    ax.axhline(overall_mean, color="#78909C", linestyle=":", linewidth=1.2, label=f"Média Geral ({overall_mean:.1f} µs)")
    ax.set_xlabel("Número da Macro-Rodada (Run)", labelpad=8)
    ax.set_ylabel("Tempo de Execução (µs)", labelpad=8)
    ax.set_title("Protocolo Híbrido Completo (Handshake + 1 Msg): Tendência e Dispersão entre Rodadas", pad=12)
    ax.set_xticks(runs)
    beautify_ax(ax, "both")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"comp_{idx:02d}_protocolo_hibrido_trend.png")


def comp_plot_scalability(per_run: dict, out_dir: Path, idx: int):
    all_df = pd.concat([df.assign(run=n) for n, df in per_run.items()], ignore_index=True)
    sub = all_df[all_df["label"].str.contains(SEC_BITS_CONTAINS_RE, regex=True) & ~all_df["label"].str.contains("peso Hamming")].copy()
    if sub.empty:
        print(f"  [aviso] sem dados de escalabilidade — pulando comp_{idx:02d}_escalabilidade_x25519.")
        return
    sub["sec_bits"] = sub["label"].str.extract(SEC_BITS_RE)[0].astype(int)
    agg = sub.groupby("sec_bits")["mean_us"].agg(["mean", "std"]).reset_index().sort_values("sec_bits")
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.errorbar(agg["sec_bits"], agg["mean"], yerr=agg["std"].fillna(0), marker="o",
                linewidth=2.2, capsize=5, capthick=1.2, color=COLOR_PRIMARY, markersize=6,
                label=f"Média agregada (N={len(per_run)} rodadas) ± 1σ")
    for _, row in agg.iterrows():
        ax.annotate(f"{row['mean']:.1f} µs", (row["sec_bits"], row["mean"]),
                    textcoords="offset points", xytext=(0, 8), ha="center", fontsize=8.5, fontweight="semibold", color="#0D47A1")
    ax.set_xticks(agg["sec_bits"])
    ax.set_xlabel("Nível de Segurança / Tamanho do Campo Simulado (bits)", labelpad=8)
    ax.set_ylabel(f"Tempo Médio Agregado entre {len(per_run)} Rodada(s) (µs)", labelpad=8)
    ax.set_title("Comparativo entre Rodadas: Escalabilidade X25519 vs. Tamanho do Campo", pad=12)
    beautify_ax(ax, "both")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"comp_{idx:02d}_escalabilidade_x25519.png")


def _cross_run_stats(per_run: dict, labels: list) -> pd.DataFrame:
    all_df = pd.concat([df.assign(run=n) for n, df in per_run.items()], ignore_index=True)
    sub = all_df[all_df["label"].isin(labels)]
    return sub.groupby("label")["mean_us"].agg(["mean", "std"]).reindex(labels)


def comp_plot_latencias_gerais(per_run: dict, out_dir: Path, idx: int):
    labels = ED25519_LABELS + [HKDF_LABEL] + ["CC20-Poly1305 Enc 1024 B (throughput)", "CC20-Poly1305 Dec 1024 B (throughput)"]
    stats = _cross_run_stats(per_run, labels).dropna(how="all")
    if stats.empty:
        print(f"  [aviso] sem dados de latencias — pulando comp_{idx:02d}_latencias_gerais.")
        return
    short = [l.replace(" (isolado)", "").replace("HKDF-SHA512 isolado (32B IKM -> 32B OKM)", "HKDF-SHA512")
             .replace("CC20-Poly1305 ", "CC20-Poly1305\n").replace(" (throughput)", "") for l in stats.index]
    fig, ax = plt.subplots(figsize=(10.5, 5.5))
    bars = ax.bar(short, stats["mean"], yerr=stats["std"].fillna(0), capsize=5,
                  color=COLOR_SLATE, edgecolor="black", linewidth=0.5, width=0.55)
    for bar, (_, row) in zip(bars, stats.iterrows()):
        h = bar.get_height()
        s = row["std"] if pd.notna(row["std"]) else 0
        ax.annotate(f"{h:.1f} µs\n±{s:.1f}", (bar.get_x() + bar.get_width() / 2, h + s),
                    textcoords="offset points", xytext=(0, 4), ha="center", fontsize=8, fontweight="bold", color="#263238")
    ax.set_xlabel("Operação / Primitiva Criptográfica", labelpad=8)
    ax.set_ylabel(f"Tempo Médio Agregado entre {len(per_run)} Rodada(s) (µs) ± 1σ", labelpad=8)
    ax.set_title("Comparativo Geral de Latências: Ed25519 + HKDF-SHA512 + ChaCha20-Poly1305 (1024 B)", pad=14)
    ax.set_ylim(0, (stats["mean"] + stats["std"].fillna(0)).max() * 1.25)
    beautify_ax(ax, "y")
    savefig(fig, out_dir, f"comp_{idx:02d}_latencias_gerais.png")


def comp_plot_latencias_ed25519(per_run: dict, out_dir: Path, idx: int):
    stats = _cross_run_stats(per_run, ED25519_LABELS).dropna(how="all")
    if stats.empty:
        print(f"  [aviso] sem dados de Ed25519 — pulando comp_{idx:02d}_latencias_ed25519.")
        return
    labels = ["KeyGen\n(Geração)", "Sign\n(Assinatura)", "Verify\n(Verificação)"]
    fig, ax = plt.subplots(figsize=(7.5, 5.2))
    bars = ax.bar(labels, stats["mean"], yerr=stats["std"].fillna(0), capsize=6,
                  color=[COLOR_PRIMARY, COLOR_SUCCESS, COLOR_ACCENT], edgecolor="black", linewidth=0.5, width=0.55)
    for bar, (_, row) in zip(bars, stats.iterrows()):
        h = bar.get_height()
        s = row["std"] if pd.notna(row["std"]) else 0
        ax.annotate(f"{h:.1f} µs (±{s:.1f})", (bar.get_x() + bar.get_width() / 2, h + s),
                    textcoords="offset points", xytext=(0, 5), ha="center", fontsize=8.5, fontweight="bold", color="#263238")
    ax.set_xlabel("Operação de Assinatura Digital (Ed25519)", labelpad=8)
    ax.set_ylabel(f"Tempo Médio entre {len(per_run)} Rodada(s) (µs) ± 1σ", labelpad=8)
    ax.set_title("Ed25519 (RFC 8032): Estabilidade das Latências de Assinatura entre Rodadas", pad=12)
    ax.set_ylim(0, (stats["mean"] + stats["std"].fillna(0)).max() * 1.2)
    beautify_ax(ax, "y")
    savefig(fig, out_dir, f"comp_{idx:02d}_latencias_ed25519.png")


def comp_plot_latencias_hkdf(per_run: dict, out_dir: Path, idx: int):
    stats = _cross_run_stats(per_run, [HKDF_LABEL]).dropna(how="all")
    if stats.empty:
        print(f"  [aviso] sem dados de HKDF — pulando comp_{idx:02d}_latencias_hkdf.")
        return
    fig, ax = plt.subplots(figsize=(5.5, 5.2))
    bar = ax.bar(["HKDF-SHA512\n(32B IKM -> 32B OKM)"], stats["mean"], yerr=stats["std"].fillna(0), capsize=6,
                 color=COLOR_PURPLE, edgecolor="black", linewidth=0.5, width=0.45)
    h = stats["mean"].iloc[0]
    s = stats["std"].iloc[0] if pd.notna(stats["std"].iloc[0]) else 0
    ax.annotate(f"{h:.1f} µs (±{s:.1f})", (0, h + s),
                textcoords="offset points", xytext=(0, 6), ha="center", fontsize=9, fontweight="bold", color="#311B92")
    ax.set_ylabel(f"Tempo Médio entre {len(per_run)} Rodada(s) (µs) ± 1σ", labelpad=8)
    ax.set_title("HKDF-SHA512 (RFC 5869): Latência de Derivação de Chaves", pad=12)
    ax.set_ylim(0, (h + s) * 1.25)
    beautify_ax(ax, "y")
    savefig(fig, out_dir, f"comp_{idx:02d}_latencias_hkdf.png")


def comp_plot_latencias_chacha(per_run: dict, out_dir: Path, idx: int):
    all_df = pd.concat([df.assign(run=n) for n, df in per_run.items()], ignore_index=True)
    sub = all_df[all_df["label"].str.contains(AEAD_CONTAINS_RE, regex=True)].copy()
    if sub.empty:
        print(f"  [aviso] sem dados de ChaCha20-Poly1305 — pulando comp_{idx:02d}_latencias_chacha20poly1305.")
        return
    extracted = sub["label"].str.extract(AEAD_RE)
    sub["op"] = extracted[0]
    sub["payload_b"] = extracted[1].astype(int)
    agg = sub.groupby(["op", "payload_b"])["mean_us"].agg(["mean", "std"]).reset_index()
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    styles = {
        "Enc": {"style": "o-", "color": COLOR_PRIMARY, "label": "Cifração + Tag MAC (Encrypt)"},
        "Dec": {"style": "s--", "color": COLOR_DANGER, "label": "Decifração + Verificação (Decrypt)"},
    }
    payloads = sorted(agg["payload_b"].unique())
    for op in ["Enc", "Dec"]:
        g = agg[agg["op"] == op].sort_values("payload_b")
        if not g.empty:
            ax.errorbar(g["payload_b"], g["mean"], yerr=g["std"].fillna(0), fmt=styles[op]["style"],
                        label=styles[op]["label"], linewidth=2.2, capsize=4, color=styles[op]["color"], markersize=6)
    ax.set_xscale("log", base=2)
    ax.set_xticks(payloads)
    ax.set_xticklabels([format_bytes_human(b) for b in payloads], rotation=30, ha="right")
    ax.set_xlabel("Tamanho do Payload de Dados (Bytes)", labelpad=6)
    ax.set_ylabel(f"Tempo Médio entre {len(per_run)} Rodada(s) (µs) ± 1σ", labelpad=8)
    ax.set_title("ChaCha20-Poly1305 (RFC 8439 - AEAD): Latência Média vs. Payload entre Rodadas", pad=12)
    beautify_ax(ax, "both")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"comp_{idx:02d}_latencias_chacha20poly1305.png")


def comp_plot_aead_throughput(per_run: dict, out_dir: Path, idx: int):
    all_df = pd.concat([df.assign(run=n) for n, df in per_run.items()], ignore_index=True)
    sub = all_df[all_df["label"].str.contains(AEAD_CONTAINS_RE, regex=True)].copy()
    if sub.empty:
        print(f"  [aviso] sem dados de throughput ChaCha20-Poly1305 — pulando comp_{idx:02d}_aead_throughput.")
        return
    extracted = sub["label"].str.extract(AEAD_RE)
    sub["op"] = extracted[0]
    sub["payload_b"] = extracted[1].astype(int)
    sub = sub[sub["mean_us"] > 0].copy()
    sub["mb_per_s"] = (sub["payload_b"] / 1e6) / (sub["mean_us"] / 1e6)
    agg = sub.groupby(["op", "payload_b"])["mb_per_s"].agg(["mean", "std"]).reset_index()
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    styles = {
        "Enc": {"style": "o-", "color": COLOR_PRIMARY, "label": "Taxa de Cifração (Encrypt)"},
        "Dec": {"style": "s--", "color": COLOR_DANGER, "label": "Taxa de Decifração (Decrypt)"},
    }
    payloads = sorted(agg["payload_b"].unique())
    for op in ["Enc", "Dec"]:
        g = agg[agg["op"] == op].sort_values("payload_b")
        if not g.empty:
            ax.errorbar(g["payload_b"], g["mean"], yerr=g["std"].fillna(0), fmt=styles[op]["style"],
                        label=styles[op]["label"], linewidth=2.2, capsize=4, color=styles[op]["color"], markersize=6)
    ax.set_xscale("log", base=2)
    ax.set_xticks(payloads)
    ax.set_xticklabels([format_bytes_human(b) for b in payloads], rotation=30, ha="right")
    ax.set_xlabel("Tamanho do Payload de Dados (Bytes)", labelpad=6)
    ax.set_ylabel(f"Throughput Médio Efetivo entre {len(per_run)} Rodada(s) (MB/s) ± 1σ", labelpad=8)
    ax.set_title("ChaCha20-Poly1305 (RFC 8439 - AEAD): Throughput Médio Efetivo vs. Payload", pad=12)
    beautify_ax(ax, "both")
    ax.legend(loc="upper left", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"comp_{idx:02d}_aead_throughput.png")


def comp_plot_stability_cv(per_run: dict, out_dir: Path, idx: int):
    if len(per_run) < 2:
        print(f"  [aviso] menos de 2 rodadas — pulando comp_{idx:02d}_stability_cv (CV% exige variacao entre rodadas).")
        return
    all_df = pd.concat([df.assign(run=n) for n, df in per_run.items()], ignore_index=True)
    stab = all_df.groupby("label")["mean_us"].agg(["mean", "std"]).reset_index()
    stab["cv_pct"] = (stab["std"] / stab["mean"]).replace([float("inf"), float("-inf")], pd.NA) * 100.0
    stab = stab.dropna(subset=["cv_pct"]).sort_values("cv_pct", ascending=False)
    if stab.empty:
        print(f"  [aviso] sem dados suficientes — pulando comp_{idx:02d}_stability_cv.")
        return
    fig, ax = plt.subplots(figsize=(10.5, max(5.5, 0.34 * len(stab))))
    bar_colors = ["#2E7D32" if cv < 3.0 else "#EF6C00" if cv < 7.0 else "#C62828" for cv in stab["cv_pct"]]
    bars = ax.barh(stab["label"], stab["cv_pct"], color=bar_colors, edgecolor="black", linewidth=0.4, height=0.65)
    ax.invert_yaxis()
    for bar in bars:
        w = bar.get_width()
        ax.annotate(f" {w:.2f}%", (w, bar.get_y() + bar.get_height() / 2),
                    va="center", fontsize=8, fontweight="bold", color="#263238")
    ax.set_xlabel("Coeficiente de Variação - CV (%) = (Desvio Padrão / Média) × 100", labelpad=8)
    ax.set_ylabel("Operação Criptográfica Avaliada", labelpad=8)
    ax.set_title(f"Índice de Estabilidade Operacional entre {len(per_run)} Rodadas (CV%)\n(Menor valor indica maior determinismo e reprodutibilidade)", pad=14)
    beautify_ax(ax, "x")
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor="#2E7D32", label="Alta Estabilidade (CV < 3%)"),
        Patch(facecolor="#EF6C00", label="Estabilidade Moderada (3% ≤ CV < 7%)"),
        Patch(facecolor="#C62828", label="Maior Dispersão (CV ≥ 7%)"),
    ]
    ax.legend(handles=legend_elements, loc="lower right", frameon=True, framealpha=0.9)
    savefig(fig, out_dir, f"comp_{idx:02d}_stability_cv.png")


COMP_PLOT_CATALOG = [
    {"id": 1, "name": "Protocolo Hibrido Completo Trend (Media/P95/P99 entre rodadas)", "fn": comp_plot_protocol_trend},
    {"id": 2, "name": "Comparativo de Escalabilidade X25519", "fn": comp_plot_scalability},
    {"id": 3, "name": "Comparativo Geral de Latencias (Ed25519 + HKDF + ChaCha20)", "fn": comp_plot_latencias_gerais},
    {"id": 4, "name": "Comparativo entre Rodadas: Latencias Ed25519", "fn": comp_plot_latencias_ed25519},
    {"id": 5, "name": "Comparativo entre Rodadas: Latencia HKDF-SHA512", "fn": comp_plot_latencias_hkdf},
    {"id": 6, "name": "Comparativo entre Rodadas: Latencias ChaCha20-Poly1305 (Enc/Dec) vs. Payload", "fn": comp_plot_latencias_chacha},
    {"id": 7, "name": "Comparativo entre Rodadas: Throughput ChaCha20-Poly1305 (Enc/Dec) vs. Payload", "fn": comp_plot_aead_throughput},
    {"id": 8, "name": "Estabilidade por Operacao entre Rodadas (CV%)", "fn": comp_plot_stability_cv},
]
COMP_PLOTS = [item["fn"] for item in COMP_PLOT_CATALOG]


def export_condensed_analysis_csv(per_run: dict, out_dir: Path) -> Path:
    """Tabela condensada (uma linha por 'label') com o resumo estatistico das
    rodadas filtradas, pronta para colar na dissertacao. Nome do arquivo usa o
    intervalo real das rodadas presentes em `per_run` (ex.: rodadas 1..30 ->
    analise_intervalo_macro_1_a_30.csv)."""
    lo, hi = min(per_run), max(per_run)
    all_df = pd.concat([df.assign(run=n) for n, df in per_run.items()], ignore_index=True)
    agg_kwargs = {
        "rodadas": ("run", "nunique"),
        "mean_us": ("mean_us", "mean"),
        "std_us": ("mean_us", "std"),
        "min_us": ("mean_us", "min"),
        "max_us": ("mean_us", "max"),
    }
    if "p95_us" in all_df.columns:
        agg_kwargs["p95_us_mean"] = ("p95_us", "mean")
    if "p99_us" in all_df.columns:
        agg_kwargs["p99_us_mean"] = ("p99_us", "mean")
    summary = (all_df.groupby("label", as_index=False)
               .agg(**agg_kwargs)
               .sort_values("mean_us", ascending=False))
    summary["std_us"] = summary["std_us"].fillna(0.0)
    fname = f"analise_intervalo_macro_{lo}_a_{hi}.csv"
    out_path = out_dir / fname
    summary.to_csv(out_path, index=False)
    return out_path


# ══════════════════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="Plotagem interativa dos resultados full-system.")
    parser.add_argument("test_dir", nargs="?", help="Pasta dentro de benchmark_runs/ (ou caminho completo).")
    parser.add_argument("runs", nargs="?", help="Runs a plotar ('intervalo', '1,4,5', '1-5' ou '0').")
    parser.add_argument("--macro-start", type=int, help="Primeira macro-rodada do intervalo de tendencia de longo prazo.")
    parser.add_argument("--macro-end", type=int, help="Ultima macro-rodada do intervalo de tendencia de longo prazo.")
    parser.add_argument("--runs", "--runs-to-plot", dest="runs_to_plot",
                        help="Quais runs plotar: 'intervalo', '1,4,5', '1-5' ou '0'.")
    parser.add_argument("--comparison-scope", "--comp-scope", dest="comparison_scope",
                        help="Escopo dos graficos comparativos: '1'/'intervalo', '2'/'todas', '0'/'nenhum'.")
    parser.add_argument("--no-dissertacao", action="store_true",
                        help="Desativa a geracao automatica dos graficos e tabelas da dissertacao de mestrado.")
    parser.add_argument("--dissertacao-only", action="store_true",
                        help="Executa apenas a geracao de graficos e tabelas da dissertacao e encerra.")
    args = parser.parse_args()

    cli_test_dir = args.test_dir
    cli_runs = args.runs_to_plot or args.runs

    test_dir = ask_test_dir(cli_test_dir)

    if args.dissertacao_only:
        benchmarks_dir = Path(__file__).resolve().parent
        if str(benchmarks_dir) not in sys.path:
            sys.path.insert(0, str(benchmarks_dir))
        from postprocess_dissertacao import process_dissertacao_results
        process_dissertacao_results(test_dir, test_dir / "comparison")
        print("\n==> Pós-processamento da dissertação concluído com sucesso.")
        return

    runs_map = discover_runs(test_dir)
    if not runs_map:
        sys.exit(f"[ERRO] Nenhuma pasta run_NN encontrada em {test_dir}")

    # 1. Determina o intervalo das macros
    macro_runs = ask_macro_interval(
        runs_map,
        args.macro_start,
        args.macro_end,
        cli_runs if (args.macro_start is None and args.macro_end is None and cli_runs not in ("intervalo", "i")) else None
    )

    # 2. Pergunta quais runs plotar (intervalo, especificas ou 0)
    runs_to_plot = ask_runs_to_plot(runs_map, macro_runs, cli_runs)

    # 3. Pergunta escopo da comparacao (intervalo, todas ou 0)
    comparison_runs = ask_comparison_scope(runs_map, macro_runs, args.comparison_scope)

    if not runs_to_plot and not comparison_runs:
        print("\n[aviso] Nenhuma rodada selecionada para plotagem (opcao 0 selecionada para runs e comparacao).")
        return

    # Carrega apenas os dados das rodadas necessarias
    needed_runs = set(runs_to_plot) | set(comparison_runs)
    loaded_runs = {}
    for n in sorted(needed_runs):
        df = load_run_csv(runs_map[n])
        if df is not None:
            loaded_runs[n] = df

    # 4. Executa graficos por rodada
    if runs_to_plot:
        per_run = {n: loaded_runs[n] for n in runs_to_plot if n in loaded_runs}
        if not per_run:
            print("\n[aviso] Nenhuma rodada valida com resultados_benchmark.csv encontrada para as runs.")
        else:
            print(f"\n==> Gerando graficos POR RODADA para as runs: {sorted(per_run.keys())}")
            for n, df in per_run.items():
                rr = f"{n:02d}"
                run_out_dir = runs_map[n]
                for plot_fn in RUN_PLOTS:
                    plot_fn(df, rr, run_out_dir)
                for extra_fn in RUN_EXTRA_PLOTS:
                    extra_fn(runs_map[n], rr, run_out_dir)
    else:
        print("\n==> Graficos por rodada: ignorados (opcao 0 selecionada).")

    # 5. Executa graficos de comparacao
    if comparison_runs:
        comparison_out_dir = test_dir / "comparison"
        comparison_out_dir.mkdir(parents=True, exist_ok=True)
        comparison_per_run = {n: loaded_runs[n] for n in comparison_runs if n in loaded_runs}
        if not comparison_per_run:
            print("\n[aviso] Nenhuma rodada valida encontrada para os comparativos.")
        else:
            print(f"\n==> Gerando {len(COMP_PLOTS)} grafico(s) COMPARATIVO(S) entre {len(comparison_per_run)} rodada(s): "
                  f"{sorted(comparison_per_run.keys())}...")
            if len(comparison_per_run) < 2:
                print("  [aviso] apenas 1 rodada — comparativos serao triviais (sem variacao entre rodadas).")
            for idx, plot_fn in enumerate(COMP_PLOTS, start=1):
                plot_fn(comparison_per_run, comparison_out_dir, idx)

            analysis_csv = export_condensed_analysis_csv(comparison_per_run, comparison_out_dir)
            print(f"  [OK] Tabela condensada exportada: {analysis_csv}")
            print(f"==> Graficos comparativos salvos em: {comparison_out_dir}")

            if not getattr(args, "no_dissertacao", False):
                try:
                    benchmarks_dir = Path(__file__).resolve().parent
                    if str(benchmarks_dir) not in sys.path:
                        sys.path.insert(0, str(benchmarks_dir))
                    from postprocess_dissertacao import process_dissertacao_results
                    process_dissertacao_results(test_dir, comparison_out_dir, analysis_csv)
                except Exception as e:
                    print(f"  [aviso] Falha ao processar saídas da dissertação: {e}")
    else:
        print("\n==> Graficos comparativos: ignorados (opcao 0 selecionada).")

    print("\n==> Processo concluido com sucesso.")


if __name__ == "__main__":
    main()
