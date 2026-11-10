(function publishImuMounting(root, factory) {
  "use strict";

  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.ArcsImuMounting = api;
})(typeof globalThis === "object" ? globalThis : this, () => {
  "use strict";

  const DEFAULT_BOARD = "arcs_mini_v2_1";
  const IDENTITY_PROFILE = Object.freeze({
    id: "identity-sensor-to-body",
    board: "unknown",
    revision: 1,
    source_frame: "imu_sensor_axes",
    target_frame: "arcs_body_axes",
    rotation_deg: Object.freeze({ roll: 0, pitch: 0, yaw: 0 }),
    vector_sign: Object.freeze([1, 1, 1]),
  });
  const BOARD_IMU_MOUNTING_PROFILES = Object.freeze({
    arcs_mini_v2_1: Object.freeze({
      id: "arcs-mini-v2-1-mpu6050-rx-180",
      board: "arcs_mini_v2_1",
      revision: 1,
      source_frame: "mpu6050_sensor_axes",
      target_frame: "arcs_body_axes",
      // The V2.1 PCB is mechanically upright while its MPU6050 +Z points
      // down. Sensor -> body is therefore a fixed 180 degree rotation about X.
      rotation_deg: Object.freeze({ roll: 180, pitch: 0, yaw: 0 }),
      vector_sign: Object.freeze([1, -1, -1]),
    }),
  });

  function finite(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }

  function wrapDegrees(value) {
    let wrapped = Number(value) % 360;
    if (!Number.isFinite(wrapped)) return null;
    if (wrapped > 180) wrapped -= 360;
    if (wrapped < -180) wrapped += 360;
    return Object.is(wrapped, -0) ? 0 : wrapped;
  }

  function readAngle(source, longName, shortName) {
    return finite(source?.[`${longName}_deg`] ?? source?.[longName] ?? source?.[shortName]);
  }

  function profileForBoard(board) {
    const key = typeof board === "string" ? board.trim() : "";
    return BOARD_IMU_MOUNTING_PROFILES[key] || IDENTITY_PROFILE;
  }

  function sensorVectorToBody(vector, board = DEFAULT_BOARD) {
    if (!Array.isArray(vector) || vector.length < 3) return null;
    const values = vector.slice(0, 3).map(finite);
    if (values.some((value) => value === null)) return null;
    const signs = profileForBoard(board).vector_sign;
    return values.map((value, index) => value * signs[index]);
  }

  function sensorAttitudeToBody(attitude, board = DEFAULT_BOARD) {
    if (!attitude || typeof attitude !== "object") return null;
    const roll = readAngle(attitude, "roll", "r");
    const pitch = readAngle(attitude, "pitch", "p");
    const yaw = readAngle(attitude, "yaw", "y");
    if ([roll, pitch, yaw].some((value) => value === null)) return null;
    const rotation = profileForBoard(board).rotation_deg;
    return {
      roll_deg: wrapDegrees(roll + rotation.roll),
      pitch_deg: wrapDegrees(pitch + rotation.pitch),
      yaw_deg: wrapDegrees(yaw + rotation.yaw),
    };
  }

  function displayedBodyAttitude(
    sensorAttitude,
    sensorLevelBaseline = null,
    sensorYawZeroDeg = 0,
    board = DEFAULT_BOARD,
  ) {
    const body = sensorAttitudeToBody(sensorAttitude, board);
    if (!body) return null;
    let roll = body.roll_deg;
    let pitch = body.pitch_deg;
    if (sensorLevelBaseline && typeof sensorLevelBaseline === "object") {
      const baseline = sensorAttitudeToBody({
        roll_deg: sensorLevelBaseline.roll_deg ?? sensorLevelBaseline.roll,
        pitch_deg: sensorLevelBaseline.pitch_deg ?? sensorLevelBaseline.pitch,
        yaw_deg: 0,
      }, board);
      if (baseline) {
        roll = wrapDegrees(roll - baseline.roll_deg);
        pitch = wrapDegrees(pitch - baseline.pitch_deg);
      }
    }
    const yawZero = finite(sensorYawZeroDeg) ?? 0;
    return {
      roll_deg: wrapDegrees(roll),
      pitch_deg: wrapDegrees(pitch),
      yaw_deg: wrapDegrees(body.yaw_deg - yawZero),
    };
  }

  return Object.freeze({
    DEFAULT_BOARD,
    BOARD_IMU_MOUNTING_PROFILES,
    profileForBoard,
    sensorVectorToBody,
    sensorAttitudeToBody,
    displayedBodyAttitude,
    wrapDegrees,
  });
});
