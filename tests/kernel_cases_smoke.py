#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The kernel case corpus as tests (docs/modeling/README.md#kernel-cases).

    kernel_cases_smoke.py <designer-binary> --ladder
    kernel_cases_smoke.py <designer-binary> --shard K/N
    kernel_cases_smoke.py --self-test

--ladder is the complexity ladder's tiers on one instance, less the cases
tests/kernel_cases/slow.json lists (a modelled thread takes half a minute),
against
tests/kernel_cases/status.json, with the vendored files checked against
SOURCE and the generated page (docs/modeling/kernel-cases.md) against the
rows: the headless suite's share. --shard K/N is every case whose name
hashes to K, on one instance (`slow;kernel_cases`); the shards together are
the corpus. --self-test feeds the translator, the grader and the ratchet
planted recipes and answers.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "tools", "kernel_cases.py")


def main():
    args = sys.argv[1:]
    if args == ["--self-test"]:
        return subprocess.call([sys.executable, TOOL, "self-test"])
    if len(args) < 2:
        sys.exit(__doc__)
    binary, mode = os.path.abspath(args[0]), args[1:]
    cmd = [sys.executable, TOOL, "run", binary, "--jobs", "1"]
    if mode == ["--ladder"]:
        cmd += ["--ladder", "--quick"]
    elif len(mode) == 2 and mode[0] == "--shard":
        cmd += ["--shard", mode[1]]
    else:
        sys.exit(__doc__)
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
