#!/bin/bash
# Builds the VehicleHud priv-app module for sdv_ivi_cf. Same lunch target as
# the image build, so only the new module and its dependencies get built.
cd ~/aosp
source build/envsetup.sh >/dev/null
lunch sdv_ivi_cf-aosp_current-userdebug >/dev/null
echo "start $(date +%T)"
/usr/bin/time -v m VehicleHud 2>&1 | tail -60
echo "end $(date +%T) status ${PIPESTATUS[0]}"
