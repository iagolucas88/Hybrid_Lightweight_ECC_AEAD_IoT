#!/usr/bin/env bash
# =============================================================================
# run_defense_vm.sh
# -----------------------------------------------------------------------------
# Sobe a VM Debian ARMHF full-system (qemu-system-arm) provisionada por
# setup_qemu_system.sh, aguarda o SSH ficar disponivel, envia o binario
# cross-compilado (ARMHF, estatico) via scp, executa-o dentro da VM e traz
# de volta o CSV de resultados para o host.
#
# DIFERENCA-CHAVE vs. o fluxo antigo (qemu-arm user-space):
#   Antes:  qemu-arm ./build-arm/benchmark_suite      (so emula a CPU;
#           usa o kernel do HOST -> nao ha ruido de kernel/scheduler do guest)
#   Agora:  qemu-system-arm + Debian completo         (emula CPU + memoria +
#           dispositivos + kernel Linux ARMHF real dentro da VM -> ruido de
#           SO genuino: scheduler, syscalls, virtio, paginacao, etc.)
#
# USO:
#   ./run_defense_vm.sh [caminho_para_binario_armhf] [args_do_binario...]
#   MACRO_RODADAS=N ITERACOES_INTERNAS=M ./run_defense_vm.sh [binario]
#
#   O proprio binario (benchmark_suite_fullsystem) agora controla o loop de
#   MACRO_RODADAS internamente (std::filesystem), criando runs/run_XX e
#   gravando em cada uma o resultado agregado de ITERACOES_INTERNAS repeticoes
#   em RAM (percentis reais). Este script apenas sobe a VM, envia o binario,
#   executa-o UMA vez (passando --macro/--iters) e recolhe a arvore runs/
#   inteira de volta para o host via scp -r.
#
#   Exemplos:
#     ./run_defense_vm.sh build-arm-static/benchmark_suite_fullsystem
#     MACRO_RODADAS=30 ITERACOES_INTERNAS=1000 ./run_defense_vm.sh build-arm-static/benchmark_suite_fullsystem
#
#   Para o binario ARMHF estatico, compile antes com (uma vez so):
#     cmake -S . -B build-arm-static -DCMAKE_TOOLCHAIN_FILE=toolchains/armhf-gcc.cmake -DIOT_STATIC_ARM=ON
#     cmake --build build-arm-static -j"$(nproc)" --target benchmark_suite_fullsystem
#
#   Para o demonstrador interativo da defesa, use run_defesa_demo_vm.sh (menu com stdin).
# =============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VM_DIR="$ROOT_DIR/vm_fullsystem"
IMG_FILE="$VM_DIR/debian-armhf.img"
KERNEL="$VM_DIR/vmlinuz"
INITRD="$VM_DIR/initrd.img"
SSH_KEY="$VM_DIR/id_ed25519"

VM_RAM_MB="${VM_RAM_MB:-2048}"      # 2 GB de RAM (conforme solicitado)
VM_SMP="${VM_SMP:-4}"               # 4 nucleos virtuais
SSH_PORT="${SSH_PORT:-2222}"        # port-forward do host para a VM (22)
VM_USER="${VM_USER:-iot}"
SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5)

BIN_LOCAL="${1:-$ROOT_DIR/build-arm-static/benchmark_suite_fullsystem}"
shift || true
REMOTE_ARGS=("$@")
MACRO_RODADAS="${MACRO_RODADAS:-30}"            # MACRO_RODADAS: passado ao binario via --macro
ITERACOES_INTERNAS="${ITERACOES_INTERNAS:-1000}" # ITERACOES_INTERNAS: passado ao binario via --iters
TEST_ID="$(date +%Y%m%d_%H%M%S)"
TEST_ROOT="$ROOT_DIR/benchmark_runs/$TEST_ID"
OUT_DIR="$TEST_ROOT/runs"
COMPARISON_DIR="$TEST_ROOT/comparison"

QEMU_PID=""
QEMU_LOG="$VM_DIR/qemu_boot.log"

cleanup() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "==> Encerrando VM (poweroff remoto)..."
        ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "sudo poweroff" 2>/dev/null || true
        # aguarda encerramento gracioso; força kill caso demore demais
        for _ in $(seq 1 15); do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 1
        done
        kill -0 "$QEMU_PID" 2>/dev/null && kill "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# --- 0. Validacoes -----------------------------------------------------------
for f in "$IMG_FILE" "$KERNEL" "$INITRD" "$SSH_KEY"; do
    [[ -f "$f" ]] || { echo "[ERRO] Arquivo ausente: $f — rode ./setup_qemu_system.sh primeiro."; exit 1; }
done
[[ -f "$BIN_LOCAL" ]] || { echo "[ERRO] Binario ARMHF nao encontrado: $BIN_LOCAL"; exit 1; }

echo "==> Binario a executar na VM : $BIN_LOCAL"
echo "==> RAM=${VM_RAM_MB}MB  SMP=${VM_SMP}  SSH local=127.0.0.1:${SSH_PORT}"
# Limpa eventuais instancias orfas ou suspensas do QEMU que estejam bloqueando a porta ou disco
if pgrep -f "qemu-system-arm.*debian-armhf.img" >/dev/null 2>&1; then
    echo "==> [aviso] Instancia anterior do QEMU detectada. Encerrando-a para liberar a porta ${SSH_PORT} e o disco..."
    pkill -9 -f "qemu-system-arm.*debian-armhf.img" 2>/dev/null || true
    sleep 1
fi

# --- 1. Sobe a VM em background (sem interface grafica) ---------------------
echo "==> [1/5] Iniciando qemu-system-arm (maquina 'virt')..."
qemu-system-arm \
    -M virt \
    -m "$VM_RAM_MB" \
    -smp "$VM_SMP" \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -append "root=/dev/vda rw console=ttyAMA0 rootwait" \
    -drive if=none,file="$IMG_FILE",format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 \
    -device virtio-net-device,netdev=net0 \
    -nographic \
    > "$QEMU_LOG" 2>&1 &
QEMU_PID=$!
echo "==> QEMU PID=$QEMU_PID  (log: $QEMU_LOG)"

# --- 2. Aguarda o SSH ficar disponivel ---------------------------------------
echo "==> [2/5] Aguardando SSH da VM ficar disponivel (timeout 180s)..."
READY=0
for i in $(seq 1 90); do
    if ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "echo ok" >/dev/null 2>&1; then
        READY=1
        break
    fi
    kill -0 "$QEMU_PID" 2>/dev/null || { echo "[ERRO] Processo QEMU morreu. Veja $QEMU_LOG"; exit 1; }
    sleep 2
done
[[ "$READY" -eq 1 ]] || { echo "[ERRO] Timeout aguardando SSH. Veja $QEMU_LOG"; exit 1; }
echo "==> SSH disponivel."

# --- 3. Envia o binario cross-compilado via scp ------------------------------
echo "==> [3/5] Enviando binario via scp..."
scp -i "$SSH_KEY" -P "$SSH_PORT" -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    "$BIN_LOCAL" "$VM_USER@127.0.0.1:/home/$VM_USER/bench_bin"
ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "chmod +x /home/$VM_USER/bench_bin"

# --- 4. Executa remotamente UMA vez: o binario faz o loop de MACRO_RODADAS --
echo "==> [4/5] Executando remotamente dentro do guest Debian ARMHF (MACRO_RODADAS=$MACRO_RODADAS, ITERACOES_INTERNAS=$ITERACOES_INTERNAS)..."
mkdir -p "$OUT_DIR"
ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" \
    "cd /home/$VM_USER && rm -rf runs && ./bench_bin --macro $MACRO_RODADAS --iters $ITERACOES_INTERNAS ${REMOTE_ARGS[*]}" \
    | tee "$OUT_DIR/vm_stdout.log"

# --- 5. Recupera a arvore runs/run_XX inteira (criada pelo binario via std::filesystem) --
echo "==> [5/5] Recolhendo runs/ do guest para o host..."
scp -r -i "$SSH_KEY" -P "$SSH_PORT" -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    "$VM_USER@127.0.0.1:/home/$VM_USER/runs/." "$OUT_DIR/"
echo "==> Rodadas salvas em: $OUT_DIR"

# --- 6. Comparativo entre rodadas (se houver mais de uma e Python/pandas disponiveis) --
REPEAT="$MACRO_RODADAS"
if [[ "$REPEAT" -gt 1 ]]; then
    PYTHON_BIN="$ROOT_DIR/benchmarks/resultados/bin/python"
    [[ -x "$PYTHON_BIN" ]] || PYTHON_BIN="python3"
    if command -v "$PYTHON_BIN" >/dev/null 2>&1 && "$PYTHON_BIN" -c "import pandas" >/dev/null 2>&1; then
        echo "==> Gerando comparativo entre as $REPEAT rodadas..."
        COMP_DIR="$COMPARISON_DIR"
        mkdir -p "$COMP_DIR"
        "$PYTHON_BIN" - "$OUT_DIR" "$COMP_DIR" <<'PYEOF'
import sys
from pathlib import Path
import pandas as pd

out_base = Path(sys.argv[1]); comp_dir = Path(sys.argv[2])
csv_files = sorted(out_base.glob("run_*/resultados_benchmark.csv"))
if not csv_files:
    raise SystemExit("Sem CSVs para comparar.")
frames = []
for f in csv_files:
    df = pd.read_csv(f)
    required = {"label", "mean_us", "p95_us", "p99_us"}
    missing = required.difference(df.columns)
    if missing:
        print(f"[AVISO] Ignorando {f.name}: colunas ausentes {sorted(missing)}")
        continue
    df["label"] = df["label"].astype(str).str.strip().str.strip('"')
    df["run"] = f.parent.name
    frames.append(df)
if not frames:
    raise SystemExit("Nenhum resultados_benchmark.csv valido para comparar.")
all_df = pd.concat(frames, ignore_index=True)
summary = (all_df.groupby("label", as_index=False)
           .agg(runs=("run", "nunique"), mean_of_means_us=("mean_us", "mean"),
                std_of_means_us=("mean_us", "std"), min_mean_us=("mean_us", "min"),
                max_mean_us=("mean_us", "max"))
           .sort_values("mean_of_means_us", ascending=False))
summary.to_csv(comp_dir / "comparison_summary.csv", index=False)
print(f"Comparativo salvo em {comp_dir}/comparison_summary.csv")
PYEOF
    else
        echo "==> [AVISO] Python/pandas nao disponivel — pulei o comparativo (CSVs individuais ainda estao em $OUT_DIR)."
    fi
fi

echo
echo "=============================================================================="
echo " Execucao full-system concluida."
echo " Resultados em: $OUT_DIR"
echo "=============================================================================="
