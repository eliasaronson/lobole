# Driving Designer over MCP

`tools/designer_mcp.py` exposes a running Designer instance to any MCP client
(Claude Code, Claude Desktop, custom agents). Design: [s1-mcp.md](s1-mcp.md).

## Setup

1. Start the app with the remote interface: `designer --remote 8437`
   — or with no window/GPU/display at all: `designer --headless --remote 8437`
   (full modeling + `mass_properties`/`list_elements`; `screenshot`,
   `render_still`, face selection and `ui_action` error with the remedy)
2. Register the server with your client. Claude Code sessions started in this
   repo pick it up automatically from the checked-in **`.mcp.json`**
   (approve the project server when prompted; `python3` must have the `mcp`
   SDK — run Claude Code from inside `nix-shell`, or `pip install mcp`).
   For other clients:

   ```
   claude mcp add designer -- python3 /path/to/Designer/tools/designer_mcp.py --port 8437
   ```

   The port also comes from `$DESIGNER_PORT`; the flag wins.

The server connects lazily — registering before the app is running is fine;
tools error with the remedy until it is reachable.

## What's exposed

30 tools mirroring the JSON-RPC remote surface (same names): document state
(`get_state`), features (`add_feature`, `modify_feature`, …), sketch
constraints and ops (`add_constraint`, `fillet`, `mirror`, `pattern`,
`offset`), variables, undo/redo, project I/O, and the verification loop —
`mass_properties` (volume/CoM/inertia/mass) and `list_elements` (stable
face/edge names for Fillet/Chamfer refs) as the geometry oracles;
`screenshot` and `render_still` return inline images; `view` snaps standard
views. `ui_action` exposes raw GUI events (menus, clicks, keys) for
driving/testing the GUI itself.

The `modeling-workflow` prompt teaches the loop that works: variables first,
constrain fully, verify with `get_state` + `screenshot` after every step.

The feature/entity/constraint JSON dialects are documented in the tool
descriptions; the source of truth is `Serialization.cpp` (`FeatureFromJson`)
— when a new feature type lands there, add it to `FEATURE_FORMATS` in
`designer_mcp.py` (standing rule: a feature an LLM can't drive is
half-shipped).

## Testing

`ctest -R mcp_smoke` models a dimensioned bracket end-to-end through the MCP
layer (skips if the `mcp` SDK is missing). `remote_smoke` covers the RPC
underneath.
