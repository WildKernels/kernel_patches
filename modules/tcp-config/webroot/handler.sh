#!/data/adb/ksu/bin/busybox sh
# Tiny HTTP handler, invoked once per connection by:
#   nc -lk -p 8090 -e handler.sh
# Answers /status and /set?algo=..&qdisc=.. with JSON. Fallback channel only
# (the KSU WebUI is the primary one); keep the logic in apply.sh.
STATE=/data/adb/tcpcfg.state
DIR=$(dirname "$0")

IFS= read -r request_line || exit 0
path=$(echo "$request_line" | awk '{print $2}')

ok() { echo "HTTP/1.1 200 OK"; echo "Content-Type: application/json"; echo "Connection: close"; echo ""; "$@"; }
err() { echo "HTTP/1.1 200 OK"; echo "Content-Type: application/json"; echo "Connection: close"; echo ""; printf '{"ok":false,"error":"%s"}\n' "$1"; }
notfound() { echo "HTTP/1.1 404 Not Found"; echo "Connection: close"; echo ""; echo '{"error":"not found"}'; }

case "$path" in
    /status)
        ALGO=$(sysctl -n net.ipv4.tcp_congestion_control 2>/dev/null)
        QDISC=$(sysctl -n net.core.default_qdisc 2>/dev/null)
        DFLT=$(zcat /proc/config.gz 2>/dev/null | grep -oE '^CONFIG_DEFAULT_TCP_CONG="[^"]*"' | cut -d'"' -f2)
        [ -n "$DFLT" ] || DFLT=unknown
        SALGO=""; SQDISC=""
        if [ -f "$STATE" ]; then
            . "$STATE"
            SALGO=$ALGO; SQDISC=$QDISC
        fi
        echo "HTTP/1.1 200 OK"
        echo "Content-Type: application/json"
        echo "Connection: close"
        echo ""
        printf '{"algo":"%s","qdisc":"%s","dflt":"%s","state_algo":"%s","state_qdisc":"%s"}\n' "$ALGO" "$QDISC" "$DFLT" "$SALGO" "$SQDISC"
        ;;
    /set*)
        QUERY_STRING=${path#/set?}
        ALGO=""; QDISC="fq_codel"
        for kv in $(echo "$QUERY_STRING" | tr '&' ' '); do
            case "$kv" in
                algo=*) ALGO=${kv#algo=} ;;
                qdisc=*) QDISC=${kv#qdisc=} ;;
            esac
        done
        [ -n "$ALGO" ] || { err "missing algo"; exit 0; }
        echo "HTTP/1.1 200 OK"
        echo "Content-Type: application/json"
        echo "Connection: close"
        echo ""
        sh "$DIR/apply.sh" "$ALGO" "$QDISC"
        ;;
    *)
        notfound
        ;;
esac
exit 0
