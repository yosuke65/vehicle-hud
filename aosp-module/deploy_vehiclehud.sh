#!/bin/bash
# Pushes the built VehicleHud priv-app module into the running sdv_ivi_cf
# Cuttlefish (userdebug, verity already disabled) and reboots so the
# package manager scans it as a privileged system app.
set -u
cd ~/aosp
source build/envsetup.sh >/dev/null
lunch sdv_ivi_cf-aosp_current-userdebug >/dev/null
A="adb -s 0.0.0.0:6520"
OUTDIR=$ANDROID_PRODUCT_OUT

ls -la $OUTDIR/system_ext/priv-app/VehicleHud/ $OUTDIR/system_ext/etc/permissions/privapp-permissions-vehiclehud.xml || exit 1

$A root >/dev/null; sleep 2; $A wait-for-device
$A remount 2>&1 | tail -2
# The earlier, normally installed copy is signed with the debug key; a
# system app with the same package name and another key would clash.
$A uninstall dev.yosukesakurai.vehiclehud 2>&1 | tail -1
$A shell mkdir -p /system_ext/priv-app/VehicleHud
$A push $OUTDIR/system_ext/priv-app/VehicleHud/VehicleHud.apk /system_ext/priv-app/VehicleHud/VehicleHud.apk | tail -1
$A push $OUTDIR/system_ext/etc/permissions/privapp-permissions-vehiclehud.xml /system_ext/etc/permissions/ | tail -1
echo "rebooting $(date +%T)"
$A reboot
sleep 20
$A wait-for-device
for i in $(seq 1 90); do
  [ "$($A shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] && break
  sleep 3
done
echo "boot_completed $(date +%T)"
$A root >/dev/null; sleep 2; $A wait-for-device
$A shell pm list packages -f dev.yosukesakurai.vehiclehud
$A shell dumpsys package dev.yosukesakurai.vehiclehud | grep -E "codePath=|pkgFlags=|privateFlags=|ACCESS_PRIVATE_DISPLAY_ID|INTERNAL_SYSTEM_WINDOW|CAR_SPEED" | head -10
$A logcat -d | grep -i -E "privapp|Privileged permission" | grep -i vehiclehud | head -5
