#!/bin/sh
#
# Dropped into /storage/.config/autostart/ , which ROCKNIX's /usr/bin/autostart
# walks as root before starting EmulationStation.
#
# Those scripts run synchronously and the loop is followed by a wait, so
# running the probe inline would stall the UI for as long as the probe takes.
# setsid detaches it past that wait.

setsid /storage/pid351/probe.sh >/dev/null 2>&1 &
