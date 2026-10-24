#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The action matrix as tests (docs/modeling/README.md#action-matrix).

    action_matrix_smoke.py <designer-binary> --sample
    action_matrix_smoke.py <designer-binary> --shard K/N
    action_matrix_smoke.py <designer-binary> --determinism
    action_matrix_smoke.py --self-test

--sample is one (design, action, family) in 32 of the reference parts, eight
random designs and the gear, against the committed baseline: the inner loop's
share. --shard K/N is every action on the designs whose name hashes to K,
each shard on one instance of its own (`slow;action_matrix`). --determinism
builds a sample twice in fresh instances and runs every action in it twice.
--self-test feeds the classifier and the ratchet planted answers. Every run
writes into a private directory, never the repo's scratch/.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "tools", "action_matrix.py")


def main():
    args = sys.argv[1:]
    if args == ["--self-test"]:
        return subprocess.call([sys.executable, TOOL, "self-test"])
    if len(args) < 2:
        sys.exit(__doc__)
    binary, mode = os.path.abspath(args[0]), args[1:]
    out = tempfile.mkdtemp(prefix="action_matrix_smoke_")
    try:
        cmd = [sys.executable, TOOL, "run", "--binary", binary, "--jobs", "1",
               "--out", out]
        if mode == ["--sample"]:
            cmd.append("--sample")
        elif mode == ["--determinism"]:
            cmd.append("--determinism")
        elif len(mode) == 2 and mode[0] == "--shard":
            cmd += ["--shard", mode[1]]
        else:
            sys.exit(__doc__)
        rc = subprocess.call(cmd)
        if rc != 0 and os.path.exists(os.path.join(out, "report.md")):
            with open(os.path.join(out, "report.md")) as f:
                print(f.read()[:20000])
        return rc
    finally:
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
