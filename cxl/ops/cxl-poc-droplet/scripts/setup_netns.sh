#!/usr/bin/env bash
# setup_netns.sh — create gnb-ns / ue-ns + veth pair for OAI rfsimulator
#
# Network topology:
#   gnb-ns: veth-gnb  10.77.0.2/30  (gNB binds rfsimulator here)
#   ue-ns:  veth-ue   10.77.0.1/30  (UE connects to gNB via rfsimulator)
#
# Must run as root. Idempotent: tears down existing ns/link before recreating.
set -euo pipefail

GNB_NS=gnb-ns
UE_NS=ue-ns
VETH_GNB=veth-gnb
VETH_UE=veth-ue
GNB_IP=10.77.0.2/30
UE_IP=10.77.0.1/30

echo "[netns] tearing down any existing state..."
ip netns del "$GNB_NS" 2>/dev/null || true
ip netns del "$UE_NS"  2>/dev/null || true
ip link del "$VETH_GNB" 2>/dev/null || true

echo "[netns] creating namespaces..."
ip netns add "$GNB_NS"
ip netns add "$UE_NS"

echo "[netns] creating veth pair $VETH_GNB <-> $VETH_UE..."
ip link add "$VETH_GNB" type veth peer name "$VETH_UE"

echo "[netns] moving into namespaces..."
ip link set "$VETH_GNB" netns "$GNB_NS"
ip link set "$VETH_UE"  netns "$UE_NS"

echo "[netns] assigning addresses..."
ip netns exec "$GNB_NS" ip addr add "$GNB_IP" dev "$VETH_GNB"
ip netns exec "$GNB_NS" ip link set "$VETH_GNB" up
ip netns exec "$GNB_NS" ip link set lo up

ip netns exec "$UE_NS"  ip addr add "$UE_IP" dev "$VETH_UE"
ip netns exec "$UE_NS"  ip link set "$VETH_UE" up
ip netns exec "$UE_NS"  ip link set lo up

echo "[netns] verifying connectivity..."
ip netns exec "$GNB_NS" ping -c1 -W2 10.77.0.1 >/dev/null \
    && echo "[netns] PASS: gnb-ns -> ue-ns" \
    || echo "[netns] FAIL: gnb-ns -> ue-ns (check veth)"

ip netns exec "$UE_NS"  ping -c1 -W2 10.77.0.2 >/dev/null \
    && echo "[netns] PASS: ue-ns -> gnb-ns" \
    || echo "[netns] FAIL: ue-ns -> gnb-ns (check veth)"

echo "[netns] done."
echo ""
echo "Usage:"
echo "  ip netns exec $GNB_NS <gNB command>"
echo "  ip netns exec $UE_NS  <UE command>"
echo ""
echo "gNB rfsimulator binds on 10.77.0.2 automatically (no --serveraddr needed)."
echo "UE rfsimulator points to 10.77.0.2 via: --rfsimulator.serveraddr 10.77.0.2"
