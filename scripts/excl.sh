#!/usr/bin/env bash
# excl.sh on PID | off: a KWin script that hides this process's surface from every capture (window.excludeFromCapture), so the monitor's own screencast shows what lies under the overlay. Nothing else is touched; off unloads it.
set -euo pipefail
name=mvexflow-excl
js=${XDG_RUNTIME_DIR:-/tmp}/$name.js
call() { qdbus6 org.kde.KWin /Scripting "org.kde.kwin.Scripting.$1" "${@:2}"; }
case "${1:-}" in
    on)
        cat >"$js" <<JS
function mark(w) { if (w && w.pid === ${2:?pid}) { w.excludeFromCapture = true; print("mvexflow: excluded pid " + w.pid + " layer " + w.layer + " now " + w.excludeFromCapture); } }
workspace.windowList().forEach(mark);
workspace.windowAdded.connect(mark);
JS
        [[ $(call isScriptLoaded "$name") == true ]] && call unloadScript "$name" >/dev/null
        id=$(call loadScript "$js" "$name")
        qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run
        ;;
    off)
        [[ $(call isScriptLoaded "$name") == true ]] && call unloadScript "$name" >/dev/null
        rm -f "$js"
        ;;
    *)
        echo "usage: excl.sh on PID | off" >&2
        exit 2
        ;;
esac
