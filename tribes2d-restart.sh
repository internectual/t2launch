#!/bin/sh
#
# A script to update the Tribes II binary and restart after crashes

# Go to the install directory
cd `dirname $0`

# Check command line arguments
if [ "$1" = "" ]; then
    echo "Usage: $0 -dedicated [-bots N] <map> <gametype>" >&2
    exit 1
fi

# Check to see if we can update the game
manifest="$HOME/.loki/installed/tribes2.xml"
if [ ! -w "$manifest" ]; then
    echo "Note: You don't have permissions to automatically update the game."
    echo "      To update the Tribes II dedicated server, log in as the user"
    echo "      that installed the game and run: loki_update tribes2"
fi

# Warn the users if they are running the server as root
if [ "`whoami`" = "root" ]; then
    echo "Warning: Running the server as root is a security risk!"
    sleep 3
fi

# Run the server...
while true
do  if [ -w "$manifest" ]; then
        loki_update tribes2
    fi
    ./tribes2d $*
done
