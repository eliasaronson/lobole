# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every text an agent reads names only what exists.

An agent on the MCP surface alone reads prose: the workflow prompt, the
server's instructions, the format texts, each tool's description, the
`help()` reference, the app's own `describe` rows and the documents
`help()` serves. A name in any of them that the app does not register (a
renamed key, a method folded into another, a wrapper's keyword that the
RPC never took) is found by the agent that copies it. This module finds it
first (docs/scripting/mcp.md#every-text-names-only-what-exists).

`unknown_names` is every call-shaped word in a text that nothing
registers:

- a `snake_case` word that is not a method, a tool, a key or a value
  `describe` publishes, nor a word of that text's VOCABULARY;
- whatever `rpc('...')` or `help('...')` names that is not a method or a
  tool;
- a key in `rpc('m', {...})`'s braces that `m`'s row does not take.

The prompt texts (`STRICT`) are held to that alone. The rest describe
what a call ANSWERS, which `describe` publishes only where a row states
`returns`; for them a word the app's or the package's source writes as a
quoted literal (`"peak_at"`) also counts as existing, so a renamed key,
whose old literal is gone, is still caught.

Used by mcp_formats_gate.py (headless, the default path) over every text,
and by mcp_smoke.py.
"""
import re

# Snake_case words that are not a method nor anything describe publishes,
# but the vocabulary of one, with where each comes from. Keyed by the text
# they appear in (the names `agent_texts` gives), so a word allowed in one
# text is not thereby allowed in all of them.
VOCABULARY: dict[str, dict[str, str]] = {
    "WORKFLOW": {
        "at_most": "a requirement's bound kind (add_requirement)",
        "at_least": "a requirement's bound kind (add_requirement)",
        "flange_width": "an example of a variable name",
        "factor_of_safety": "a study_result requirement's quantity "
                            "(add_requirement)",
        "buckling_factor": "a study_result requirement's quantity "
                           "(add_requirement)",
    },
    "FEATURE_FORMATS": {
        "corrected_frenet": "a Sweep's default frame (src/core/features)",
        "thread_mill": "a CAM operation and tool type (add_cam_op)",
        "three_points": "an align_import mode (src/core/ImportAlign.cpp)",
    },
    "CONSTRAINT_FORMATS": {
        "value_deg": "an angle constraint's degrees twin of value "
                     "(add_constraint)"},
    "row:export_bom": {
        "printed_unit_mass_g": "a CSV column (src/ui/Bom.cpp)",
        "printed_total_mass_g": "a CSV column (src/ui/Bom.cpp)"},
    # A key the row builds as a stem and the unit it is in, written once
    # per unit: `bound_mm | bound_deg`.
    "row:list_relations": {
        "bound_mm": "bound_ + b's unit", "bound_deg": "bound_ + b's unit",
        "height_deg": "height_ + b's unit"},
    "row:check_followers": {"clash_mm": "clash_ + the follower's unit",
                            "height_deg": "height_ + b's unit"},
    "row:run_motion_study": {
        "jump_deg": "jump_ + the mate's unit",
        "jump_mm": "jump_ + the mate's unit",
        "expected_deg": "expected_ + the mate's unit",
        "expected_mm": "expected_ + the mate's unit"},
    "row:check_seat": {
        "d_a": "SKF's symbol for a shaft shoulder's diameter",
        "r_a": "SKF's symbol for a fillet radius",
        "r_s": "SKF's symbol for a ring's chamfer"},
    "row:debug_fault": {"runtime_error": "the C++ exception it throws"},
}

# The texts whose every word is checked against describe alone: what the
# client is told before it asks anything.
STRICT = ("WORKFLOW", "instructions", "FEATURE_FORMATS",
          "CONSTRAINT_FORMATS")


def known_names(desc):
    """Every name `describe` publishes: methods, their parameter keys and
    what they answer, every key and enum value of their schemas, the
    feature types' contract keys and the words of their `params` text, and
    every list of names it serves (mate types, axis kinds, ...)."""
    known = set(desc)

    def walk(x):
        if isinstance(x, dict):
            for k, v in x.items():
                known.add(k)
                walk(v)
        elif isinstance(x, list):
            for v in x:
                if isinstance(v, str):
                    known.add(v)
                else:
                    walk(v)
    for key, value in desc.items():
        if key not in ("methods", "feature_types", "sketch_entity_types"):
            walk(value)
    for m in desc["methods"]:
        known.update(m.get("params", []))
        known.update(re.findall(r"[a-z][a-z0-9_]*", m.get("returns") or ""))
        walk(m.get("request_schema", {}))
        walk(m.get("result_schema", {}))
    for table in ("feature_types", "sketch_entity_types"):
        for ft in desc.get(table, []):
            for entry in (ft.get("contract") or "").split(", "):
                known.add(entry.split(":", 1)[0].split("^", 1)[0]
                          .rstrip("?"))
            known.update(re.findall(r"[a-z][a-z0-9_]*",
                                    ft.get("params", "")))
    return known


def source_literals(paths):
    """Every snake_case word the given source files write as a quoted
    literal, "word" or 'word'."""
    out = set()
    for p in paths:
        with open(p, encoding="utf-8") as f:
            out.update(re.findall(r"""(?<![\\\w])["']([a-z][a-z0-9]*"""
                                  r"""(?:_[a-z0-9]+)+)["']""", f.read()))
    return out


_WORD = re.compile(r"\b[a-z][a-z0-9]*(?:_[a-z0-9]+)+\b")
_CALLED = re.compile(r"\b(?:rpc|help)\(\s*['\"]([A-Za-z0-9_]+)['\"]")
_CALL_KEYS = re.compile(r"\brpc\(\s*['\"]([a-z0-9_]+)['\"]\s*,\s*\{")
_PATH = re.compile(r"[\w.-]*/[\w./#-]+|\b[\w-]+\.(?:py|md|json|lob|step|"
                   r"stl|png|cpp|h|3mf|dproj|txt|csv)\b")


def _braced(text, start):
    """The text of the {...} opening at `start`, nested braces kept."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return text[start + 1:]


def _top_level_keys(body):
    """The keys of a prose object, `{a, b: 1, 'c': [..]}`: the first word of
    each top-level entry."""
    depth, entry, keys = 0, "", []
    for ch in body + ",":
        if ch in "{[(":
            depth += 1
        elif ch in "}])":
            depth -= 1
        if ch == "," and depth == 0:
            m = re.match(r"\s*['\"]?([A-Za-z_][A-Za-z0-9_]*)['\"]?\s*(?::|$)",
                         entry)
            if m:
                keys.append(m.group(1))
            entry = ""
        else:
            entry += ch
    return keys


def unknown_names(text, desc, tools, vocabulary=None, known=None,
                  literals=None, topics=()):
    """The call-shaped names in `text` that nothing registers, sorted.

    `tools` are the MCP tools' names; `vocabulary` the words allowed for
    this text beyond them (VOCABULARY's entry, which says why); `literals`,
    where given, the source's quoted words, which count as existing too;
    `topics` what help('x') answers besides a method (a document, the
    examples).
    A path or a file name is not a name and is skipped (docs_paths checks
    paths)."""
    rows = {m["name"]: m for m in desc["methods"]}
    if known is None:
        known = known_names(desc)
    allowed = set(vocabulary or {}) | set(literals or ())
    stripped = _PATH.sub(" ", text)

    def exists(w):
        if w in rows or w in tools or w in known or w in allowed:
            return True
        # `x_expr` is the expression twin of `x` (or of `x_deg`).
        if w.endswith("_expr"):
            base = w[:-len("_expr")]
            return exists(base) or exists(base + "_deg") or \
                base in ("x", "y", "z", "r")
        return False
    unknown = {w for w in _WORD.findall(stripped) if not exists(w)}
    # rpc('x') and help('x') are calls whatever else x may be: x must be a
    # method or a tool.
    unknown |= {c for c in _CALLED.findall(stripped)
                if c not in rows and c not in tools and c not in topics}
    # rpc('m', {a, b}) names m's keys: each must be one m takes.
    for m in _CALL_KEYS.finditer(stripped):
        row = rows.get(m.group(1))
        if row is None or row.get("params_free_form"):
            continue
        takes = set(row.get("params", []))
        for key in _top_level_keys(_braced(stripped, m.end() - 1)):
            if key not in takes:
                unknown.add(f"{m.group(1)}.{key}")
    return sorted(unknown)


def docs_paths(text):
    """Every `docs/...` path a text cites, anchors kept."""
    return sorted(set(m.rstrip(".,;:)'\"`") for m in
                      re.findall(r"docs/[\w./#-]+", text)))


def agent_texts(m, desc, tools):
    """Every text an agent reads, by name: `m` is lobole.mcp, `tools` the
    announced tools as {name: description}."""
    out = {"WORKFLOW": m.WORKFLOW, "instructions": m.INSTRUCTIONS,
           "FEATURE_FORMATS": m.FEATURE_FORMATS,
           "CONSTRAINT_FORMATS": m.CONSTRAINT_FORMATS}
    for name, description in tools.items():
        out["tool:" + name] = description or ""
    for name, entry in m._REFERENCE.items():
        out["reference:" + name] = entry["doc"]
    for row in desc["methods"]:
        out["row:" + row["name"]] = row["doc"]
    for path in getattr(m, "AGENT_DOCS", ()):
        out["doc:" + path] = m.document_section(path) or ""
    return out


def unserved_paths(name, text, m):
    """The docs/... citations in `text` that help() does not serve, or whose
    #anchor names no heading there. A served document's relative links
    (`../variables/README.md#x`) are its citations too."""
    import posixpath
    cited = docs_paths(text)
    if name.startswith("doc:"):
        base = posixpath.dirname(name[len("doc:"):])
        for link in re.findall(r"\]\(([^)\s]+)\)", text):
            if "://" in link or link.startswith("#"):
                continue
            cited.append(posixpath.normpath(posixpath.join(base, link)))
    bad = []
    for c in cited:
        path, _, anchor = c.partition("#")
        if m.document_section(path, anchor) is None:
            bad.append(c)
    return sorted(set(bad))


def scan(texts, desc, tools, literals, topics=()):
    """{text name: [unknown names]} for every text with any."""
    known = known_names(desc)
    found = {}
    for name, text in texts.items():
        u = unknown_names(text, desc, tools, VOCABULARY.get(name),
                          known=known,
                          literals=None if name in STRICT else literals,
                          topics=topics)
        if u:
            found[name] = u
    return found
