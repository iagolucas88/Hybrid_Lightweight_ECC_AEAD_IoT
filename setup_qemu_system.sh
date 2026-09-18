#!/usr/bin/env bash
# =============================================================================
# setup_qemu_system.sh
# -----------------------------------------------------------------------------
# Provisiona um ambiente QEMU FULL-SYSTEM (qemu-system-arm) com um Debian
# ARMHF (32-bit) minimalista, para substituir o QEMU User-Space (qemu-arm)
# usado nas rodadas anteriores da dissertacao.
#
# POR QUE NAO HA "cloud image" OFICIAL ARMHF?
#   A equipe Debian Cloud (cloud.debian.org/images/cloud) so publica imagens
#   genericcloud/nocloud/generic para amd64, arm64, ppc64el e riscv64. NAO
#   existe imagem cloud-init oficial para armhf (32-bit), pois a arquitetura
#   nao e mais um alvo comum de nuvem. Para ter um Debian ARMHF genuino e
#   reprodutivel, este script usa `debootstrap` (ferramenta OFICIAL Debian
#   para construir rootfs) diretamente sobre uma imagem de disco ext4, o que
#   e o metodo padrao recomendado pela propria comunidade QEMU/Debian para
#   placas ARM 32-bit emuladas (ex.: ARM Versatile Express / "virt").
#
# MAQUINA EMULADA: `virt` (qemu-system-arm -M virt)
#   - Suporta virtio-blk, virtio-net, PCIe, multi-core e >1GB de RAM;
#   - E o alvo mais proximo de um "IoT gateway" ARMv7 generico de 32 bits,
#     mais representativo do que placas legadas (ex.: vexpress-a9, 1 core).
#
# SAIDA GERADA (dentro de ./vm_fullsystem/):
#   debian-armhf.img   -> disco raiz ext4 (rootfs Debian bookworm armhf)
#   vmlinuz            -> kernel Linux ARMHF extraido do rootfs
#   initrd.img         -> initramfs correspondente
#   id_ed25519(.pub)   -> par de chaves SSH dedicado a VM de defesa
#
# USO:
#   sudo ./setup_qemu_system.sh
#
# Após concluir, use ./run_defense_vm.sh para subir a VM e rodar os testes.
# =============================================================================
set -euo pipefail

# --- 0. Constantes e validacao de privilegios -------------------------------
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VM_DIR="$ROOT_DIR/vm_fullsystem"
MOUNT_DIR="$VM_DIR/rootfs_mnt"
IMG_FILE="$VM_DIR/debian-armhf.img"
IMG_SIZE_MB="${IMG_SIZE_MB:-4096}"        # 4 GB de disco raiz
SUITE="${DEBIAN_SUITE:-bookworm}"          # release Debian estavel com bom suporte armhf
MIRROR="${DEBIAN_MIRROR:-http://deb.debian.org/debian}"
VM_HOSTNAME="iot-defesa-armhf"
VM_USER="iot"
SSH_KEY="$VM_DIR/id_ed25519"

if [[ "${EUID}" -ne 0 ]]; then
    echo "[ERRO] Este script precisa de root (debootstrap, mount, chroot)."
    echo "       Execute: sudo $0"
    exit 1
fi

REAL_USER="${SUDO_USER:-$USER}"

echo "==> Diretorio de trabalho: $VM_DIR"
mkdir -p "$VM_DIR" "$MOUNT_DIR"

# --- 1. Instala dependencias do host ----------------------------------------
echo "==> [1/7] Instalando dependencias (qemu-system-arm, debootstrap, etc.)"
apt-get update -qq
apt-get install -y \
    qemu-system-arm qemu-utils \
    debootstrap binfmt-support qemu-user qemu-user-binfmt \
    e2fsprogs dosfstools parted kpartx \
    openssh-client sshpass >/dev/null

# --- 2. Cria e formata a imagem de disco raiz -------------------------------
if [[ -f "$IMG_FILE" ]]; then
    echo "==> [2/7] Imagem $IMG_FILE ja existe — pulando criacao (delete-a para refazer do zero)."
else
    echo "==> [2/7] Criando imagem de disco ext4 de ${IMG_SIZE_MB}MB"
    truncate -s "${IMG_SIZE_MB}M" "$IMG_FILE"
    mkfs.ext4 -F -L rootfs "$IMG_FILE" >/dev/null
fi

echo "==> Montando imagem via loop device"
LOOP_DEV="$(losetup --find --show "$IMG_FILE")"
trap 'umount "$MOUNT_DIR" 2>/dev/null || true; losetup -d "$LOOP_DEV" 2>/dev/null || true' EXIT
mount "$LOOP_DEV" "$MOUNT_DIR"

# --- 3. debootstrap: monta o rootfs Debian ARMHF ----------------------------
if [[ -x "$MOUNT_DIR/bin/bash" ]]; then
    echo "==> [3/7] Rootfs ja provisionado — pulando debootstrap."
else
    echo "==> [3/7] Executando debootstrap --arch=armhf $SUITE (isso baixa ~300MB)"
    debootstrap --arch=armhf --foreign \
        --include=openssh-server,systemd-sysv,ifupdown,net-tools,sudo,ca-certificates,linux-image-armmp,rsync \
        "$SUITE" "$MOUNT_DIR" "$MIRROR"

    echo "==> Copiando binario qemu-arm (user-mode) para o rootfs (segundo estagio via binfmt)"
    # Distros recentes (ex.: Ubuntu 25.10+) renomearam o pacote "qemu-user-static"
    # para "qemu-user"; o binario tambem passou de qemu-arm-static para qemu-arm.
    QEMU_ARM_STATIC_BIN="$(command -v qemu-arm-static || command -v qemu-arm)"
    [[ -n "$QEMU_ARM_STATIC_BIN" ]] || { echo "[ERRO] qemu-arm(-static) nao encontrado no host."; exit 1; }
    cp "$QEMU_ARM_STATIC_BIN" "$MOUNT_DIR/usr/bin/qemu-arm-static"
    chroot "$MOUNT_DIR" /debootstrap/debootstrap --second-stage
fi

# --- 4. Configuracao minima do guest (rede, SSH, usuario) -------------------
echo "==> [4/7] Configurando hostname, rede, usuario e SSH dentro do chroot"

echo "$VM_HOSTNAME" > "$MOUNT_DIR/etc/hostname"
cat > "$MOUNT_DIR/etc/hosts" <<EOF
127.0.0.1   localhost
127.0.1.1   $VM_HOSTNAME
EOF

# Interface de rede simples via DHCP (a VM usara -netdev user do QEMU)
cat > "$MOUNT_DIR/etc/network/interfaces" <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF

# fstab: raiz em /dev/vda (virtio-blk sob a maquina "virt")
cat > "$MOUNT_DIR/etc/fstab" <<'EOF'
/dev/vda    /    ext4    defaults,noatime    0    1
EOF

# Gera par de chaves SSH dedicado (no host) para acesso automatizado sem senha
if [[ ! -f "$SSH_KEY" ]]; then
    echo "==> Gerando par de chaves SSH dedicado para a VM de defesa"
    ssh-keygen -t ed25519 -N "" -C "defesa-armhf-vm" -f "$SSH_KEY" >/dev/null
fi

chroot "$MOUNT_DIR" /bin/bash -c "
    set -e
    id -u $VM_USER >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo $VM_USER
    echo '$VM_USER:$VM_USER' | chpasswd
    echo 'root:root' | chpasswd
    mkdir -p /home/$VM_USER/.ssh
    chmod 700 /home/$VM_USER/.ssh
    systemctl enable ssh || true
"

# Injeta a chave publica para login sem senha (usada por run_defense_vm.sh)
install -o 1000 -g 1000 -m 600 "${SSH_KEY}.pub" "$MOUNT_DIR/home/$VM_USER/.ssh/authorized_keys"
chown -R "$REAL_USER":"$REAL_USER" "$VM_DIR" 2>/dev/null || true

# Permite sudo sem senha para o usuario de automacao (necessario para benchmarks isolados)
echo "$VM_USER ALL=(ALL) NOPASSWD:ALL" > "$MOUNT_DIR/etc/sudoers.d/90-$VM_USER"
chmod 440 "$MOUNT_DIR/etc/sudoers.d/90-$VM_USER"

# --- 5. Extrai kernel/initrd do rootfs para o host (usado por -kernel/-initrd) --
echo "==> [5/7] Extraindo kernel e initrd para $VM_DIR"
KERNEL_SRC="$(ls "$MOUNT_DIR"/boot/vmlinuz-* 2>/dev/null | sort -V | tail -n1 || true)"
INITRD_SRC="$(ls "$MOUNT_DIR"/boot/initrd.img-* 2>/dev/null | sort -V | tail -n1 || true)"

if [[ -z "$KERNEL_SRC" || -z "$INITRD_SRC" ]]; then
    echo "[ERRO] Kernel/initrd nao encontrados em /boot dentro do rootfs."
    echo "       Verifique se o pacote linux-image-armmp foi instalado corretamente."
    exit 1
fi

cp "$KERNEL_SRC" "$VM_DIR/vmlinuz"
cp "$INITRD_SRC" "$VM_DIR/initrd.img"

# --- 6. Limpeza de apt cache dentro do rootfs (reduz tamanho da imagem) -----
echo "==> [6/7] Limpando cache apt do guest"
chroot "$MOUNT_DIR" /bin/bash -c "apt-get clean" || true

# --- 7. Desmonta tudo --------------------------------------------------------
echo "==> [7/7] Finalizando: desmontando imagem"
sync
umount "$MOUNT_DIR"
losetup -d "$LOOP_DEV"
trap - EXIT

chown "$REAL_USER":"$REAL_USER" "$IMG_FILE" "$VM_DIR/vmlinuz" "$VM_DIR/initrd.img" "$SSH_KEY" "${SSH_KEY}.pub" 2>/dev/null || true

cat <<EOF

=============================================================================
 AMBIENTE FULL-SYSTEM PRONTO
=============================================================================
 Imagem raiz : $IMG_FILE
 Kernel      : $VM_DIR/vmlinuz
 Initrd      : $VM_DIR/initrd.img
 Chave SSH   : $SSH_KEY (usuario: $VM_USER / senha de fallback: $VM_USER)

 Proximo passo:
   ./run_defense_vm.sh
=============================================================================
EOF
