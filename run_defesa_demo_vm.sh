#!/usr/bin/env bash
# =============================================================================
# run_defesa_demo_vm.sh
# -----------------------------------------------------------------------------
# Sobe a VM Debian ARMHF full-system (qemu-system-arm) e executa o
# DEMONSTRADOR INTERATIVO DA DEFESA (menu com trafego/MITM/timing/inspecao)
# dentro do guest, com terminal interativo real (pseudo-tty via ssh -t) para
# que os "Escolha:" do menu funcionem normalmente pelo teclado.
#
# Ao encerrar a sessao (opcao "0 - Sair"), a pasta Defesa Log/ gerada dentro
# da VM (auditoria completa da sessao) e copiada de volta para o host.
#
# PRE-REQUISITOS:
#   1) sudo ./setup_qemu_system.sh            (uma vez so, provisiona a VM)
#   2) Compilar o binario ARMHF estatico:
#        cmake -S . -B build-arm-static -DCMAKE_TOOLCHAIN_FILE=toolchains/armhf-gcc.cmake -DIOT_STATIC_ARM=ON
#        cmake --build build-arm-static -j"$(nproc)" --target demonstrador_defesa
#
# USO:
#   ./run_defesa_demo_vm.sh [caminho_para_binario_armhf]
# =============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VM_DIR="$ROOT_DIR/vm_fullsystem"
IMG_FILE="$VM_DIR/debian-armhf.img"
KERNEL="$VM_DIR/vmlinuz"
INITRD="$VM_DIR/initrd.img"
SSH_KEY="$VM_DIR/id_ed25519"

VM_RAM_MB="${VM_RAM_MB:-2048}"
VM_SMP="${VM_SMP:-4}"
SSH_PORT="${SSH_PORT:-2222}"
VM_USER="${VM_USER:-iot}"
SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5)

BIN_LOCAL="${1:-$ROOT_DIR/build-arm-static/demonstrador_defesa}"
OUT_DIR="$ROOT_DIR/Defesa Log/vm_$(date +%Y%m%d_%H%M%S)"

QEMU_PID=""
QEMU_LOG="$VM_DIR/qemu_boot.log"

cleanup() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "==> Encerrando VM (poweroff remoto)..."
        ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "sudo poweroff" 2>/dev/null || true
        for _ in $(seq 1 15); do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 1
        done
        kill -0 "$QEMU_PID" 2>/dev/null && kill "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for f in "$IMG_FILE" "$KERNEL" "$INITRD" "$SSH_KEY"; do
    [[ -f "$f" ]] || { echo "[ERRO] Arquivo ausente: $f — rode ./setup_qemu_system.sh primeiro."; exit 1; }
done
[[ -f "$BIN_LOCAL" ]] || { echo "[ERRO] Binario ARMHF nao encontrado: $BIN_LOCAL"; exit 1; }

# Limpa eventuais instancias orfas ou suspensas do QEMU que estejam bloqueando a porta ou disco
if pgrep -f "qemu-system-arm.*debian-armhf.img" >/dev/null 2>&1; then
    echo "==> [aviso] Instancia anterior do QEMU detectada. Encerrando-a para liberar a porta ${SSH_PORT} e o disco..."
    pkill -9 -f "qemu-system-arm.*debian-armhf.img" 2>/dev/null || true
    sleep 1
fi

echo "==> [1/4] Iniciando qemu-system-arm (maquina 'virt')..."
qemu-system-arm \
    -M virt -m "$VM_RAM_MB" -smp "$VM_SMP" \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "root=/dev/vda rw console=ttyAMA0 rootwait" \
    -drive if=none,file="$IMG_FILE",format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 \
    -device virtio-net-device,netdev=net0 \
    -nographic \
    > "$QEMU_LOG" 2>&1 &
QEMU_PID=$!
echo "==> QEMU PID=$QEMU_PID  (log: $QEMU_LOG)"

echo "==> [2/4] Aguardando SSH da VM ficar disponivel (timeout 180s)..."
READY=0
for i in $(seq 1 90); do
    if ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "echo ok" >/dev/null 2>&1; then
        READY=1; break
    fi
    kill -0 "$QEMU_PID" 2>/dev/null || { echo "[ERRO] Processo QEMU morreu. Veja $QEMU_LOG"; exit 1; }
    sleep 2
done
[[ "$READY" -eq 1 ]] || { echo "[ERRO] Timeout aguardando SSH. Veja $QEMU_LOG"; exit 1; }
echo "==> SSH disponivel."

echo "==> [3/4] Enviando demonstrador_defesa via scp..."
scp -i "$SSH_KEY" -P "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    "$BIN_LOCAL" "$VM_USER@127.0.0.1:/home/$VM_USER/demonstrador_defesa"
ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "chmod +x /home/$VM_USER/demonstrador_defesa"

echo "==> [4/4] Sessao interativa — use o menu normalmente (0 para sair ao final)."
echo "--------------------------------------------------------------------------"
# -t forca alocacao de pseudo-tty: preserva cores ANSI e leitura interativa do menu.
ssh -t "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "cd /home/$VM_USER && ./demonstrador_defesa" || true
echo "--------------------------------------------------------------------------"

echo "==> Recuperando 'Defesa Log/' da VM para o host..."
mkdir -p "$OUT_DIR"
if ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "test -d '/home/$VM_USER/Defesa Log'" 2>/dev/null; then
    scp -r -i "$SSH_KEY" -P "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        "$VM_USER@127.0.0.1:/home/$VM_USER/Defesa Log/." "$OUT_DIR/"
    echo "  [OK] Auditoria salva em: $OUT_DIR"
else
    echo "  [AVISO] Nenhum log de auditoria encontrado na VM (sessao encerrada sem executar simulacoes?)."
fi
