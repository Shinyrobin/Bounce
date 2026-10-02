#!/usr/bin/env node
// BounceBot MCP server: run a whole scripted playtest of Bounce in ONE tool call and get back a compact digest
// plus the screenshots inline. Talks to the BounceEditor HTTP bridge (PIE in the open editor) or, when no editor
// is running, launches the game standalone with -BounceBot=<script>. No dependencies.
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import http from "node:http";
import { dirname, join, relative, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
const UPROJECT = join(ROOT, "Bounce.uproject");
const ROUTES = join(ROOT, "Tools/BounceBot/routes");
const PORT = Number(process.env.BOUNCEBOT_PORT ?? 8765);

// ---------- helpers ----------

function request(method, path, body, timeoutMs) {
  return new Promise((resolvePromise, reject) => {
    const data = body === undefined ? undefined : Buffer.from(typeof body === "string" ? body : JSON.stringify(body));
    const req = http.request(
      { host: "127.0.0.1", port: PORT, path, method, headers: data ? { "Content-Type": "application/json", "Content-Length": data.length } : {} },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString("utf8");
          try {
            resolvePromise(JSON.parse(text));
          } catch {
            reject(new Error(`bridge returned non-JSON (${res.statusCode}): ${text.slice(0, 300)}`));
          }
        });
      },
    );
    req.on("error", reject);
    if (timeoutMs) req.setTimeout(timeoutMs, () => req.destroy(new Error(`no answer from the editor after ${Math.round(timeoutMs / 1000)}s`)));
    if (data) req.write(data);
    req.end();
  });
}

const bridgeUp = () => request("GET", "/status", undefined, 3000).catch(() => null);

function listFiles(dir, ext, out = []) {
  if (!existsSync(dir)) return out;
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) listFiles(p, ext, out);
    else if (p.endsWith(ext)) out.push(p);
  }
  return out;
}

/** "Lvl_BounceTest" -> "/Game/Bounce/Maps/Lvl_BounceTest" */
function resolveMap(map) {
  if (!map || map.startsWith("/")) return map;
  const hit = listFiles(join(ROOT, "Content"), ".umap").find((p) => p.replace(/\\/g, "/").endsWith(`/${map}.umap`));
  if (!hit) throw new Error(`map '${map}' not found under Content/`);
  return "/Game/" + relative(join(ROOT, "Content"), hit).replace(/\\/g, "/").replace(/\.umap$/, "");
}

function loadRoute(name) {
  const file = join(ROUTES, name.endsWith(".json") ? name : `${name}.json`);
  if (!existsSync(file)) throw new Error(`route '${name}' not found in Tools/BounceBot/routes (have: ${routeNames().join(", ") || "none"})`);
  return JSON.parse(readFileSync(file, "utf8"));
}

const routeNames = () => (existsSync(ROUTES) ? readdirSync(ROUTES).filter((f) => f.endsWith(".json")).map((f) => f.slice(0, -5)) : []);

function editorExe() {
  if (process.env.UE_EDITOR) return process.env.UE_EDITOR;
  const assoc = JSON.parse(readFileSync(UPROJECT, "utf8")).EngineAssociation;
  return `C:/Program Files/Epic Games/UE_${assoc}/Engine/Binaries/Win64/UnrealEditor.exe`;
}

function runStandalone(script, timeoutS) {
  const stamp = new Date().toISOString().replace(/[-:T]/g, "").slice(0, 15);
  const base = join(ROOT, "Saved/BounceBot");
  const outDir = join(base, "Runs", `${stamp}-${(script.name ?? "run").replace(/[^\w-]/g, "_")}`);
  mkdirSync(outDir, { recursive: true });
  const scriptFile = join(outDir, "script.json");
  writeFileSync(scriptFile, JSON.stringify(script, null, 2));
  const args = [UPROJECT];
  if (script.map) args.push(script.map);
  args.push("-game", `-BounceBot=${scriptFile}`, `-BounceBotOut=${outDir}`, "-windowed", "-ResX=1280", "-ResY=720", "-nosplash", "-nosound");
  return new Promise((resolvePromise, reject) => {
    const proc = spawn(editorExe(), args, { windowsHide: false, stdio: "ignore" });
    const timer = setTimeout(() => {
      proc.kill();
      reject(new Error(`standalone run timed out after ${timeoutS}s`));
    }, timeoutS * 1000);
    proc.on("error", (e) => {
      clearTimeout(timer);
      reject(e);
    });
    proc.on("exit", () => {
      clearTimeout(timer);
      const reportFile = join(outDir, "report.json");
      if (!existsSync(reportFile)) return reject(new Error(`game exited without a report (see ${join(ROOT, "Saved/Logs/Bounce.log")})`));
      resolvePromise(JSON.parse(readFileSync(reportFile, "utf8")));
    });
  });
}

// ---------- digest ----------

function digest(r, { verbose }) {
  const out = [];
  const f2 = (n) => (typeof n === "number" ? Math.round(n * 100) / 100 : n);
  for (const k of ["gameTime", "realTime"]) r[k] = f2(r[k]);
  for (const s of r.shots ?? []) s.t = f2(s.t);
  if (r.ok === false && !r.steps) return `BounceBot FAILED: ${r.error}`;
  const speed = r.realTime > 0 ? ` (${(r.gameTime / r.realTime).toFixed(1)}x realtime)` : "";
  const clock = r.fixedFps ? `@${r.fixedFps}fps fixed` : "realtime clock";
  out.push(`BounceBot "${r.name}" ${r.ok ? "OK" : "FAILED"}: ${r.gameTime}s game in ${r.realTime}s real${speed}, ${r.frames} frames ${clock}, map ${r.map}`);
  if (r.error) out.push(`Error: ${r.error}`);
  out.push(`Steps ${r.stepsRun}/${r.stepsTotal}:`);
  for (const s of r.steps ?? []) out.push(" " + s);
  const b = r.bounces ?? {};
  const types = Object.entries(b.byType ?? {}).map(([k, v]) => `${k} ${v}`).join(", ");
  out.push(`Bounces ${b.count ?? 0}${types ? ` (${types})` : ""}, max in ${b.maxIn} / out ${b.maxOut} cm/s · maxZ ${r.maxZ} · minZ ${r.minZ} · dist ${r.distance}cm`);
  if (verbose) for (const l of b.list ?? []) out.push("   " + l);
  out.push(`End ${r.end}${r.deaths ? ` · ball DIED ${r.deaths}x` : ""}${r.pawnChanges ? ` · pawn replaced ${r.pawnChanges}x` : ""}`);
  if (r.checksPassed || r.checksFailed) out.push(`Checks: ${r.checksPassed} passed, ${r.checksFailed} failed`);
  const values = Object.entries(r.values ?? {});
  if (values.length) out.push("Values: " + values.map(([k, v]) => `${k}=${v}`).join(" · "));
  const log = r.log ?? {};
  const nErr = log.errorsDistinct ?? 0, nWarn = log.warningsDistinct ?? 0;
  out.push(`Log during run: ${nErr} distinct errors, ${nWarn} distinct warnings`);
  for (const l of log.errors ?? []) out.push("  E " + l);
  for (const l of (log.warnings ?? []).slice(0, verbose ? 12 : 4)) out.push("  W " + l);
  if (r.shots?.length) out.push("Shots: " + r.shots.map((s, i) => `${String(i + 1).padStart(2, "0")} '${s.label}'${s.frames > 1 ? ` burst x${s.frames}` : ""} t=${s.t}`).join(" · "));
  out.push(`Run dir: ${r.dir} (report.json, trace.csv)`);
  return out.join("\n");
}

// ---------- tools ----------

async function run(args) {
  let script = args.route ? loadRoute(args.route) : args.script;
  if (typeof script === "string") script = JSON.parse(script);
  if (!script?.steps) throw new Error("pass `route` (saved route name) or `script` (object with a `steps` array)");
  script = { ...script, ...(args.overrides ?? {}) };
  if (script.map) script.map = resolveMap(script.map);

  const mode = args.mode ?? "auto";
  const status = mode === "standalone" ? null : await bridgeUp();
  let report;
  if (status) {
    const timeoutS = (script.realTimeout ?? 300) + 150;
    report = await request("POST", "/run", script, timeoutS * 1000);
  } else if (mode === "editor") {
    throw new Error(`BounceBot bridge not reachable on 127.0.0.1:${PORT}. Is the editor open with the BounceEditor module built?`);
  } else {
    report = await runStandalone(script, (script.realTimeout ?? 300) + 180);
  }

  const content = [{ type: "text", text: digest(report, { verbose: !!args.verbose }) }];
  const shots = report.shots ?? [];
  const max = args.images === "none" ? 0 : (args.max_images ?? 8);
  for (const [i, s] of shots.slice(0, max).entries()) {
    if (!existsSync(s.file)) continue;
    content.push({ type: "text", text: `[${String(i + 1).padStart(2, "0")} '${s.label}' t=${s.t}${s.frames > 1 ? `, ${s.frames} frames left-to-right, top-to-bottom` : ""}]` });
    content.push({ type: "image", data: readFileSync(s.file).toString("base64"), mimeType: "image/jpeg" });
  }
  if (shots.length > max && max > 0) content.push({ type: "text", text: `${shots.length - max} more shots not inlined (see run dir).` });
  return { content };
}

async function status() {
  const s = await bridgeUp();
  const routes = routeNames().map((n) => {
    try {
      const r = loadRoute(n);
      return `  ${n}: ${r.description ?? r.name ?? ""} (${r.steps?.length ?? 0} steps)`;
    } catch {
      return `  ${n}: (unreadable)`;
    }
  });
  return [
    s ? `Editor bridge up on :${PORT} · PIE ${s.pie ? `running (${s.map})` : "not running"} · ${s.busy ? "BUSY" : "idle"}` : `Editor bridge not reachable on :${PORT} (runs will launch the game standalone)`,
    `Routes (${routes.length}):`,
    ...routes,
  ].join("\n");
}

async function stop(args) {
  await request("POST", args.end_pie ? "/stop?pie=1" : "/stop", "{}", 5000);
  return "stop requested";
}

function saveRoute(args) {
  if (!/^[\w-]+$/.test(args.name ?? "")) throw new Error("name must be letters, digits, - or _");
  const script = typeof args.script === "string" ? JSON.parse(args.script) : args.script;
  if (!script?.steps) throw new Error("script needs a `steps` array");
  mkdirSync(ROUTES, { recursive: true });
  const file = join(ROUTES, `${args.name}.json`);
  writeFileSync(file, JSON.stringify(script, null, 2) + "\n");
  return `saved ${relative(ROOT, file)}`;
}

const STEP_REF = `Script: {name, map?, start?: "playerStart"(default)|"here"|{to:[x,y,z],yaw}, fixedFps?:60 (0=realtime), timeScale?, shotWidth?:960, timeout?:120 (game s), faceMove?:true, fresh?:false (restart PIE), endPie?:false, steps:[...]}.
Steps are {"do":..} objects or shorthand strings ("wait 1.5", "jump 0.3", "slam", "slam apex", "shot label", "waitUntil apex", "stop"):
- wait {seconds}
- move {stick:[x,y] (camera-relative, y=forward) | heading:deg | dir:[x,y] (world), seconds, jump?:hold jump, strength?, keep?, until?: any waitUntil condition (+type/value) to stop early, seconds then = max (default 10)}
- moveTo {to:[x,y,z] | actor:"name" , tolerance?:100, timeout?:20, jump?, brake?:true, until?, relative?:false (points are offsets from the run start), required?:true}; path {points:[[x,y,z],...], ...same}
- jump {hold?:0.15}   - slam {atApex?}   - stop (release inputs)
- waitUntil {until: bounce|land|apex|rest|dead|alive|zAbove|zBelow, type?: Hop|Jump|Slam|Wall|Settle, value?, timeout?:5}
- look {yaw?, pitch?, turn?, lookAt?:[x,y,z]}   - teleport {to:[x,y,z]|"start", yaw?}
- shot {label?, from?:[x,y,z] | offset?:[x,y,z] (world offset from ball), lookAt?, fov?, settleFrames?}
- burst {count?:6, interval?:0.1, label?, from?/offset?, follow?} -> one contact-sheet image
- set {path:"BallMovement.JumpHeight"|"Pawn.X"|"X", value, keep?} (restored after the run unless keep)  - get {path}
- check {what: x|y|z|vx|vy|vz|speed|hspeed|maxZ|minZ|distance|bounces|deaths|dead|time|yaw|lastBounceIn|lastBounceOut, gt?, lt?, eq?, tol?, label?}
- cmd {command}   - log {text}   - timeScale {value}   - repeat {times, steps:[...]}`;

// ---------- MCP stdio transport ----------

const TOOLS = [
  {
    name: "bouncebot_run",
    description:
      "Run a scripted playtest of the Bounce game (the BounceBot) in ONE call: drives the player ball through moves/jumps/slams/routes " +
      "at a fixed timestep (usually faster than realtime), takes screenshots, and returns a compact digest (per-step results with " +
      "positions, bounce stats, checks, log errors) plus the screenshots inline. Uses PIE in the open editor (starting it if needed, " +
      "reusing it otherwise) or launches the game standalone if no editor is up. Prefer this over many small editor MCP calls.\n" + STEP_REF,
    inputSchema: {
      type: "object",
      properties: {
        route: { type: "string", description: "Name of a saved route in Tools/BounceBot/routes (see bouncebot_status)." },
        script: { type: "object", description: "Inline script (see description). Use instead of route." },
        overrides: { type: "object", description: "Top-level script fields to override, e.g. {\"fresh\":true} or {\"shotWidth\":640}." },
        mode: { type: "string", enum: ["auto", "editor", "standalone"], description: "Default auto: editor bridge if reachable, else standalone." },
        images: { type: "string", enum: ["inline", "none"], description: "Default inline." },
        max_images: { type: "number", description: "Max screenshots to inline. Default 8." },
        verbose: { type: "boolean", description: "Include every bounce and more log warnings." },
      },
    },
    handler: run,
  },
  {
    name: "bouncebot_status",
    description: "Is the BounceBot editor bridge up / PIE running, and which saved routes exist (with descriptions).",
    inputSchema: { type: "object", properties: {} },
    handler: status,
  },
  {
    name: "bouncebot_stop",
    description: "Stop the current BounceBot run (its report is still returned to the waiting run call). end_pie also ends PIE.",
    inputSchema: { type: "object", properties: { end_pie: { type: "boolean" } } },
    handler: stop,
  },
  {
    name: "bouncebot_save_route",
    description: "Save a script as a reusable route (Tools/BounceBot/routes/<name>.json) so it can be re-run with bouncebot_run {route}. Add a `description` field to the script.",
    inputSchema: { type: "object", properties: { name: { type: "string" }, script: { type: "object" } }, required: ["name", "script"] },
    handler: saveRoute,
  },
];

const send = (msg) => process.stdout.write(JSON.stringify(msg) + "\n");
const byName = new Map(TOOLS.map((t) => [t.name, t]));

async function handle({ id, method, params }) {
  const isRequest = id !== undefined && id !== null;
  try {
    let result;
    if (method === "initialize") {
      result = { protocolVersion: params?.protocolVersion ?? "2025-06-18", capabilities: { tools: {} }, serverInfo: { name: "bouncebot", version: "1.0.0" } };
    } else if (method === "ping") {
      result = {};
    } else if (method === "tools/list") {
      result = { tools: TOOLS.map(({ handler, ...t }) => t) };
    } else if (method === "tools/call") {
      const tool = byName.get(params?.name);
      if (!tool) throw Object.assign(new Error(`Unknown tool: ${params?.name}`), { code: -32602 });
      try {
        const out = await tool.handler(params.arguments ?? {});
        result = Array.isArray(out?.content) ? { content: out.content } : { content: [{ type: "text", text: typeof out === "string" ? out : JSON.stringify(out, null, 2) }] };
      } catch (e) {
        result = { content: [{ type: "text", text: String(e?.message ?? e) }], isError: true };
      }
    } else {
      if (!isRequest) return;
      throw Object.assign(new Error(`Method not found: ${method}`), { code: -32601 });
    }
    if (isRequest) send({ jsonrpc: "2.0", id, result });
  } catch (e) {
    if (isRequest) send({ jsonrpc: "2.0", id, error: { code: e.code ?? -32603, message: String(e.message ?? e) } });
  }
}

const rl = createInterface({ input: process.stdin });
rl.on("line", (line) => {
  line = line.replace(/^\uFEFF/, "");
  if (!line.trim()) return;
  let msg;
  try {
    msg = JSON.parse(line);
  } catch {
    send({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "Parse error" } });
    return;
  }
  for (const m of Array.isArray(msg) ? msg : [msg]) handle(m);
});
rl.on("close", () => process.exit(0));
