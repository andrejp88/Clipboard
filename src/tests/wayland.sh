#!/bin/sh
. ./resources.sh
start_test "Test Wayland functionality"
export CLIPBOARD_FORCETTY=1
set +u

# Test if we have wl-copy installed
if ! command -v wl-copy >/dev/null 2>&1;
then
    echo "⏭️ Skipping Wayland tests due to missing wl-copy"
    exit 0
fi

if [ -n "$CLIPBOARD_NOGUI" ]
then
    echo "⏭️ Skipping Wayland tests due to CLIPBOARD_NOGUI"
    exit 0
fi

if [ "$CLIPBOARD_REQUIREWAYLAND" = "0" ]
then
    echo "⏭️ Skipping Wayland tests due to CLIPBOARD_REQUIREWAYLAND=0"
    exit 0
fi

if ! wl-copy -o >/dev/null 2>&1;
then
    echo "⏭️ Skipping Wayland tests due to wl-copy failing"
    exit 0
fi

make_files

cb copy "Some text"

assert_equals "Some text" "$(wl-paste)"

unset CLIPBOARD_FORCETTY

cb copy < ../TurnYourClipboardUp.png

sleep 6

assert_equals "$(cat ../TurnYourClipboardUp.png)" "$(until wl-paste; do sleep 1; done)"

cb copy < ../"Exosphere 2.0.mp3"

sleep 6

assert_equals "$(cat ../"Exosphere 2.0.mp3")" "$(until wl-paste; do sleep 1; done)"