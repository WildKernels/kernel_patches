#!/system/bin/sh
# TCP Config module — apply saved settings at boot and serve the web page.
#
#   - state file: /data/adb/tcpcfg.state (written by the web UI)
#   - web server: toybox/busybox nc -l -p 8090 -e handler.sh (one connection
#     per fork; the while loop re-listens after each request)
#   - saved selection is re-applied on every boot; without a state file
#     nothing is touched (kernel default wins)

MODDIR=${0%/*}
# toybox nc has no -e; use KSU's busybox nc (has -e and -lk persistent listen)
NC=/data/adb/ksu/bin/busybox
STATE=/data/adb/tcpcfg.state
PORT=8090

log() { echo "tcp-config: $*"; }

# ---- 1. re-apply saved state (if any) ----
# ALGO and QDISC are independent: applying one never touches the other.
if [ -f "$STATE" ]; then
    . "$STATE"
    ALGO="${ALGO:-}"
    QDISC="${QDISC:-}"
    if [ -n "$ALGO" ]; then
        case "$ALGO" in
            kerneldflt)
                DFLT=$(zcat /proc/config.gz 2>/dev/null | grep -oE '^CONFIG_DEFAULT_TCP_CONG="[^"]*"' | cut -d'"' -f2)
                [ -n "$DFLT" ] || DFLT=cubic
                sysctl -w net.ipv4.tcp_congestion_control="$DFLT" >/dev/null 2>&1
                ;;
            *)
                sysctl -w net.ipv4.tcp_congestion_control="$ALGO" >/dev/null 2>&1
                ;;
        esac
        if [ -n "$QDISC" ]; then
            sysctl -w net.core.default_qdisc="$QDISC" >/dev/null 2>&1
        fi
        log "applied ALGO=$ALGO QDISC=$QDISC"
    fi
else
    log "no state, leaving kernel defaults"
fi

# ---- 2. start the tiny HTTP server (idempotent) ----
if ! $NC nc --help 2>&1 | grep -q -- '-e '; then
    log "ERROR: busybox nc without -e support"
    exit 1
fi
# kill whatever listens on our port (pid files are unreliable: setsid
# wrappers die but children survive; ss is absent, use netstat)
OLD=$(netstat -tlnp 2>/dev/null | grep ":$PORT " | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
[ -n "$OLD" ] && kill "$OLD" 2>/dev/null
# -lk: persistent server, runs handler.sh per connection
setsid $NC nc -lk -p $PORT -e "$MODDIR/webroot/handler.sh" >/dev/null 2>&1 &
echo $! > /data/adb/tcpcfg.httpd.pid
sleep 0.5
if netstat -tln 2>/dev/null | grep -q ":$PORT "; then
    log "web server on :$PORT (http://<phone-ip>:$PORT)"
else
    log "WARN: server not listening on :$PORT (check nc)"
fi

exit 0
