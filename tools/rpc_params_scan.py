#!/usr/bin/env python3
"""Extract each RPC method's accepted parameter vocabulary from the source.

The vocabulary a method accepts is not written down anywhere a program can
read: `RpcParams` records it at RUNTIME, in `SeenKeys()`, but only by running
the handler — the unknown-key check is deliberately post-execution
(RemoteServer.cpp, "we learn a method's vocabulary by which keys it reads").
So a schema for `describe` cannot be harvested; it has to be declared and then
KEPT true. This module is the "kept true" half: it reads the handlers and says
what keys each branch actually looks up, and tests/rpc_params_gate.py diffs
that against the MethodDoc tables.

Why brace-aware rather than a regex sweep: handlers group methods that share a
preamble —

    if (m == "set_occurrence_transform" || m == "set_occurrence_visible" ||
        m == "set_occurrence_fixed" || ...) {
      Occurrence* occ = state.assembly->FindById(p.at("id")...);   // all five
      if (m == "set_occurrence_transform") {
        ... p.at("transform") ...                                  // just one
      }

so `id` belongs to five methods and `transform` to one. Splitting the file at
each `m ==` mention gets 35% of branches wrong; tracking the brace depth of
each method scope gets it right, because a key belongs to whichever method
scope encloses it most tightly.
"""
import re
import sys

# `p` is the RpcParams by convention in every handler; `params` appears in the
# server's own dispatch. Both spellings, so a rename shows up as a diff rather
# than as silence.
KEY_RE = re.compile(
    r'\b(?:p|params|request\.params)\s*'
    r'(?:\.\s*(?:at|value|contains)\s*\(|\[)\s*"([A-Za-z0-9_]+)"'
    r'|\bneed\s*\(\s*(?:p|params)\s*,\s*"([A-Za-z0-9_]+)"')
RAW_RE = re.compile(r'\b(?:p|params)\s*\.\s*raw\s*\(\s*\)')
# Method names in a condition: `m == "x"`, and the `||` chains that group
# them. `request.method` is the second spelling: `screenshot` and
# `render_sheet` are staged asynchronously BEFORE the RpcParams path exists,
# so they match on the raw request and read `request.params` directly. That
# also means the unknown-key rejection does not cover those two — worth
# knowing, and a reason to have their vocabulary written down.
COND_RE = re.compile(r'\b(?:m|request\.method)\s*==\s*"([A-Za-z0-9_]+)"')
# Two escape hatches, both greppable so the exceptions stay a visible list:
#   // rpc-params: opaque        — the vocabulary is genuinely free-form
#   // rpc-params: a, b, c       — these keys too, read somewhere the scanner
#                                  cannot see (a lambda taking the key as an
#                                  argument, a helper in another file)
OPT_OUT = "rpc-params: opaque"
EXTRA_RE = re.compile(r'rpc-params:\s*([a-z0-9_]+(?:\s*,\s*[a-z0-9_]+)*)\s*$',
                      re.M)


def _strip(src):
    """Blank out comments and string literals, preserving offsets so the
    positions we compute still index into the ORIGINAL text. A method name
    inside a comment or an error message must not read as a branch."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            i = j + 1
        else:
            i += 1
    return ''.join(out)


def scan_source(src):
    """{method: {"keys": sorted[str], "opaque": bool}} for one .cpp."""
    # Comments are blanked (a method name in a comment is not a branch) but
    # string literals are KEPT, because the key names and the branch names
    # both live in them. Offsets are preserved either way.
    bare = _strip(src)

    out = {}

    def note(names, key=None, opaque=False):
        for nm in names:
            e = out.setdefault(nm, {"keys": set(), "opaque": False})
            if key:
                e["keys"].add(key)
            if opaque:
                e["opaque"] = True

    # Walk the file once, tracking brace depth and a stack of open method
    # scopes. A scope opens at the `{` following a condition that mentions
    # `m == "..."`, and closes when the depth returns below it.
    stack = []          # [(names, depth_at_open)]
    depth = 0
    i, n = 0, len(bare)
    pending = None      # names seen in a condition, awaiting its `{`
    while i < n:
        c = bare[i]
        if c == '{':
            depth += 1
            if pending:
                stack.append((pending, depth))
                pending = None
        elif c == '}':
            while stack and stack[-1][1] == depth:
                stack.pop()
            depth -= 1
        elif c == 'i' and bare.startswith('if', i) and \
                not (i and (bare[i - 1].isalnum() or bare[i - 1] == '_')):
            # Read the condition; if it names methods, they scope the block.
            j = bare.find('(', i)
            if j >= 0:
                d, k = 0, j
                while k < n:
                    if bare[k] == '(':
                        d += 1
                    elif bare[k] == ')':
                        d -= 1
                        if d == 0:
                            break
                    k += 1
                names = COND_RE.findall(bare[j:k + 1])
                if names:
                    pending = names
                i = k
        i += 1

    # Second pass for the keys: for each match, find the innermost enclosing
    # method scope by re-walking depths. Cheaper and clearer than threading
    # both through one loop, and this file is 9k lines total.
    scopes = _scopes(bare)
    for mm in KEY_RE.finditer(bare):
        owner = _innermost(scopes, mm.start())
        if owner:
            note(owner, key=mm.group(1) or mm.group(2))
    for mm in RAW_RE.finditer(bare):
        owner = _innermost(scopes, mm.start())
        if owner:
            note(owner, opaque=True)
    # The two escape hatches. `opaque` first, so a branch marked opaque is
    # not also parsed as a key list.
    for mm in re.finditer(re.escape(OPT_OUT), src):
        owner = _innermost(scopes, mm.start())
        if owner:
            note(owner, opaque=True)
    for mm in EXTRA_RE.finditer(src):
        if mm.group(1).strip() == "opaque":
            continue
        owner = _innermost(scopes, mm.start())
        if owner:
            for k in (x.strip() for x in mm.group(1).split(",")):
                note(owner, key=k)
    # A branch with no keys at all is a real answer (a parameterless method),
    # so make sure every named method appears even when it read nothing.
    for names, _s, _e in scopes:
        note(names)
    return {k: {"keys": sorted(v["keys"]), "opaque": v["opaque"]}
            for k, v in out.items()}


def _scopes(bare):
    """[(names, start, end)] for every method scope, by brace matching."""
    out, stack, depth = [], [], 0
    pending = None
    i, n = 0, len(bare)
    while i < n:
        c = bare[i]
        if c == '{':
            depth += 1
            if pending:
                stack.append((pending, i, depth))
                pending = None
        elif c == '}':
            while stack and stack[-1][2] == depth:
                names, start, _ = stack.pop()
                out.append((names, start, i))
            depth -= 1
        elif c == 'i' and bare.startswith('if', i) and \
                not (i and (bare[i - 1].isalnum() or bare[i - 1] == '_')):
            j = bare.find('(', i)
            if j >= 0:
                d, k = 0, j
                while k < n:
                    if bare[k] == '(':
                        d += 1
                    elif bare[k] == ')':
                        d -= 1
                        if d == 0:
                            break
                    k += 1
                names = COND_RE.findall(bare[j:k + 1])
                if names:
                    pending = names
                i = k
        i += 1
    while stack:
        names, start, _ = stack.pop()
        out.append((names, start, n))
    return out


def _innermost(scopes, pos):
    best = None
    for names, start, end in scopes:
        if start <= pos <= end:
            if best is None or start > best[1]:
                best = (names, start)
    return best[0] if best else None


def scan_files(paths):
    merged = {}
    for path in paths:
        with open(path) as f:
            for name, info in scan_source(f.read()).items():
                e = merged.setdefault(name, {"keys": set(), "opaque": False})
                e["keys"].update(info["keys"])
                e["opaque"] = e["opaque"] or info["opaque"]
    return {k: {"keys": sorted(v["keys"]), "opaque": v["opaque"]}
            for k, v in sorted(merged.items())}


if __name__ == "__main__":
    for name, info in scan_files(sys.argv[1:]).items():
        mark = " (opaque)" if info["opaque"] else ""
        print(f"{name}: {', '.join(info['keys']) or '-'}{mark}")
