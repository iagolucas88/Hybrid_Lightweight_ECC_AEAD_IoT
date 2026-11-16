#!/usr/bin/env python3
"""
Gera gráficos de alta resolução para:
1. Primitivas isoladas e consumo de memória do SO (VmHWM / getrusage).
2. Amortização do protocolo híbrido para N=1, N=100 e N=1000 mensagens.
"""

import os
import shutil
from pathlib import Path

os.environ["MPLCONFIGDIR"] = "/tmp/matplotlib"
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Estilo e paleta consistente com a dissertação
C_PRIMARY = "#0D47A1"    # Azul Marinho
C_SECONDARY = "#1565C0"  # Azul Médio
C_CYAN = "#00838F"       # Ciano Escuro
C_GREEN = "#2E7D32"      # Verde Escuro
C_AMBER = "#E65100"      # Âmbar / Laranja
C_RED = "#C62828"        # Vermelho
C_SLATE = "#37474F"      # Cinza Ardósia
C_GRID = "#E0E0E0"

OUT_DIR_DOCS = Path("docs/plots")
OUT_DIR_DOCS.mkdir(parents=True, exist_ok=True)

ARTIFACT_DIR = Path("/home/iagol/.gemini/antigravity/brain/d1490cdd-8ad2-4583-b220-1f1af9e3b6b4")

# ==============================================================================
# GRÁFICO 1: PRIMITIVAS ISOLADAS E CONSUMO DE MEMÓRIA DO SO
# ==============================================================================
def plot_primitivas_e_memoria():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14.5, 6.4), gridspec_kw={"width_ratios": [1.25, 1.0]})
    fig.patch.set_facecolor("white")

    # --- SUBPLOT 1: Latência e Energia das Primitivas Isoladas ---
    primitives = [
        "Ed25519 Verify (64B)",
        "Ed25519 KeyGen",
        "Ed25519 Sign (32B)",
        "X25519 Ladder (255b)",
        "HKDF-SHA512 (32B)",
        "CC20-Poly1305 Enc (64B)",
        "CC20-Poly1305 Dec (64B)",
    ]
    primitives_inv = primitives[::-1]
    times_us = [16791.4, 9095.4, 8143.8, 5327.2, 51.2, 4.18, 4.15][::-1]
    energy_uj = [t * 0.500 for t in times_us]  # 500 mW @ 667 MHz

    colors = [C_GREEN, C_GREEN, C_CYAN, C_SECONDARY, C_AMBER, C_AMBER, C_RED]
    y_pos = np.arange(len(primitives_inv))

    bars = ax1.barh(y_pos, times_us, color=colors, height=0.58, edgecolor="#212121", linewidth=0.8, alpha=0.92)
    ax1.set_xscale("log")
    ax1.set_yticks(y_pos)
    ax1.set_yticklabels(primitives_inv, fontsize=9.5, fontweight="bold", color="#212121")
    ax1.set_xlabel("Tempo Médio de Execução (µs, escala logarítmica)", fontsize=10.5, fontweight="bold", labelpad=8)
    ax1.set_title("A) Desempenho e Custo das Primitivas Criptográficas Isoladas\n(ARM Cortex-A9 a 667 MHz — P_core = 500 mW)",
                  fontsize=11.5, fontweight="bold", pad=12, color="#0D47A1")

    ax1.grid(axis="x", which="both", linestyle="--", linewidth=0.5, color=C_GRID, alpha=0.7)
    ax1.set_xlim(1.0, 95000)

    for bar, t, e in zip(bars, times_us, energy_uj):
        w = bar.get_width()
        if t < 10:
            txt = f" {t:.2f} µs ({e:.2f} µJ)"
        elif t < 100:
            txt = f" {t:.1f} µs ({e:.1f} µJ)"
        else:
            txt = f" {t:,.0f} µs ({e:,.0f} µJ)"
        ax1.text(w * 1.15, bar.get_y() + bar.get_height() / 2, txt,
                 va="center", ha="left", fontsize=9, fontweight="bold", color="#263238")

    # --- SUBPLOT 2: Consumo de Memória do SO (Medição Real POSIX/Procfs) ---
    mem_categories = [
        "Aritmética X25519\n(Fork Isolado)",
        "VmHWM Procfs\n(Working Set RSS)",
        "VmPeak Procfs\n(Memória Virtual)",
        "ru_maxrss POSIX\n(getrusage)"
    ]
    mem_kb = [188, 1496, 1780, 3552]
    mem_mb = [v / 1024.0 for v in mem_kb]
    bar_colors = ["#43A047", "#1E88E5", "#00ACC1", "#FB8C00"]

    x_pos = np.arange(len(mem_categories))
    bars2 = ax2.bar(x_pos, mem_kb, color=bar_colors, width=0.48, edgecolor="#212121", linewidth=0.8, alpha=0.92)

    ax2.set_ylabel("Memória Ocupada no SO (Kilobytes — KB)", fontsize=10.5, fontweight="bold", labelpad=8)
    ax2.set_xticks(x_pos)
    ax2.set_xticklabels(mem_categories, fontsize=9.0, fontweight="bold", color="#212121", rotation=14, ha="right")
    ax2.set_title("B) Consumo Real de Memória RAM no Linux Convidado\n(Medição Estrita via /proc/self/status e getrusage)",
                  fontsize=11.5, fontweight="bold", pad=12, color="#0D47A1")
    ax2.grid(axis="y", linestyle="--", linewidth=0.5, color=C_GRID, alpha=0.7)
    ax2.set_ylim(0, 5200)

    for bar, kb, mb in zip(bars2, mem_kb, mem_mb):
        h = bar.get_height()
        ax2.annotate(f"{kb:,} KB\n({mb:.2f} MB)",
                     xy=(bar.get_x() + bar.get_width() / 2, h),
                     xytext=(0, 6), textcoords="offset points",
                     ha="center", va="bottom", fontsize=9.5, fontweight="bold", color="#263238")

    # Adicionar caixa de destaque de viabilidade em IoT restrita
    nota_viabilidade = (
        "Comprovação de Viabilidade em Dispositivos Restritos:\n"
        "• VmHWM real do processo completo: ~1,46 MB (working set físico)\n"
        "• Aritmética modular Curve25519 em execução: apenas 188 KB\n"
        "• Totalmente viável em SoCs/MPUs IoT (64 MB a 512 MB de RAM)\n"
        "• Ocupa apenas ~0,07% da memória em nós industriais de 2 GB"
    )
    ax2.text(0.04, 0.94, nota_viabilidade, transform=ax2.transAxes, ha="left", va="top",
             fontsize=8.5, fontweight="medium", color="#1B5E20",
             bbox=dict(boxstyle="round,pad=0.5", facecolor="#E8F5E9", edgecolor="#81C784", alpha=0.95))

    plt.tight_layout()
    out_file = OUT_DIR_DOCS / "demonstracao_01_primitivas_e_memoria.png"
    plt.savefig(out_file, dpi=300)
    plt.close()
    print(f"[OK] Gerado: {out_file}")

    if ARTIFACT_DIR.is_dir():
        shutil.copy(out_file, ARTIFACT_DIR / "demonstracao_01_primitivas_e_memoria.png")


# ==============================================================================
# GRÁFICO 2: AMORTIZAÇÃO PARA 1, 100 E 1000 MENSAGENS
# ==============================================================================
def plot_amortizacao_1_100_1000():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14.5, 6.4))
    fig.patch.set_facecolor("white")

    scenarios = ["N = 1 msg\n(Handshake Puro)", "N = 100 msgs\n(Ponto Ótimo)", "N = 1.000 msgs\n(Sessão Longa)"]

    # Dados consolidados das 50 macro-rodadas (50.000 amostras)
    amort_us = [49681.9, 504.4, 57.5]
    amort_energy_uj = [amort_us[0] * 0.500, amort_us[1] * 0.500, amort_us[2] * 0.500]

    # --- SUBPLOT 1: Custo Médio Amortizado por Mensagem (Queda Exponencial) ---
    x = np.arange(len(scenarios))
    colors = [C_RED, C_AMBER, C_GREEN]

    bars1 = ax1.bar(x, amort_us, width=0.48, color=colors, edgecolor="#212121", linewidth=0.8, alpha=0.92)
    ax1.set_yscale("log")
    ax1.set_ylabel("Custo Médio Amortizado por Mensagem (µs, log)", fontsize=10.5, fontweight="bold", labelpad=8)
    ax1.set_xticks(x)
    ax1.set_xticklabels(scenarios, fontsize=10, fontweight="bold", color="#212121")
    ax1.set_title("A) Custo Médio Amortizado por Mensagem Transmitida\n(Redução de 98,94% para N=100 e 99,88% para N=1000)",
                  fontsize=11.5, fontweight="bold", pad=12, color="#0D47A1")
    ax1.grid(axis="y", which="both", linestyle="--", linewidth=0.5, color=C_GRID, alpha=0.7)
    ax1.set_ylim(20, 250000)

    # Rótulos de tempo e economia percentual
    economias = ["Referência (0%)", "-98,94% de CPU", "-99,88% de CPU"]
    for bar, val, uj, econ in zip(bars1, amort_us, amort_energy_uj, economias):
        h = bar.get_height()
        label_txt = f"{val:,.1f} µs\n({uj:,.1f} µJ)\n[{econ}]"
        ax1.annotate(label_txt, xy=(bar.get_x() + bar.get_width() / 2, h),
                     xytext=(0, 7), textcoords="offset points",
                     ha="center", va="bottom", fontsize=9.5, fontweight="bold", color="#263238")

    # --- SUBPLOT 2: Decomposição do Tempo Acumulado da Sessão (Handshake vs. AEAD) ---
    hs_us = [49677.7, 49677.7, 49677.7]
    msgs_us = [4.2, 418.0, 4180.0]
    total_us = [h + m for h, m in zip(hs_us, msgs_us)]

    pct_hs = [(h / t) * 100.0 for h, t in zip(hs_us, total_us)]
    pct_msgs = [(m / t) * 100.0 for m, t in zip(msgs_us, total_us)]

    bars_hs = ax2.bar(x, hs_us, width=0.48, label="Handshake Assimétrico Inicial (Ed25519 + X25519 + HKDF)",
                      color="#E53935", edgecolor="#212121", linewidth=0.8, alpha=0.90)
    bars_msg = ax2.bar(x, msgs_us, bottom=hs_us, width=0.48, label="Custo Acumulado das N Mensagens (ChaCha20-Poly1305)",
                       color="#1E88E5", edgecolor="#212121", linewidth=0.8, alpha=0.90)

    ax2.set_ylabel("Tempo Total Acumulado da Sessão Criptográfica (µs)", fontsize=10.5, fontweight="bold", labelpad=8)
    ax2.set_xticks(x)
    ax2.set_xticklabels(scenarios, fontsize=10, fontweight="bold", color="#212121")
    ax2.set_title("B) Decomposição do Tempo Total da Sessão Criptográfica\n(Demonstração do Domínio do Handshake Inicial Diluído no Tempo)",
                  fontsize=11.5, fontweight="bold", pad=12, color="#0D47A1")
    ax2.grid(axis="y", linestyle="--", linewidth=0.5, color=C_GRID, alpha=0.7)
    ax2.set_ylim(0, 74000)

    for i, (tot, p_hs, p_m) in enumerate(zip(total_us, pct_hs, pct_msgs)):
        ax2.annotate(f"Total: {tot:,.0f} µs\n(HS: {p_hs:.2f}% | AEAD: {p_m:.2f}%)",
                     xy=(i, tot), xytext=(0, 6), textcoords="offset points",
                     ha="center", va="bottom", fontsize=9, fontweight="bold", color="#263238")

    ax2.legend(loc="upper left", frameon=True, facecolor="white", edgecolor="#CFD8DC", fontsize=9.0)

    plt.tight_layout()
    out_file = OUT_DIR_DOCS / "demonstracao_02_amortizacao_1_100_1000.png"
    plt.savefig(out_file, dpi=300)
    plt.close()
    print(f"[OK] Gerado: {out_file}")

    if ARTIFACT_DIR.is_dir():
        shutil.copy(out_file, ARTIFACT_DIR / "demonstracao_02_amortizacao_1_100_1000.png")


if __name__ == "__main__":
    plot_primitivas_e_memoria()
    plot_amortizacao_1_100_1000()
