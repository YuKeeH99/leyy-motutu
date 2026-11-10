(function publishViewerOrientation(root, factory) {
  "use strict";

  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.ArcsViewerOrientation = api;
})(typeof globalThis === "object" ? globalThis : this, () => {
  "use strict";

  function finite(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }

  function readAngle(source, longName, shortName) {
    return finite(source?.[`${longName}_deg`] ?? source?.[longName] ?? source?.[shortName]);
  }

  /**
   * Build the orientation sent to the 3D viewer without changing the IMU
   * telemetry used by readouts, session zeroing, or tilt safety.
   *
   * MPU6050 has no magnetic heading reference, so its integrated yaw drifts
   * while the robot is stationary. Heading therefore stays locked unless the
   * operator explicitly opts in for the current browser session.
   */
  function toViewerOrientation(attitude, options = {}) {
    if (!attitude || typeof attitude !== "object") return { valid: false };
    const roll = readAngle(attitude, "roll", "r");
    const pitch = readAngle(attitude, "pitch", "p");
    const yaw = readAngle(attitude, "yaw", "y");
    const followYaw = options.followYaw === true;
    if (roll === null || pitch === null || (followYaw && yaw === null)) return { valid: false };
    return {
      roll_deg: roll,
      pitch_deg: pitch,
      yaw_deg: followYaw ? yaw : 0,
      valid: options.valid !== false,
      fresh: options.fresh !== false,
    };
  }

  return Object.freeze({ toViewerOrientation });
});
