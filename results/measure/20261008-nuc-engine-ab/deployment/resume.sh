#!/bin/bash
cd /data1/efs/bench/nuc-engine-ab-20261008
python3 src/scripts/ab_storage_engines_nuc.py --base "$PWD" --seconds 30 --repeat 2 --resume > progress.log 2>&1
echo "$?" > exit-code
