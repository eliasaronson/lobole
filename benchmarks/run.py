#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Run the Designer agent benchmark: every arm x task x repetition, graded.

    nix-shell shell.nix --run "python3 benchmarks/run.py --reps 3"
    ... --arms full --tasks t0-plate --reps 1        # one cell, for iterating
    ... --dry-run                                    # print the plan, spend nothing

WHAT IT MEASURES, AND WHY IT IS SHAPED THIS WAY
-----------------------------------------------
The subject under test is DESIGNER'S API SURFACE, not the model. So each run
gets a fresh, empty, isolated Designer instance on a kernel-chosen port and an
agent with NO built-in tools at all (`--tools ""`): it cannot read the repo,
cannot shell out, cannot look at the source. Whatever it manages to build, it
built through the MCP surface alone. That is the thing a change to a tool
description, a refusal message or a `help()` string moves — and the thing no
C++ test can see move.

Three things are deliberately taken out of the agent's hands:

* **The export.** The runner exports the STEP itself when the agent stops.
  Grading an agent that forgot to export tells you about the agent, not the
  API, and we already know agents forget.
* **The verdict.** `benchmarks/oracle/` reads the STEP back with OpenCASCADE
  and grades name-independent geometry. The app is never asked whether it
  succeeded.
* **The port.** Instances are launched here and pinned into the MCP config, so
  a run can never wander onto the developer's own instance on 8799 and start
  editing their open document.

Runs are SEQUENTIAL on purpose: wall-clock is one of the numbers, and a
machine running four agents and four CAD kernels does not produce a wall-clock
anyone can compare.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "benchmarks")
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(BENCH, "oracle"))
from smoke_app import launch          # noqa: E402
from grade import grade_file          # noqa: E402

# Everything the agent is told beyond the task itself. Identical for every arm
# — an arm that needs its own briefing puts it in `append_system_prompt`.
HARNESS_PREFIX = """You are connected to a running instance of Loböle, a
parametric CAD application, through its MCP tools. A new empty document is
already open. Build the part described below in that document.

Work only through the Designer tools — you have no other tools. When you are
finished, reply with one short sentence saying whether you built the part
successfully or not.

--- PART ---
"""


def now_stamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def load_json_dir(path, wanted):
    out = []
    for f in sorted(os.listdir(path)):
        if not f.endswith(".json"):
            continue
        d = json.load(open(os.path.join(path, f)))
        if not wanted or d.get("id", d.get("name")) in wanted:
            out.append(d)
    return out


def prompt_for(task):
    """The prompt the agent gets — and nothing the grader keeps back.

    A Fit task carries the geometry it will be graded against, and the agent
    must never see it: hand over the tube and the task stops being "design a
    cap" and becomes "boolean these two". Today that holds because nothing
    puts `inputs` in the prompt at all, which is luck rather than a rule. This
    is the rule: an input reaches the prompt only if it says it may, and a
    host that claims it may is a broken task rather than a leak nobody
    noticed.
    """
    parts = [HARNESS_PREFIX + task["prompt"]]
    for i in task.get("inputs", []):
        if not isinstance(i, dict):
            continue
        if i.get("kind") == "host_geometry" and i.get("agent_visible"):
            raise SystemExit(
                f"{task['id']}: host_geometry is agent_visible — that is the "
                f"answer to the task, and the grader keeps it")
        if i.get("agent_visible") and i.get("text"):
            parts.append(i["text"])
    return "\n\n".join(parts)


def mcp_config(port, arm):
    """The agent's whole world: one server, pinned to this run's instance."""
    if arm.get("server") == "document":
        # The direct-emission arm gets a different server entirely — one tool
        # that takes a whole document, and no RPC surface to drive. It lives
        # in benchmarks/arms/ and not in tools/, because it is an instrument
        # and not a capability we ship.
        args = [os.path.join(ROOT, "benchmarks", "arms", "document_mcp.py"),
                "--port", str(port)]
        return {"mcpServers": {"lobole":
                               {"command": sys.executable, "args": args}}}
    args = ["-m", "lobole.mcp", "--port", str(port), "--no-autostart"]
    if arm.get("only_tools"):
        args += ["--only-tools", ",".join(arm["only_tools"])]
    return {"mcpServers": {"lobole": {
        "command": sys.executable, "args": args,
        "env": {"PYTHONPATH": os.path.join(ROOT, "python")}}}}


def run_agent(prompt, cfg_path, arm, model, cwd, budget, timeout):
    """One `claude -p` session. Returns the parsed stream plus raw stdout."""
    cmd = [
        "claude", "-p", prompt,
        "--model", model,
        "--strict-mcp-config", "--mcp-config", cfg_path,
        "--tools", "",                    # no built-in tools: MCP or nothing
        "--setting-sources", "",          # no user/project/local settings
        "--disable-slash-commands",
        "--permission-mode", "bypassPermissions",
        "--no-session-persistence",
        "--output-format", "stream-json", "--verbose",
        "--max-budget-usd", str(budget),
    ]
    if arm.get("append_system_prompt"):
        cmd += ["--append-system-prompt", arm["append_system_prompt"]]
    os.makedirs(cwd, exist_ok=True)   # right before launch, not minutes before
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                              timeout=timeout)
        stdout, err, timed_out = proc.stdout, proc.stderr, False
    except subprocess.TimeoutExpired as e:
        stdout = e.stdout.decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        err, timed_out = f"timed out after {timeout}s", True
    return parse_stream(stdout, err, timed_out, time.time() - t0), stdout


def parse_stream(stdout, stderr, timed_out, wall):
    """Pull the numbers out of stream-json: cost, tokens, turns, tool calls.

    Tool calls are counted BY NAME. Which tools an arm actually reaches for is
    half the finding — "it never called describe" explains a failure that a
    total call count only reports.
    """
    out = {"cost_usd": None, "duration_ms": None, "num_turns": None,
           "tool_calls": 0, "tools_used": {}, "final_text": "",
           "error": stderr.strip()[:600] if stderr else "",
           "timed_out": timed_out, "wall_s": round(wall, 1),
           "input_tokens": None, "output_tokens": None}
    for line in stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue
        if ev.get("type") == "assistant":
            for block in ev.get("message", {}).get("content", []):
                if block.get("type") == "tool_use":
                    name = block.get("name", "?").replace("mcp__designer__", "")
                    out["tool_calls"] += 1
                    out["tools_used"][name] = out["tools_used"].get(name, 0) + 1
        elif ev.get("type") == "result":
            out["cost_usd"] = ev.get("total_cost_usd")
            out["duration_ms"] = ev.get("duration_ms")
            out["num_turns"] = ev.get("num_turns")
            out["final_text"] = (ev.get("result") or "")[:2000]
            u = ev.get("usage") or {}
            out["input_tokens"] = u.get("input_tokens")
            out["output_tokens"] = u.get("output_tokens")
            if ev.get("is_error"):
                out["error"] = (out["error"] + " " + str(ev.get("subtype", ""))).strip()
    return out


CLAIM_SCHEMA = json.dumps({
    "type": "object",
    "properties": {"claimed_success": {"type": "boolean"}},
    "required": ["claimed_success"],
})


def claimed_success(text):
    """Did the agent's last message claim it built the part?

    An over-claim — 'Done, the flange is complete' over a part with no holes in
    it — is the failure this project cares about most, so it gets classified by
    a model rather than by keyword luck. Returns None if the classifier itself
    failed, which is recorded as unknown rather than guessed.
    """
    if not text.strip():
        return False
    q = ("Here is an agent's final message after a CAD modelling task. Did it "
         "claim it SUCCEEDED in building the part? Answer only with the JSON.\n\n"
         "--- message ---\n" + text[:2000])
    try:
        p = subprocess.run(["claude", "-p", q, "--model", "haiku", "--tools", "",
                            "--setting-sources", "", "--no-session-persistence",
                            "--strict-mcp-config", "--json-schema", CLAIM_SCHEMA,
                            "--output-format", "json"],
                           capture_output=True, text=True, timeout=180)
        res = json.loads(p.stdout)
        payload = res.get("result") if isinstance(res, dict) else None
        if isinstance(payload, str):
            payload = json.loads(payload)
        return bool(payload["claimed_success"])
    except Exception:
        return None


def doc_grade(state, variables, checks):
    """The few checks geometry cannot answer — 'is this DRIVEN by a variable?'

    Marked app-reported everywhere it surfaces. It is the app's own account of
    itself, so it is evidence, not a verdict, and it is never mixed into the
    oracle's score.
    """
    results = []
    blob = json.dumps(state)
    byname = {v.get("name"): v for v in (variables or {}).get("variables", [])}
    for c in checks:
        ok = False
        got = None
        if c["kind"] == "variable":
            v = byname.get(c["name"])
            got = v.get("value") if v else None
            try:
                ok = got is not None and abs(float(got) - c["value"]) < 1e-6
            except (TypeError, ValueError):
                ok = False
        elif c["kind"] == "expr_references":
            # An expression mentioning the variable must survive somewhere in
            # the feature tree; a retyped literal leaves no trace of the name.
            got = c["variable"] in blob
            ok = bool(got)
        results.append({**c, "pass": bool(ok), "got": got, "source": "app"})
    return results


def one_run(task, arm, rep, model, outdir, args):
    rid = f"{task['id']}__{arm['name']}__r{rep}"
    rundir = os.path.join(outdir, "runs", rid)
    os.makedirs(rundir, exist_ok=True)
    step = os.path.join(rundir, "part.step")
    # An empty cwd, outside the repo: no CLAUDE.md to discover, nothing to read
    # even if the agent had tools to read it with.
    cwd = os.path.join(rundir, "cwd")

    rec = {"task": task["id"], "tier": task["tier"], "arm": arm["name"],
           "rep": rep, "model": model, "id": rid}
    with launch(args.binary) as app:
        d = app.designer
        d.call("new_project")
        cfg = os.path.join(rundir, "mcp.json")
        json.dump(mcp_config(app.port, arm), open(cfg, "w"), indent=2)

        agent, raw = run_agent(prompt_for(task), cfg, arm, model,
                               cwd, args.budget, args.timeout)
        open(os.path.join(rundir, "stream.jsonl"), "w").write(raw)
        rec.update(agent)
        # The agent never got off the ground: no turns, no cost, an error from
        # the CLI itself. That is a broken CELL, not a failed part, and calling
        # it 0/5 would quietly slander whichever arm it landed in.
        if agent["num_turns"] is None and agent["error"]:
            rec["harness_error"] = agent["error"][:400]

        # The runner exports; the agent is never asked to. A part that exists
        # in the document counts, whether or not the agent thought to save it.
        try:
            d.call("export_step", path=step)
            rec["exported"] = True
        except Exception as e:
            rec["exported"] = False
            rec["export_error"] = str(e)[:300]
        for name, fn in (("state", lambda: d.call("get_state")),
                         ("variables", lambda: d.call("list_variables"))):
            try:
                rec[f"_{name}"] = fn()
            except Exception:
                rec[f"_{name}"] = {}

    graded = grade_file(task, step)
    rec["passed"] = graded["passed"]
    rec["total"] = graded["total"]
    rec["checks"] = [{k: v for k, v in c.items() if k != "present"}
                     for c in graded["checks"]]
    rec["facts"] = graded["facts"]
    if task.get("doc_checks"):
        rec["doc_checks"] = doc_grade(rec.pop("_state", {}),
                                      rec.pop("_variables", {}),
                                      task["doc_checks"])
    rec.pop("_state", None)
    rec.pop("_variables", None)

    claim = claimed_success(rec.get("final_text", "")) if args.classify else None
    rec["claimed_success"] = claim
    rec["over_claim"] = bool(claim) and rec["passed"] < rec["total"]
    json.dump(rec, open(os.path.join(rundir, "record.json"), "w"), indent=2)
    return rec


def summarize(records, outdir, meta):
    """One table per arm. Sums, not averages of averages."""
    broken = [r for r in records if r.get("harness_error")]
    records = [r for r in records if not r.get("harness_error")]
    meta = dict(meta)
    arms = sorted({r["arm"] for r in records})
    lines = [f"# Designer benchmark — {meta['stamp']}", "",
             f"model `{meta['model']}` · {meta['reps']} rep(s) · "
             f"{len(meta['tasks'])} tasks · sequential on one machine", "",
             "| arm | checks passed | tasks clean | pass^k | over-claims | "
             "tool calls | cost USD | wall s |",
             "|---|---|---|---|---|---|---|---|"]
    if broken:
        lines[3:3] = [f"**{len(broken)} cell(s) excluded — the agent never "
                      f"started, so they score nothing rather than zero:** "
                      + ", ".join(f"`{r['task']}/{r['arm']}/r{r['rep']}`"
                                  for r in broken), ""]
    for a in arms:
        rs = [r for r in records if r["arm"] == a]
        passed = sum(r["passed"] for r in rs)
        total = sum(r["total"] for r in rs)
        clean = sum(1 for r in rs if r["passed"] == r["total"])
        over = sum(1 for r in rs if r["over_claim"])
        calls = sum(r["tool_calls"] for r in rs)
        cost = sum(r["cost_usd"] or 0 for r in rs)
        wall = sum(r["wall_s"] or 0 for r in rs)
        # pass^k: a task counts only if EVERY rep of it was clean. Averaging
        # reps reports a one-in-three failure as 67%, which is the wrong
        # shape for "can an agent do this reliably" — the question the arms
        # exist to compare. mecheval's rule, and the harsher reading is the
        # honest one.
        tasks = sorted({r["task"] for r in rs})
        passk = sum(1 for t in tasks
                    if all(x["passed"] == x["total"]
                           for x in rs if x["task"] == t))
        lines.append(f"| {a} | {passed}/{total} | {clean}/{len(rs)} | "
                     f"{passk}/{len(tasks)} | {over} | "
                     f"{calls} | {cost:.2f} | {wall:.0f} |")
    lines += ["", "## Per task", "",
              "| task | tier | " + " | ".join(arms) + " |",
              "|---|---|" + "---|" * len(arms)]
    for t in meta["tasks"]:
        row = [f"| {t} | "]
        tier = next((r["tier"] for r in records if r["task"] == t), "")
        row.append(f"T{tier} | ")
        for a in arms:
            rs = [r for r in records if r["task"] == t and r["arm"] == a]
            row.append(f"{sum(r['passed'] for r in rs)}/"
                       f"{sum(r['total'] for r in rs)} | ")
        lines.append("".join(row))
    text = "\n".join(lines) + "\n"
    open(os.path.join(outdir, "summary.md"), "w").write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arms", default="", help="comma-separated; default all")
    ap.add_argument("--tasks", default="", help="comma-separated; default all")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--model", default="sonnet")
    ap.add_argument("--budget", type=float, default=2.0,
                    help="per-run USD ceiling handed to claude --max-budget-usd")
    ap.add_argument("--timeout", type=int, default=900, help="per-run seconds")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "lobole"))
    ap.add_argument("--out", default=os.path.join(BENCH, "results"))
    ap.add_argument("--no-classify", dest="classify", action="store_false",
                    help="skip the over-claim classifier (saves a haiku call)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not shutil.which("claude"):
        sys.exit("benchmark: no `claude` on PATH — the arms are claude -p runs.")
    if not os.path.exists(args.binary):
        sys.exit(f"benchmark: no designer binary at {args.binary}")
    oracle = os.path.join(ROOT, "build", "bench_oracle")
    if not os.path.exists(oracle):
        sys.exit("benchmark: build bench_oracle first (ninja bench_oracle) — "
                 "without it there is nothing to grade with.")

    tasks = load_json_dir(os.path.join(BENCH, "tasks"),
                          set(filter(None, args.tasks.split(","))))
    arms = load_json_dir(os.path.join(BENCH, "arms"),
                         set(filter(None, args.arms.split(","))))
    if not tasks or not arms:
        sys.exit("benchmark: nothing selected")

    plan = [(t, a, r) for t in tasks for a in arms
            for r in range(1, args.reps + 1)]
    print(f"{len(plan)} runs: {len(tasks)} tasks x {len(arms)} arms x "
          f"{args.reps} reps, model {args.model}, "
          f"<= ${args.budget * len(plan):.0f} ceiling")
    if args.dry_run:
        for t, a, r in plan:
            print(f"  {t['id']:24s} {a['name']:10s} r{r}")
        return 0

    stamp = now_stamp()
    outdir = os.path.join(args.out, f"{stamp}-{args.model}")
    os.makedirs(outdir, exist_ok=True)
    meta = {"stamp": stamp, "model": args.model, "reps": args.reps,
            "tasks": [t["id"] for t in tasks], "arms": [a["name"] for a in arms],
            "git": subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                                  cwd=ROOT, capture_output=True,
                                  text=True).stdout.strip()}
    json.dump(meta, open(os.path.join(outdir, "meta.json"), "w"), indent=2)

    records = []
    jsonl = open(os.path.join(outdir, "runs.jsonl"), "a")
    for i, (t, a, r) in enumerate(plan, 1):
        print(f"[{i}/{len(plan)}] {t['id']} / {a['name']} / rep {r} ... ",
              end="", flush=True)
        try:
            rec = one_run(t, a, r, args.model, outdir, args)
        except Exception as e:  # one bad cell must not lose the sweep
            rec = {"task": t["id"], "tier": t["tier"], "arm": a["name"],
                   "rep": r, "model": args.model, "passed": 0, "total":
                   len(t["checks"]), "harness_error": str(e)[:400],
                   "tool_calls": 0, "cost_usd": 0, "wall_s": 0,
                   "over_claim": False}
            print(f"HARNESS ERROR: {e}")
        else:
            if rec.get("harness_error"):
                print(f"HARNESS ERROR (excluded): "
                      f"{rec['harness_error'][:120]}")
            else:
                flag = "  OVER-CLAIM" if rec["over_claim"] else ""
                print(f"{rec['passed']}/{rec['total']} checks, "
                      f"{rec['tool_calls']} calls, ${rec['cost_usd'] or 0:.2f}, "
                      f"{rec['wall_s']}s{flag}")
        records.append(rec)
        jsonl.write(json.dumps(rec) + "\n")
        jsonl.flush()
    jsonl.close()
    print()
    print(summarize(records, outdir, meta))
    print(f"results: {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
