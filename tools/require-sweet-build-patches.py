#!/usr/bin/env python3
"""Insert actual-source verification into pinned dxb before expensive compilation.
This deliberately does not change upstream's historical optional patch policy.
The SWEET canonical tree check aborts if any required new patch was skipped.
"""
from pathlib import Path
import argparse
p=argparse.ArgumentParser(description=__doc__);p.add_argument('build_script',type=Path);args=p.parse_args()
raw=args.build_script.read_bytes()
anchor=b'    patches\n    venv\n'
assert raw.count(anchor)==1,'Unsupported builder: expected one patches/venv boundary'
replacement=b'''    patches
    if [[ "${MACHINE_CONFIG:-}" == "SWEET" ]]; then
        python3 "${GITHUB_WORKSPACE}/tools/verify-gmu-service-patch.py" --patched-tree "${WORK_DIR}/xnu" || exit $?
    fi
    venv
'''
args.build_script.write_bytes(raw.replace(anchor,replacement,1))
print('Inserted mandatory actual-SWEET-source check before compilation')
