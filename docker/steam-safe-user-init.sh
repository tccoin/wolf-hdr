#!/usr/bin/env bash

set -e

source /opt/gow/bash-lib/utils.sh

gow_log "**** Configure default user ****"

if [[ "${UNAME}" != "root" ]]; then
    PUID="${PUID:-1000}"
    PGID="${PGID:-1000}"
    UMASK="${UMASK:-000}"

    gow_log "Setting default user uid=${PUID}(${UNAME}) gid=${PGID}(${UNAME})"
    if id -u "${PUID}" &>/dev/null; then
        oldname=$(id -nu "${PUID}")
        # Docker restart retains /etc/passwd from the writable container layer.
        # On the second boot the old user is therefore already `retro`; the
        # upstream `userdel -r retro` recursively removes $HOME.  Wolf binds
        # its persistent Steam profile at $HOME, so that command erases the
        # game library.  Keep the home directory whenever it belongs to the
        # account we are about to recreate.
        if [[ "${oldname}" == "${UNAME}" ]]; then
            userdel "${oldname}"
        else
            userdel -r "${oldname}"
        fi
    fi

    groupadd -f -g "${PGID}" "${UNAME}"
    useradd -m -d "${HOME}" -u "${PUID}" -g "${PGID}" -s /bin/bash "${UNAME}"

    gow_log "Setting umask to ${UMASK}"
    umask "${UMASK}"

    gow_log "Ensure retro home directory is writable"
    chown "${PUID}:${PGID}" "${HOME}"

    gow_log "Ensure XDG_RUNTIME_DIR is writable"
    chown -R "${PUID}:${PGID}" "${XDG_RUNTIME_DIR}"
else
    gow_log "Container running as root. Nothing to do."
fi

gow_log "DONE"
