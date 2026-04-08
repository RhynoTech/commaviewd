#!/usr/bin/env bash
set +e
echo "Stopping services..."
bash /data/commaview/stop.sh 2>/dev/null || true
echo "Removing boot hook..."
sed -i '/# commaview-hook/d; /commaview\/start.sh/d' /data/continue.sh 2>/dev/null || true
echo "Removing files..."
rm -rf /data/commaview
echo "CommaView uninstalled"
