#!/bin/sh
set -eu

# KDE is a lobby-backed Docker runner.  Stopping only PID 1 ends this runner;
# Wolf's normal runner-exit handler then stops the lobby and switches the
# current Moonlight session back to its still-running Wolf UI producer.  The
# Wolf service itself is not part of this container and stays online.
kill -TERM 1
