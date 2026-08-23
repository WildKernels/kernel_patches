#!/system/bin/sh
# Uninstall: stop our httpd, remove state.
PID=$(cat /data/adb/tcpcfg.httpd.pid 2>/dev/null)
[ -n "$PID" ] && kill "$PID" 2>/dev/null
rm -f /data/adb/tcpcfg.state /data/adb/tcpcfg.httpd.pid
exit 0
