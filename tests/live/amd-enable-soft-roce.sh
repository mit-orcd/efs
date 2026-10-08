#!/usr/bin/env bash
# Run as root on AMD. Persistent software RoCE on its current Wi-Fi netdev.
set -euo pipefail
ip link show wlp194s0 >/dev/null
command -v rdma >/dev/null
mkdir -p /etc/modules-load.d
printf '%s\n' ib_uverbs rdma_rxe > /etc/modules-load.d/efs-soft-roce.conf
modprobe ib_uverbs
modprobe rdma_rxe
cat > /etc/systemd/system/efs-soft-roce.service <<'UNIT'
[Unit]
Description=EFS software RoCE device
Wants=network-online.target
After=network-online.target systemd-modules-load.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'test -d /sys/class/infiniband/rxe0 || exec /usr/bin/rdma link add rxe0 type rxe netdev wlp194s0'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable --now efs-soft-roce.service
rdma link show
ibv_devinfo -d rxe0
ls -l /dev/infiniband
