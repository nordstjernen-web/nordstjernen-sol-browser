#!/usr/bin/env bash
# check-no-profile-data.sh — fail when a package or staging tree carries browser profile data.
set -euo pipefail

if [ $# -eq 0 ]; then
    echo "usage: $0 DIR|PACKAGE..." >&2
    exit 2
fi

list_entries() {
    local target=$1
    if [ -d "$target" ]; then
        (cd "$target" && find . -mindepth 1 | sed 's#^\./##')
        return
    fi
    case "$target" in
        *.zip|*.msix|*.appx) unzip -Z1 "$target" ;;
        *.deb) dpkg-deb -c "$target" | awk '{ print $NF }' ;;
        *.rpm) rpm -qlp "$target" ;;
        *.apk|*.tar|*.tar.*|*.tgz|*.txz|*.pkg) tar -tf "$target" ;;
        *)
            echo "check-no-profile-data: cannot list $target" >&2
            return 1 ;;
    esac
}

profile_entry='(^|/)(cookies|localstorage|indexeddb|sessionstorage|models)(/|$)'
profile_entry+='|(^|/)(cookies\.db|history\.sqlite|session\.db|remote-control\.json'
profile_entry+='|altsvc\.txt|hsts-curl\.txt|nordstjernen-debug\.log|nordstjernen\.conf)$'
profile_entry+='|\.(db|sqlite|sqlite3)(-wal|-shm|-journal)?$'

status=0
for target in "$@"; do
    entries=$(list_entries "$target") || { status=1; continue; }
    found=$(printf '%s\n' "$entries" | grep -iE "$profile_entry" || true)
    if [ -n "$found" ]; then
        echo "check-no-profile-data: $target contains browser profile data:" >&2
        printf '  %s\n' $found >&2
        status=1
    fi
done
exit $status
