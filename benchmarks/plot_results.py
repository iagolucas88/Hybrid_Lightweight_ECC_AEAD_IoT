#!/usr/bin/env python3
"""
Gera automaticamente todos os gráficos do benchmark de criptografia IoT.
Uso: python3 plot_results.py [benchmark_results.csv]
"""
import sys
import pandas as pd
import matplotlib
matplotlib.use("Agg")          # sem display (QEMU / CI)
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
from pathlib import Path

default_csv = Path("benchmark_results.csv")
if not default_csv.exists():
    default_csv = Path(__file__).resolve().parent.parent / "benchmark_results.csv"
CSV  = Path(sys.argv[1]) if len(sys.argv) > 1 else default_csv
OUT  = Path("plots")
OUT.mkdir(exist_ok=True)

df = pd.read_csv(CSV)
df["label"] = df["label"].str.strip().str.strip('"')


# ── helper ────────────────────────────────────────────────────────────────────
def savefig(name):
    p = OUT / name
    plt.tight_layout()
    plt.savefig(p, dpi=150)
    plt.close()
    print(f"  → {p}")


def errbar(ax, row, color):
    ax.errorbar(row["label"], row["mean_us"],
                yerr=row["std_us"], fmt="o", color=color,
                capsize=4, capthick=1.5, elinewidth=1.5)


# ── 1. Montgomery ladder: custo vs. nível de segurança ───────────────────────
mont = df[df["label"].str.contains("Curve25519")].copy()
fig, ax = plt.subplots(figsize=(9, 5))
ax.bar(mont["label"], mont["mean_us"] / 1e3, color="steelblue", edgecolor="k", linewidth=0.6)
ax.errorbar(range(len(mont)), mont["mean_us"] / 1e3,
            yerr=mont["std_us"] / 1e3, fmt="none", ecolor="black",
            capsize=5, capthick=1.5, elinewidth=1.5)
ax.set_ylabel("Tempo médio (ms)")
ax.set_title("Escada de Montgomery — Custo vs. Nível de Segurança\n"
             "(Curve25519 / X25519, k·Gu por iteração)")
ax.set_xticks(range(len(mont)))
ax.set_xticklabels(
    [l.replace("Curve25519-like (", "").replace("Curve25519 (", "")
      .replace(" [real]", "★").replace(")", "")
     for l in mont["label"]], rotation=18, ha="right", fontsize=8)
ax.yaxis.set_major_formatter(ticker.FormatStrFormatter("%.1f"))
ax.grid(axis="y", linestyle="--", alpha=0.5)
savefig("01_montgomery_levels.png")

# ── 2. Montgomery: escala log-log (field_bits vs. mean_us) ───────────────────
bits_map = {"160": 160, "224": 224, "255": 255, "384": 384, "448": 448, "521": 521}
fb = [int(s.split("/")[1].split("-")[0].strip()) for s in mont["label"]]
fig, ax = plt.subplots(figsize=(7, 5))
ax.loglog(fb, mont["mean_us"], "o-", color="steelblue", linewidth=2, markersize=7)
# referência O(n^2)
x = np.array(fb)
ref = mont["mean_us"].iloc[0] * (x / x[0]) ** 2
ax.loglog(x, ref, "--", color="gray", linewidth=1, label="O(n²) referência")
ax.set_xlabel("Tamanho do campo (bits)")
ax.set_ylabel("Tempo médio (μs) — escala log")
ax.set_title("Complexidade da Escada de Montgomery\n(log-log: tempo × tamanho do primo)")
ax.legend()
ax.grid(True, which="both", linestyle="--", alpha=0.4)
savefig("02_montgomery_loglog.png")

# ── 3. Ed25519: KeyGen / Sign / Verify ───────────────────────────────────────
ed = df[df["label"].str.contains("Ed25519")].copy()
colors = ["#2196F3", "#FF5722", "#4CAF50"]
fig, ax = plt.subplots(figsize=(7, 5))
bars = ax.bar(ed["label"].str.extract(r"(KeyGen|Sign|Verify)")[0],
              ed["mean_us"] / 1e3,
              color=colors, edgecolor="k", linewidth=0.6, width=0.5)
ax.errorbar(range(len(ed)), ed["mean_us"] / 1e3,
            yerr=ed["std_us"] / 1e3,
            fmt="none", ecolor="black", capsize=5, capthick=1.5, elinewidth=1.5)
ax.set_ylabel("Tempo médio (ms)")
ax.set_title("Edwards25519 / Ed25519 — KeyGen | Sign | Verify (128-bit)")
ax.grid(axis="y", linestyle="--", alpha=0.5)
for bar, row in zip(bars, ed.itertuples()):
    ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.05,
            f"{row.mean_us/1e3:.1f} ms", ha="center", va="bottom", fontsize=9)
savefig("03_ed25519.png")

# ── 4. HKDF-SHA512 ────────────────────────────────────────────────────────────
hkdf = df[df["label"].str.contains("HKDF")].iloc[0]
fig, ax = plt.subplots(figsize=(5, 4))
ax.bar(["HKDF-SHA512"], [hkdf["mean_us"]], color="#9C27B0",
       edgecolor="k", linewidth=0.6, width=0.3)
ax.errorbar([0], [hkdf["mean_us"]], yerr=[hkdf["std_us"]],
            fmt="none", ecolor="black", capsize=5, capthick=1.5)
ax.set_ylabel("Tempo médio (μs)")
ax.set_title("HKDF-SHA512  (32 B IKM → 32 B OKM)")
ax.grid(axis="y", linestyle="--", alpha=0.5)
ax.text(0, hkdf["mean_us"] + hkdf["std_us"] * 1.2,
        f'{hkdf["mean_us"]:.1f} μs', ha="center", va="bottom", fontsize=10)
savefig("04_hkdf.png")

# ── 5. ChaCha20-Poly1305: Enc e Dec por payload ───────────────────────────────
enc = df[df["label"].str.contains("Enc")].copy()
dec = df[df["label"].str.contains("Dec")].copy()

enc["payload_bytes"] = pd.to_numeric(
    enc["label"].str.extract(r"(\d+)\s*B")[0], errors="coerce"
)
dec["payload_bytes"] = pd.to_numeric(
    dec["label"].str.extract(r"(\d+)\s*B")[0], errors="coerce"
)
enc = enc.dropna(subset=["payload_bytes"]).sort_values("payload_bytes")
dec = dec.dropna(subset=["payload_bytes"]).sort_values("payload_bytes")
dec = dec.set_index("payload_bytes").reindex(enc["payload_bytes"]).reset_index()

sizes = enc["payload_bytes"].astype(int).tolist()

fig, ax = plt.subplots(figsize=(9, 5))
x = np.arange(len(sizes))
w = 0.35
ax.bar(x - w/2, enc["mean_us"], w, label="Encrypt", color="#03A9F4", edgecolor="k", linewidth=0.5)
ax.bar(x + w/2, dec["mean_us"], w, label="Decrypt+Verify", color="#FF9800", edgecolor="k", linewidth=0.5)
ax.errorbar(x - w/2, enc["mean_us"], yerr=enc["std_us"].values,
            fmt="none", ecolor="k", capsize=3, elinewidth=1)
ax.errorbar(x + w/2, dec["mean_us"], yerr=dec["std_us"].values,
            fmt="none", ecolor="k", capsize=3, elinewidth=1)
ax.set_xticks(x)
ax.set_xticklabels([f"{s} B" for s in sizes])
ax.set_xlabel("Tamanho do payload")
ax.set_ylabel("Tempo médio (μs)")
ax.set_title("ChaCha20-Poly1305 — Encrypt e Decrypt+Verify")
ax.legend()
ax.grid(axis="y", linestyle="--", alpha=0.5)
savefig("05_chacha20.png")

# ── 6. ChaCha20: vazão (throughput) ──────────────────────────────────────────
enc_tput = [s * 1e6 / t / 1e6 for s, t in zip(sizes, enc["mean_us"])]  # MB/s
dec_tput = [s * 1e6 / t / 1e6 for s, t in zip(sizes, dec["mean_us"])]

fig, ax = plt.subplots(figsize=(9, 5))
ax.plot(sizes, enc_tput, "o-", label="Encrypt", color="#03A9F4", linewidth=2)
ax.plot(sizes, dec_tput, "s--", label="Decrypt+Verify", color="#FF9800", linewidth=2)
ax.set_xscale("log", base=2)
ax.set_xlabel("Tamanho do payload (bytes, escala log₂)")
ax.set_ylabel("Vazão (MB/s)")
ax.set_title("ChaCha20-Poly1305 — Vazão vs. Tamanho do Payload")
ax.legend()
ax.grid(True, which="both", linestyle="--", alpha=0.4)
savefig("06_chacha20_throughput.png")

# ── 7. Protocolo híbrido completo ─────────────────────────────────────────────
proto = df[df["label"].str.contains("Protocolo|FullProto")].iloc[0]
fig, ax = plt.subplots(figsize=(6, 4))
stats = ["mean_us", "median_us", "p95_us", "p99_us"]
labels = ["Média", "Mediana", "P95", "P99"]
vals = [proto[s] / 1e3 for s in stats]
colors_p = ["#2196F3", "#4CAF50", "#FF9800", "#F44336"]
bars = ax.bar(labels, vals, color=colors_p, edgecolor="k", linewidth=0.6, width=0.5)
ax.set_ylabel("Tempo (ms)")
ax.set_title("Protocolo Híbrido Completo\n(X25519 + HKDF + ChaCha20, 128-bit, payload 64 B)")
ax.grid(axis="y", linestyle="--", alpha=0.5)
for bar, v in zip(bars, vals):
    ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.1,
            f"{v:.1f} ms", ha="center", va="bottom", fontsize=9)
savefig("07_full_protocol.png")

# ── 8. Visão geral comparativa (todos os testes, mediana) ────────────────────
fig, ax = plt.subplots(figsize=(14, 6))
ordered = df.sort_values("median_us", ascending=False)
colors_all = plt.cm.tab20(np.linspace(0, 1, len(ordered)))
ax.barh(ordered["label"], ordered["median_us"] / 1e3,
        color=colors_all, edgecolor="k", linewidth=0.4)
ax.set_xlabel("Mediana (ms)")
ax.set_title("Visão Geral — Todos os Testes (mediana)")
ax.grid(axis="x", linestyle="--", alpha=0.5)
ax.tick_params(axis="y", labelsize=7)
savefig("08_overview.png")

print(f"\nTodos os gráficos salvos em ./{OUT}/")