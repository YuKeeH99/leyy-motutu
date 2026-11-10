"use strict";

const assert = require("node:assert/strict");
const mounting = require("./quadruped_studio_assets/imu-mounting.js");

function close(actual, expected, message) {
  assert.ok(Math.abs(actual - expected) < 1e-9, message || `${actual} != ${expected}`);
}

const profile = mounting.profileForBoard("arcs_mini_v2_1");
assert.equal(profile.id, "arcs-mini-v2-1-mpu6050-rx-180");
assert.deepEqual(profile.rotation_deg, { roll: 180, pitch: 0, yaw: 0 });
assert.deepEqual(profile.vector_sign, [1, -1, -1]);

const rawVector = [0.1, -0.2, -1.12];
assert.deepEqual(mounting.sensorVectorToBody(rawVector), [0.1, 0.2, 1.12]);
assert.deepEqual(rawVector, [0.1, -0.2, -1.12], "raw sensor telemetry must not be mutated");

close(mounting.sensorAttitudeToBody({ roll_deg: -180, pitch_deg: 0, yaw_deg: 0 }).roll_deg, 0);
close(mounting.sensorAttitudeToBody({ roll_deg: -179, pitch_deg: 2, yaw_deg: 3 }).roll_deg, 1);
close(mounting.sensorAttitudeToBody({ roll_deg: 179, pitch_deg: 2, yaw_deg: 3 }).roll_deg, -1);

const withoutBaseline = mounting.displayedBodyAttitude(
  { roll_deg: -179, pitch_deg: 2, yaw_deg: 7 },
  null,
  0,
);
assert.deepEqual(withoutBaseline, { roll_deg: 1, pitch_deg: 2, yaw_deg: 7 });

const withLegacySensorBaseline = mounting.displayedBodyAttitude(
  { roll_deg: -178.5, pitch_deg: 2.25, yaw_deg: 7 },
  { roll: -179, pitch: 2 },
  5,
);
assert.deepEqual(withLegacySensorBaseline, { roll_deg: 0.5, pitch_deg: 0.25, yaw_deg: 2 });

const acrossWrapBoundary = mounting.displayedBodyAttitude(
  { roll_deg: 179, pitch_deg: 0, yaw_deg: 0 },
  { roll: -179, pitch: 0 },
  0,
);
close(acrossWrapBoundary.roll_deg, -2, "baseline subtraction must remain continuous across +/-180");

const unknownBoard = mounting.displayedBodyAttitude(
  { roll_deg: 12, pitch_deg: -4, yaw_deg: 8 },
  null,
  0,
  "unknown_board",
);
assert.deepEqual(unknownBoard, { roll_deg: 12, pitch_deg: -4, yaw_deg: 8 });

console.log("quadruped IMU mounting tests passed");
