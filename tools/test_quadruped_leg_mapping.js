"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const assets = path.join(__dirname, "quadruped_studio_assets");
const studio = fs.readFileSync(path.join(assets, "studio.js"), "utf8");
const viewer = fs.readFileSync(path.join(assets, "dog-viewer.js"), "utf8");
const html = fs.readFileSync(path.join(assets, "index.html"), "utf8");
const css = fs.readFileSync(path.join(assets, "studio.css"), "utf8");

const expectedLegs = [
  { code: "FL", label: "前左", pad: "PA04" },
  { code: "FR", label: "前右", pad: "PA05" },
  { code: "RR", label: "后右", pad: "PA06" },
  { code: "RL", label: "后左", pad: "PA07" },
];
const expectedViewerLegs = [
  { code: "FL", bone: "ThighFLBone", direction: 1 },
  { code: "FR", bone: "ThighFRBone", direction: -1 },
  { code: "RR", bone: "ThighBRBone", direction: -1 },
  { code: "RL", bone: "ThighBLBone", direction: 1 },
];

function sourceBlock(source, startMarker, endMarker) {
  const start = source.indexOf(startMarker);
  assert.notEqual(start, -1, `missing source marker: ${startMarker}`);
  const end = source.indexOf(endMarker, start);
  assert.notEqual(end, -1, `missing source marker: ${endMarker}`);
  return source.slice(start, end);
}

const legsBlock = sourceBlock(studio, "const LEGS = Object.freeze([", "const ACTION_TEMPLATES");
const studioLegs = [...legsBlock.matchAll(
  /\{ code: "([A-Z]{2})", label: "([^"]+)", pad: "(PA\d{2})" \}/g,
)].map((match) => ({ code: match[1], label: match[2], pad: match[3] }));
assert.deepEqual(studioLegs, expectedLegs, "studio channel labels must follow the verified wire order");

const viewerBlock = sourceBlock(viewer, "const LEG_CHANNELS = Object.freeze([", "const ACTIVE_PLAYBACK_STATES");
const viewerLegs = [...viewerBlock.matchAll(
  /code: "([A-Z]{2})", bone: "([^"]+)", direction: (-?\d+)/g,
)].map((match) => ({ code: match[1], bone: match[2], direction: Number(match[3]) }));
assert.deepEqual(viewerLegs, expectedViewerLegs, "each wire channel must drive its matching 3D leg bone");

const gaitFunction = sourceBlock(studio, "function gaitPhaseOffsets(pattern)", "function gaitWave(");
const gaitContext = {};
vm.runInNewContext(`${gaitFunction}\nthis.results = {
  trot: gaitPhaseOffsets("trot"),
  pace: gaitPhaseOffsets("pace"),
  crawl: gaitPhaseOffsets("crawl"),
};`, gaitContext);
assert.deepEqual(
  Array.from(gaitContext.results.trot),
  [0, .5, 0, .5],
  "trot diagonals must be FL+RR and FR+RL in channel order",
);
assert.deepEqual(
  Array.from(gaitContext.results.pace),
  [0, .5, .5, 0],
  "pace pairs must be left FL+RL and right FR+RR in channel order",
);
assert.deepEqual(
  Array.from(gaitContext.results.crawl),
  [0, .5, .75, .25],
  "crawl offsets must account for the generator's cycle + phase convention",
);
const chronologicalCrawlOrder = Array.from(gaitContext.results.crawl)
  .map((phase, channel) => ({ channel, liftAt: (1 - phase) % 1 }))
  .sort((left, right) => left.liftAt - right.liftAt)
  .map(({ channel }) => channel);
assert.deepEqual(
  chronologicalCrawlOrder,
  [0, 2, 1, 3],
  "crawl lift events must occur FL -> RR -> FR -> RL in physical channel order",
);

const gaitWaveFunction = sourceBlock(studio, "function gaitWave(", "function gaitDialogValues(");
assert.match(
  gaitWaveFunction,
  /stanceRatio = \.78/,
  "crawl stance must leave a non-overlapping recovery margin between adjacent legs",
);

const postureConstants = sourceBlock(
  studio,
  "const SAFE_POSTURE_MIN_US = 900;",
  "const CALIBRATION_MIN_SAMPLES",
);
assert.match(
  postureConstants,
  /const SAFE_PRONE_PROFILE_BY_CHIP = Object\.freeze\(\{[\s\S]*?"6b3bc01b1642ccbd": Object\.freeze\([\s\S]*?homePulses: Object\.freeze\(\[2100, 900, 2050, 870\]\)[\s\S]*?pronePulses: Object\.freeze\(\[1750, 1250, 1750, 1250\]\)/,
  "the per-chip prone pose must remain bound to the observed device and current exact Home",
);
assert.match(
  postureConstants,
  /stand: Object\.freeze\(\{ operation: "stand", label: "同步起身" \}\)/,
  "locked stand-up must be exposed as one simultaneous four-leg transition",
);
assert.match(
  postureConstants,
  /prone: Object\.freeze\(\{ operation: "prone", label: "同步趴下" \}\)/,
  "locked lie-down must be exposed as one simultaneous four-leg transition",
);

const deriveProneFunction = sourceBlock(
  studio,
  "function derivePronePulses(",
  "function calibratedPronePose(",
);
const proneContext = {
  LEGS: [{}, {}, {}, {}],
  integer: (value, fallback) => Number.isInteger(Number(value)) ? Number(value) : fallback,
  SAFE_PRONE_PROFILE_BY_CHIP: Object.freeze({
    "6b3bc01b1642ccbd": Object.freeze({
      board: "arcs_mini_v2_1",
      mountingRevision: 2,
      homePulses: Object.freeze([2100, 900, 2050, 870]),
      pronePulses: Object.freeze([1750, 1250, 1750, 1250]),
      servoSignature: "900/1500/2100/1/0|900/1500/2100/0/0|900/1500/2100/1/0|900/1500/2100/0/0",
    }),
  }),
};
vm.runInNewContext(`${deriveProneFunction}\nthis.result = derivePronePulses(
  [2100, 900, 2050, 870],
  [
    {min_us: 900, center_us: 1500, max_us: 2100, reverse: true, trim_deg: 0},
    {min_us: 900, center_us: 1500, max_us: 2100, reverse: false, trim_deg: 0},
    {min_us: 900, center_us: 1500, max_us: 2100, reverse: true, trim_deg: 0},
    {min_us: 900, center_us: 1500, max_us: 2100, reverse: false, trim_deg: 0},
  ],
  {chip_id: "6b3bc01b1642ccbd", board: "arcs_mini_v2_1", mounting_revision: 2},
);`, proneContext);
assert.deepEqual(
  Array.from(proneContext.result),
  [1750, 1250, 1750, 1250],
  "every leg must land on the same calibrated angle, so FR keeps the bounded midpoint too",
);
assert.throws(
  () => vm.runInNewContext(`${deriveProneFunction}\nderivePronePulses(
    [2100, 900, 2050, 870],
    [
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: true, trim_deg: 0},
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: false, trim_deg: 0},
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: true, trim_deg: 0},
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: false, trim_deg: 0},
    ],
    {chip_id: "fedcba9876543210"},
  );`, proneContext),
  /尚未单独标定整机趴姿/,
  "a field correction from one physical dog must never be reused for another chip",
);
assert.throws(
  () => vm.runInNewContext(`${deriveProneFunction}\nderivePronePulses(
    [2000, 1000, 2000, 1000],
    [
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: true, trim_deg: 0},
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: false, trim_deg: 0},
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: true, trim_deg: 0},
      {min_us: 900, center_us: 1500, max_us: 2100, reverse: false, trim_deg: 0},
    ],
    {chip_id: "6b3bc01b1642ccbd", board: "arcs_mini_v2_1", mounting_revision: 2},
  );`, proneContext),
  /Home \/ 舵机标定已变更/,
  "the field correction must expire when the same chip receives a different Home",
);

const safePostureHelpers = sourceBlock(
  studio,
  "function buildSafePostureRequest(",
  "async function runSafePostureRoutine()",
);
const helperContext = {
  SAFE_POSTURE_DURATION_MS: 800,
  LEGS: [{}, {}, {}, {}],
};
vm.runInNewContext(`${safePostureHelpers}
const plan = {
  identity: { connectionEpoch: 7, chipId: "6b3bc01b1642ccbd", board: "arcs_mini_v2_1" },
  pulses: [1750, 900, 1750, 1250],
};
this.request = buildSafePostureRequest(plan);
this.safe = safePostureResultIsSafe({
  accepted: true,
  target_reached: true,
  outputs_safe: true,
  servos_us: [1750, 900, 1750, 1250],
  servo_state: { current_us: [1750, 900, 1750, 1250], enabled_mask: 0, active: false, watchdog_ms: 0 },
}, plan.pulses);
this.unsafeWatchdog = safePostureResultIsSafe({
  accepted: true,
  target_reached: true,
  outputs_safe: true,
  servos_us: [1750, 900, 1750, 1250],
  servo_state: { current_us: [1750, 900, 1750, 1250], enabled_mask: 0, active: false, watchdog_ms: 1000 },
}, plan.pulses);`, helperContext);
assert.deepEqual(
  JSON.parse(JSON.stringify(helperContext.request)),
  {
    connection_epoch: 7,
    chip_id: "6b3bc01b1642ccbd",
    board: "arcs_mini_v2_1",
    servos_us: [1750, 900, 1750, 1250],
    duration_ms: 800,
    safety_confirmed: true,
  },
  "the browser must send one identity-bound four-channel posture request",
);
assert.equal(helperContext.safe, true);
assert.equal(helperContext.unsafeWatchdog, false);

const keyframeStep = sourceBlock(
  studio,
  "function keyframeTestDuration(",
  "function renderKeyframeInspector()",
);
assert.match(keyframeStep, /Math\.max\(1000, authoredMs \/ rate\)/);
assert.match(keyframeStep, /servos_us: frame\.servos_us\.map/);
assert.match(keyframeStep, /frame\.duration_ms === 0 \? "smooth"/);
assert.match(keyframeStep, /requestJson\(API\.pose/);
assert.match(keyframeStep, /window\.setTimeout\(resolve, durationMs \+ 120\)/);
assert.doesNotMatch(keyframeStep, /API\.light/);
assert.doesNotMatch(keyframeStep, /schedulePose\(/);

const safePostureRun = sourceBlock(
  studio,
  "async function runSafePostureRoutine()",
  "function applyCalibratedHome(",
);
assert.match(safePostureRun, /requestJson\(API\.postureTest/);
assert.match(safePostureRun, /buildSafePostureRequest\(fresh\)/);
assert.match(safePostureRun, /safePostureResultIsSafe\(response\.result, fresh\.pulses\)/);
assert.match(safePostureRun, /await requestJson\(API\.estop/);
assert.doesNotMatch(safePostureRun, /API\.servoTest/);
assert.doesNotMatch(safePostureRun, /for \(let step/);
assert.doesNotMatch(safePostureRun, /channel:/);
assert.doesNotMatch(safePostureRun, /pulse_us:/);

const gaitGenerator = sourceBlock(studio, "function generateGaitAction(", "function openActionDialog(");
assert.match(
  gaitGenerator,
  /const leftSide = legIndex === 0 \|\| legIndex === 3;/,
  "turn mixing must identify physical left channels FL/CH0 and RL/CH3",
);
assert.doesNotMatch(
  gaitGenerator,
  /const leftSide = legIndex === 0 \|\| legIndex === 2;/,
  "the old FL/CH0 + PA06/CH2 left-side assumption must not return",
);

assert.match(studio, /const stand = \[1460, 1540, 1540, 1460\];/);
assert.match(studio, /const crouch = \[1560, 1440, 1440, 1560\];/);
assert.match(
  studio,
  /return LEGS\.map\(\(\) => \(\{[\s\S]*?reverse: false,/,
  "field observations must not silently become global reverse defaults",
);

const expectedHtmlContracts = [
  /id="calibrationServo2Row"[\s\S]*?<\/div>\s*<div id="calibrationServo3Row"/,
  /id="calibrationServo2Row"[\s\S]*?>RR<small>PA06<\/small>/,
  /id="calibrationServo3Row"[\s\S]*?>RL<small>PA07<\/small>/,
  /data-focus-leg="2"><b>RR<\/b><small id="diagramAngle2">/,
  /data-focus-leg="3"><b>RL<\/b><small id="diagramAngle3">/,
  /data-leg-card="2"[\s\S]*?<strong>RR<\/strong><small>后右 · PA06<\/small>/,
  /data-leg-card="3"[\s\S]*?<strong>RL<\/strong><small>后左 · PA07<\/small>/,
  /<b>RR<\/b><input id="cal2Min"/,
  /<b>RL<\/b><input id="cal3Min"/,
  /id="safeStandBtn"[^>]*data-safe-posture="stand">同步起身<\/button>/,
  /id="safeProneBtn"[^>]*data-safe-posture="prone">同步趴下<\/button>/,
  /id="safePostureDialog"[^>]*aria-labelledby="safePostureDialogTitle"/,
  /id="safePostureMode">FL \+ FR \+ RR \+ RL<\/strong>/,
];
expectedHtmlContracts.forEach((contract) => assert.match(html, contract));

const homeLabels = sourceBlock(html, 'id="calibrationHomeAngles"', 'class="calibration-readiness"');
assert.deepEqual(
  [...homeLabels.matchAll(/<small>(FL|FR|RR|RL)<\/small>/g)].map((match) => match[1]),
  ["FL", "FR", "RR", "RL"],
  "Home snapshot labels must follow servo array order",
);

assert.match(css, /\.servo-card\[data-leg-card="2"\]::before \{ background: var\(--rr\); \}/);
assert.match(css, /\.servo-card\[data-leg-card="3"\]::before \{ background: var\(--rl\); \}/);
assert.match(css, /\.servo-card\[data-leg-card="2"\] \.servo-range \{ accent-color: var\(--rr\); \}/);
assert.match(css, /\.servo-card\[data-leg-card="3"\] \.servo-range \{ accent-color: var\(--rl\); \}/);
assert.match(
  css,
  /var\(--fl\) 0 25%, var\(--fr\) 25% 50%, var\(--rr\) 50% 75%, var\(--rl\) 75%/,
  "combined timeline color key must follow physical channel order",
);

console.log("quadruped physical leg mapping tests passed");
