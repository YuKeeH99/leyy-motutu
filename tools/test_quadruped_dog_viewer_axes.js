"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const viewer = fs.readFileSync(
  path.join(__dirname, "quadruped_studio_assets", "dog-viewer.js"),
  "utf8",
);

assert.match(
  viewer,
  /bodyPitchQuaternion\.setFromAxisAngle\(bodyLateralAxis, THREE\.MathUtils\.degToRad\(state\.bodyOrientation\.pitchDeg\)\)/,
  "positive body Pitch must use the model's positive lateral-axis rotation (nose-up mapping)",
);
assert.match(
  viewer,
  /bodyRollQuaternion\.setFromAxisAngle\(bodyForwardAxis, THREE\.MathUtils\.degToRad\(-state\.bodyOrientation\.rollDeg\)\)/,
  "positive body Roll must use the model's negative forward-axis rotation (validated side-tilt mapping)",
);
assert.match(
  viewer,
  /bodyYawQuaternion\.setFromAxisAngle\(bodyUpAxis, THREE\.MathUtils\.degToRad\(-state\.bodyOrientation\.yawDeg\)\)/,
  "the previously validated Yaw visual sign must remain unchanged",
);
assert.doesNotMatch(
  viewer,
  /degToRad\(-state\.bodyOrientation\.pitchDeg\)/,
  "the old inverted Pitch mapping must not return",
);
assert.doesNotMatch(
  viewer,
  /degToRad\(state\.bodyOrientation\.rollDeg\)/,
  "the old inverted Roll mapping must not return",
);
assert.match(
  viewer,
  /targetBodyQuaternion\.copy\(bodyYawQuaternion\)\s*\.multiply\(bodyPitchQuaternion\)\s*\.multiply\(bodyRollQuaternion\)/,
  "Yaw/Pitch/Roll composition order must remain stable",
);
assert.match(
  viewer,
  /const MODEL_HEADING_CORRECTION_RAD = Math\.PI;/,
  "the bundled model needs one fixed 180-degree heading correction",
);
assert.match(
  viewer,
  /loadedModel\.rotation\.y = MODEL_HEADING_CORRECTION_RAD;\s*loadedModel\.updateMatrixWorld\(true\);/,
  "the fixed front/back correction must be applied to the imported model, not live IMU orientation",
);

console.log("quadruped dog viewer axis tests passed");
