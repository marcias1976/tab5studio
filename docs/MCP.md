# Designing with Claude (MCP) — optional

`mcp/tab5studio_mcp.py` is an [MCP](https://modelcontextprotocol.io) server: it lets Claude (Claude Code, Claude Desktop or another MCP client) design Tab5 screens in your Tab5 Studio. You describe a screen ("a big clock in Rajdhani, the weather now on the right, the forecast below, amber"), Claude builds it as a Studio project, shows it on the Tab5 as a live preview, looks at a screenshot of the Tab5 and fixes what does not look right.

- **Optional:** nothing starts it unless you add it to your MCP client. Studio and the Tab5 work the same without it.
- **Nothing on the Tab5:** the server only calls Studio's own API (projects, live preview, Tab5 screenshot, save). The Tab5 gets the same live preview as from the Studio button; no firmware change.
- **Nothing new on the network:** the MCP client starts the script on your computer over stdio; it signs in to Studio with Studio's password (HTTP Basic, like scripts).
- **Python 3.8+, standard library only:** no `pip install`.
- **Cost:** your own Claude plan; screenshots use a little more of it.

## Setup

You need: Tab5 Studio running, and (for the preview and screenshots) the Tab5 address and PIN set in Studio → Preview → Tab5 connection.

**Claude Code** (on the Studio server, from the repo folder):

```bash
claude mcp add tab5studio -e STUDIO_URL=http://127.0.0.1:8080 -e STUDIO_PASSWORD='<Studio password>' \
  -- python3 "$PWD/mcp/tab5studio_mcp.py"
```

**Claude Desktop:** Settings → Developer → Edit config (`claude_desktop_config.json`), see `mcp/claude_desktop_config.example.json`:

```json
{
  "mcpServers": {
    "tab5studio": {
      "command": "python3",
      "args": ["/path/to/tab5studio/mcp/tab5studio_mcp.py"],
      "env": { "STUDIO_URL": "http://<Studio address>:8080", "STUDIO_PASSWORD": "<Studio password>" }
    }
  }
}
```

The script may run on another computer than Studio: copy `mcp/tab5studio_mcp.py` there and point `STUDIO_URL` at Studio. Keep Studio in your home network (or behind your VPN); the password travels as HTTP Basic, the same as the browser login.

Then ask Claude, for example: *"Using tab5studio, design a Wariant-1 with a large clock and the 3-day forecast in cyan, show it on my Tab5 and check the screenshot."*

## Tools

| Tool | What it does |
|---|---|
| `get_reference` | the scene format: screen, faces, element types and fields, tap actions, layout rules, this Studio's fonts and images (Claude reads it first) |
| `list_projects`, `get_project` | Studio projects, a project's scene (or one face) as JSON |
| `create_project` | a new project: empty clock faces or a copy of a project; the name is cleaned as Studio stores it (letters, digits, spaces, `-`, `_`), and an existing name is refused unless `overwrite` |
| `set_face` | replace all elements of a face (+ background, palette, `enabled` / `builtin` / `ms`) |
| `update_elements` | change elements by id, remove, add |
| `check_layout` | estimated boxes: off screen, overlaps, under the timer pill (top left) or the menu button (top right), missing `stop` / `timer_close` |
| `search_icons` | Lucide icons by name → code points |
| `preview_on_tab5` | live preview of a project on the Tab5 (`/api/live`), opening the given face |
| `tab5_screenshot` | what the Tab5 shows now (`/api/tab5/screen`, JPEG) |
| `stop_preview` | end the live preview |
| `save_to_tab5` | save on the Tab5 for good and set its palette (`/api/live` with `save`); only when the user asks |

`set_face` and `update_elements` write the project file at once (`PUT /api/projects/<name>`) and return the layout check. Numbers are stored whole (as Studio stores them), so `opacity: 80.0` becomes `80`. `update_elements` on a face an older project lacks asks for `set_face` first. Designed elements on intro / alarm / timer switch `builtin` off, elements on Wariant-3 switch `enabled` on. `save_to_tab5` sends Studio's `sceneHash`, so Studio shows the saved project as up to date.

A project open in the Studio editor does not reload by itself: open it again to see Claude's changes (saving the old copy in the editor would overwrite them). Claude is told to design in its own new project, not in yours, unless you ask.

## Protocol

JSON-RPC 2.0 over stdio, one message per line: `initialize`, `ping`, `tools/list`, `tools/call`. Errors from Studio or from checking the elements come back as a tool result with `isError: true`, in plain words, so Claude can fix the call (a wrong `STUDIO_PASSWORD` says so). A line that is not a JSON-RPC object gets `Invalid Request`; the server keeps running.

## Checking it works

Without Studio (the protocol and the tool list):

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | python3 mcp/tab5studio_mcp.py
```

With Studio, ask Claude in a fresh session: *"Using tab5studio, list my projects"*. Then try a test project with `preview_on_tab5` + `tab5_screenshot` and `stop_preview`, and delete the test project in Studio (Open project → Delete) when done.
