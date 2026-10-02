# BounceBot

A scripted playtester for Bounce. One call runs a whole route: it moves, jumps, slams, looks around, takes screenshots and checks values, then returns one compact report with the screenshots. That replaces dozens of small editor MCP round-trips.

```
Claude ──bouncebot_run──▶ Tools/BounceBot/mcp/server.mjs ──HTTP :8765──▶ BounceEditor module (starts/reuses PIE)
                                       │                                         │
                                       └── no editor? launches -game ──▶ UBounceBotSubsystem (runs the script)
                                                                                 └▶ Saved/BounceBot/Runs/<time>-<name>/
                                                                                      report.json, trace.csv, NN_label.jpg
```

## Pieces

| File | What it does |
|---|---|
| `Source/Bounce/.../BounceBotSubsystem.*` | The runner, a game-instance subsystem. Steps run off the game tick and inputs go through Enhanced Input injection, so the real bindings get exercised. It records telemetry (bounces, apex, distance, a 10 Hz trace) and captures warnings/errors logged during the run. |
| `Source/BounceEditor/` | Editor-only HTTP bridge on `127.0.0.1:8765` (`POST /run`, `GET /status`, `POST /stop`). It starts PIE in its own 1280x720 window without grabbing the mouse, or reuses PIE if it's already running. It also turns off editor background throttling while a run is going. |
| `Tools/BounceBot/mcp/server.mjs` | MCP server (`bouncebot` in `.mcp.json`) with tools `bouncebot_run`, `bouncebot_status`, `bouncebot_stop` and `bouncebot_save_route`. |
| `Tools/BounceBot/routes/*.json` | Saved routes. Run one with `bouncebot_run {route: "smoke"}`. |

## Speed

By default every frame advances exactly 1/60 s of game time (`fixedFps`). Runs are deterministic, and they finish faster than realtime whenever the editor renders above 60 fps. Every report gives game time vs real time. Set `"fixedFps": 0` to use the wall clock.

PIE stays open between runs. Each run teleports the ball back to the PlayerStart unless `"start": "here"`. Pass `"fresh": true` to restart PIE.

## Other ways to run

- In the PIE / game console: `BounceBot.Run smoke` (a route name or a script path), `BounceBot.Stop`
- Standalone (CI-style, quits when done):
  `UnrealEditor.exe Bounce.uproject /Game/Bounce/Maps/Lvl_BounceTest -game -BounceBot=Tools/BounceBot/routes/smoke.json -BounceBotOut=<dir>`

## Script

```jsonc
{
  "name": "jump_slam",
  "description": "shown by bouncebot_status",
  "map": "Lvl_BounceTest",        // optional, short name or /Game path
  "start": "playerStart",         // | "here" | {"to":[x,y,z],"yaw":90}
  "fixedFps": 60, "timeScale": 1, "timeout": 120, "shotWidth": 960, "faceMove": true,
  "fresh": false, "endPie": false,
  "steps": [ "wait 1", {"do": "jump", "hold": 1.2}, "waitUntil apex", "slam", {"do": "burst", "offset": [0,-700,150]} ]
}
```

| Step | Fields |
|---|---|
| `wait` | `seconds` |
| `move` | `stick:[x,y]` (camera-relative, y = forward), or `heading:deg` / `dir:[x,y]` (world). Also `seconds`, `jump` (hold jump throughout), `strength`, `keep`, and `until` (any `waitUntil` condition plus `type`/`value`) to stop early. With `until`, `seconds` is the maximum (default 10) |
| `moveTo` / `path` | `to:[x,y,z]`, `points:[[..],..]` or `actor:"name"`. Also `tolerance` 100, `timeout` 20, `jump`, `brake` true, `until` (stops early, not a failure), `relative` (points offset from the run start), `required` true. Reports STUCK when it makes no progress for `stuckTime` (4 s) |
| `jump` | `hold` 0.15 |
| `slam` | `atApex` |
| `stop` | releases all inputs |
| `waitUntil` | `until`: `bounce`, `land`, `apex`, `rest`, `dead`, `alive`, `zAbove` or `zBelow`. Also `type` (Hop/Jump/Slam/Wall/Settle), `value`, `timeout` 5 |
| `look` | `yaw`, `pitch`, `turn`, `lookAt:[x,y,z]` |
| `teleport` | `to:[x,y,z]` or `"start"`, `yaw` |
| `shot` | `label`, `settleFrames`. For a fixed camera: `from:[x,y,z]` or `offset:[x,y,z]` (world offset from the ball), plus `lookAt` (default the ball) and `fov` |
| `burst` | `count` 6, `interval` 0.1, `label`, camera fields as for `shot`, `follow`. Saves one contact-sheet image (frames run left→right, top→bottom) |
| `set` / `get` | `path`: `BallMovement.JumpHeight`, `Pawn.MaxLeanAngle` or a bare property name. `set` takes `value` (number, bool, string or `[x,y,z]`) and restores the old value after the run unless `keep` |
| `check` | `what`: `x y z vx vy vz speed hspeed maxZ minZ distance bounces deaths dead time yaw lastBounceIn lastBounceOut`. Also `gt`, `lt`, `eq`+`tol`, `label` |
| `cmd` / `log` / `timeScale` | `command` / `text` / `value` |
| `repeat` | `times`, `steps:[...]` |

String shorthands: `"wait 1.5"`, `"jump 0.4"`, `"slam"`, `"slam apex"`, `"shot label"`, `"burst label"`, `"waitUntil apex"`, `"look 90"`, `"cmd stat fps"`, `"stop"`.

A run counts as failed when any `check` fails, or any `moveTo`/`waitUntil` with `required` set fails. Failing steps are marked `FAIL` in the step list.
