"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const policy = require("./quadruped_studio_assets/viewer-orientation.js");

const measured = { roll_deg: 4.5, pitch_deg: -2.25, yaw_deg: -37 };
const locked = policy.toViewerOrientation(measured, { fresh: true });
assert.deepEqual(locked, {
  roll_deg: 4.5,
  pitch_deg: -2.25,
  yaw_deg: 0,
  valid: true,
  fresh: true,
});
assert.equal(measured.yaw_deg, -37, "viewer policy must not mutate the numeric IMU readout source");

const followed = policy.toViewerOrientation(measured, { followYaw: true, fresh: false });
assert.deepEqual(followed, {
  roll_deg: 4.5,
  pitch_deg: -2.25,
  yaw_deg: -37,
  valid: true,
  fresh: false,
});

assert.deepEqual(
  policy.toViewerOrientation({ roll: 1, pitch: 2 }),
  { roll_deg: 1, pitch_deg: 2, yaw_deg: 0, valid: true, fresh: true },
  "locked heading must still allow reliable Roll/Pitch without a Yaw sample",
);
assert.deepEqual(
  policy.toViewerOrientation({ roll: 1, pitch: 2 }, { followYaw: true }),
  { valid: false },
  "opting into heading requires a valid Yaw sample",
);
assert.deepEqual(policy.toViewerOrientation(null), { valid: false });

const assets = path.join(__dirname, "quadruped_studio_assets");
const html = fs.readFileSync(path.join(assets, "index.html"), "utf8");
const studio = fs.readFileSync(path.join(assets, "studio.js"), "utf8");
assert.ok(
  html.indexOf("/assets/viewer-orientation.js") < html.indexOf("/assets/studio.js"),
  "viewer policy must load before studio.js",
);
assert.match(html, /id="imuYawFollowToggle"[^>]*type="checkbox"/);
assert.doesNotMatch(
  html.match(/<input id="imuYawFollowToggle"[^>]*>/)?.[0] || "",
  /\bchecked\b/,
  "3D heading follow must be opt-in",
);
assert.match(studio, /viewerYawFollowEnabled:\s*false/);
assert.match(studio, /followYaw:\s*studio\.viewerYawFollowEnabled/);

console.log("quadruped 3D heading policy tests passed");
