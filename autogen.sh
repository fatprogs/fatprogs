#!/bin/sh

# This script initializes the GNU build system by generating the configure
# script and Makefiles from the .ac and .am source files.

set -e

# Check for required tools
REQUIRED_TOOLS="autoreconf autoconf automake libtoolize"
MISSING_TOOLS=""

for tool in $REQUIRED_TOOLS; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        MISSING_TOOLS="$MISSING_TOOLS $tool"
    fi
done

if [ -n "$MISSING_TOOLS" ]; then
    echo "Error: The following required tools are missing:$MISSING_TOOLS"
    echo ""
    echo "Please install them using your package manager."
    echo "Example for Ubuntu/Debian:"
    echo "  sudo apt-get update"
    echo "  sudo apt-get install autoconf automake libtool"
    echo ""
    echo "Example for Fedora/RHEL/CentOS:"
    echo "  sudo dnf install autoconf automake libtool"
    echo ""
    exit 1
fi

echo "Generating build files using autoreconf..."
mkdir -p m4
autoreconf -i -v -f
