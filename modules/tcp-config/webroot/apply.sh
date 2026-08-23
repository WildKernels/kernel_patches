#!/data/adb/ksu/bin/busybox sh
# Core apply logic, shared by the WebUI (kernelsu.exec) and the nc handler.
# Usage: apply.sh <algo: cubic|bbr|kerneldflt> <qdisc: fq|fq_codel|pfifo_fast>
# algo and qdisc are INDEPENDENT — choosing an algorithm never changes the
# qdisc. Persists /data/adb/tcpcfg.state.
STATE=/data/adb/tcpcfg.state

ALGO=$1
QDISC=${2:-fq_codel}
[ -n "$ALGO" ] || { echo '{"ok":false,"error":"missing algo"}'; exit 0; }

case "$ALGO" in
    cubic) FINAL_ALGO=cubic ;;
    bbr) FINAL_ALGO=bbr ;;
    kerneldflt)
        FINAL_ALGO=$(zcat /proc/config.gz 2>/dev/null | grep -oE '^CONFIG_DEFAULT_TCP_CONG="[^"]*"' | cut -d'"' -f2)
        [ -n "$FINAL_ALGO" ] || FINAL_ALGO=cubic
        ;;
    *) echo "{\"ok\":false,\"error\":\"bad algo\"}"; exit 0 ;;
esac

case "$QDISC" in
    fq|fq_codel|pfifo_fast) FINAL_QDISC=$QDISC ;;
    *) echo "{\"ok\":false,\"error\":\"bad qdisc\"}"; exit 0 ;;
esac

if sysctl -w net.ipv4.tcp_congestion_control="$FINAL_ALGO" >/dev/null 2>&1 \
   && sysctl -w net.core.default_qdisc="$FINAL_QDISC" >/dev/null 2>&1; then
    echo "ALGO=$FINAL_ALGO" > "$STATE"
    echo "QDISC=$FINAL_QDISC" >> "$STATE"
    printf '{"ok":true,"algo":"%s","qdisc":"%s"}\n' "$FINAL_ALGO" "$FINAL_QDISC"
else
    echo '{"ok":false,"error":"sysctl failed"}'
fi
exit 0
