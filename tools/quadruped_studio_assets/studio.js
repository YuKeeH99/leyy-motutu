(() => {
  "use strict";

  const API = Object.freeze({
    state: "/api/state",
    statusCompat: "/api/status",
    events: "/api/events",
    eventsCompat: "/events",
    project: "/api/project",
    actions: "/api/actions",
    calibration: "/api/calibration",
    arm: "/api/arm",
    pose: "/api/pose",
    light: "/api/light",
    ledCalibration: "/api/led-calibration",
    imu: "/api/imu",
    estop: "/api/estop",
    postureTest: "/api/posture-test",
    recordStart: "/api/record/start",
    recordStop: "/api/record/stop",
    playback: "/api/playback",
  });

  const STORAGE_KEY = "arcs.quadruped-motion-studio.project.v1";
  const PROJECT_SCHEMA = "arcs-motion-project";
  const PROJECT_VERSION = 1;
  const SERVO_ELECTRICAL_MIN = 500;
  const SERVO_ELECTRICAL_MAX = 2500;
  const SAFE_MIN = 1000;
  const SAFE_CENTER = 1500;
  const SAFE_MAX = 2000;
  const DEFAULT_PIXELS = 8;
  const MAX_PIXELS = 64;
  const MAX_ACTIONS = 64;
  const MAX_KEYFRAMES = 2000;
  const MAX_PROJECT_DURATION_MS = 30 * 60 * 1000;
  // Live slider input is throttled, never debounced.  A hobby servo frame is
  // 20 ms, so a 40 ms host cadence gives every command two board-side ticks
  // while continuously forwarding the newest target during a drag.
  const POSE_COALESCE_MS = 40;
  const LIVE_DRAG_DURATION_MS = 40;
  const HOME_BUTTON_DURATION_MS = 500;
  const LIGHT_COALESCE_MS = 70;
  const HOLD_TO_ARM_MS = 1200;
  // A dedicated locked-only backend operation owns the complete four-channel
  // transition and verifies all outputs are off before replying.  Do not
  // rebuild this safety boundary as arm -> async pose -> browser timer.
  const SAFE_POSTURE_MIN_US = 900;
  const SAFE_POSTURE_MAX_US = 2100;
  const SAFE_POSTURE_DURATION_MS = 800;
  // Safe posture targets are whole-device fixture data, not a formula.  The
  // current Home includes per-leg trim (FL 2100, FR 900, RR 2050, RL 870),
  // while the independently verified prone pose remains 1750/1250/1750/1250.
  // Binding both arrays to chip id, mounting revision and servo signature keeps
  // a field result from silently leaking to a mechanically different dog.
  const SAFE_PRONE_PROFILE_BY_CHIP = Object.freeze({
    "6b3bc01b1642ccbd": Object.freeze({
      board: "arcs_mini_v2_1",
      mountingRevision: 2,
      homePulses: Object.freeze([2100, 900, 2050, 870]),
      pronePulses: Object.freeze([1750, 1250, 1750, 1250]),
      servoSignature: "900/1500/2100/1/0|900/1500/2100/0/0|900/1500/2100/1/0|900/1500/2100/0/0",
    }),
  });
  const SAFE_POSTURE_ROUTINES = Object.freeze({
    stand: Object.freeze({ operation: "stand", label: "同步起身" }),
    prone: Object.freeze({ operation: "prone", label: "同步趴下" }),
  });
  const CALIBRATION_MIN_SAMPLES = 6;
  const CALIBRATION_MIN_SPAN_MS = 3000;
  const CALIBRATION_TIMEOUT_MS = 8000;
  const CALIBRATION_ACCEL_MIN_G = .8;
  const CALIBRATION_ACCEL_MAX_G = 1.2;
  const CALIBRATION_ACCEL_VARIANCE_MAX = .0004;
  const CALIBRATION_GYRO_MEAN_MAX_DPS = 3;
  const CALIBRATION_GYRO_PEAK_MAX_DPS = 5;
  const CALIBRATION_GYRO_AXIS_VARIANCE_MAX = .25;
  const CALIBRATION_ATTITUDE_VARIANCE_MAX = .25;
  const LED_COLOR_ORDERS = Object.freeze(["GRB", "RGB", "RBG", "GBR", "BRG", "BGR"]);
  const LED_CALIBRATION_BYPASS = Object.freeze({
    color_order: "GRB",
    r_gain_permille: 1000,
    g_gain_permille: 1000,
    b_gain_permille: 1000,
    gamma_x100: 100,
  });
  const LED_CALIBRATION_TYPICAL = Object.freeze({
    color_order: "GRB",
    r_gain_permille: 1000,
    g_gain_permille: 800,
    b_gain_permille: 310,
    gamma_x100: 220,
  });
  const LEGS = Object.freeze([
    { code: "FL", label: "前左", pad: "PA04" },
    { code: "FR", label: "前右", pad: "PA05" },
    { code: "RR", label: "后右", pad: "PA06" },
    { code: "RL", label: "后左", pad: "PA07" },
  ]);
  const ACTION_TEMPLATES = Object.freeze({
    custom: { name: "自定义动作", direction: 0, turn: 0, note: "从空白时间线开始创作。" },
    forward: { name: "前进步态", direction: 1, turn: 0, note: "机构相关模板；必须先校准并低速试跑。" },
    backward: { name: "后退步态", direction: -1, turn: 0, note: "独立后退模板；不能视为已验证的前进反放。" },
    march: { name: "原地踏步", direction: 1, turn: 0, note: "低幅相位模板，用于检查腿序与机身稳定性。" },
    "turn-left": { name: "左转步态", direction: 1, turn: -35, note: "左右幅值差开环转向，实际角度需实机标定。" },
    "turn-right": { name: "右转步态", direction: 1, turn: 35, note: "左右幅值差开环转向，实际角度需实机标定。" },
  });
  const EASING_TO_WIRE = Object.freeze({
    linear: "linear",
    "ease-in-out": "smooth",
    "ease-in": "ease-in",
    "ease-out": "ease-out",
    step: "linear",
    smooth: "smooth",
  });
  const IMU_MOUNTING = globalThis.ArcsImuMounting;
  if (!IMU_MOUNTING || typeof IMU_MOUNTING.displayedBodyAttitude !== "function") {
    throw new Error("IMU mounting profile failed to load");
  }
  const VIEWER_ORIENTATION = globalThis.ArcsViewerOrientation;
  if (!VIEWER_ORIENTATION || typeof VIEWER_ORIENTATION.toViewerOrientation !== "function") {
    throw new Error("3D viewer orientation policy failed to load");
  }

  const $ = (id) => document.getElementById(id);
  const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
  const number = (value, fallback = 0) => {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : fallback;
  };
  const integer = (value, fallback = 0) => Math.round(number(value, fallback));
  const uid = (prefix = "id") => {
    if (globalThis.crypto?.randomUUID) return `${prefix}-${crypto.randomUUID()}`;
    return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 9)}`;
  };
  const deepCopy = (value) => JSON.parse(JSON.stringify(value));
  const safeText = (value, fallback, maxLength) => {
    const text = typeof value === "string" ? value.trim() : "";
    return (text || fallback).slice(0, maxLength);
  };
  const IDENTIFIER_RE = /^[A-Za-z0-9_-]{1,64}$/;
  const normalizeIdentifier = (value, fallback, errors = null, strict = false, label = "ID") => {
    const text = typeof value === "string" ? value.trim() : "";
    if (IDENTIFIER_RE.test(text)) return text;
    if (strict && errors) errors.push(`${label} 只能包含字母、数字、下划线和短横线，长度 1–64`);
    return fallback;
  };
  const isTypingTarget = (target) => {
    if (!(target instanceof Element)) return false;
    return Boolean(target.closest("input, select, textarea, [contenteditable='true']"));
  };

  class ProjectValidationError extends Error {
    constructor(errors) {
      super(errors.slice(0, 4).join("；"));
      this.name = "ProjectValidationError";
      this.errors = errors;
    }
  }

  function normalizeHex(value, fallback = "#35A7FF") {
    const text = String(value || "").trim();
    const short = /^#?([0-9a-f]{3})$/i.exec(text);
    if (short) {
      return `#${short[1].split("").map((item) => item + item).join("")}`.toUpperCase();
    }
    const full = /^#?([0-9a-f]{6})$/i.exec(text);
    return full ? `#${full[1].toUpperCase()}` : fallback;
  }

  function defaultLed(overrides = {}) {
    return {
      enabled: false,
      color: "#35A7FF",
      brightness: 40,
      pixel_count: DEFAULT_PIXELS,
      ...overrides,
    };
  }

  function defaultCalibration() {
    return LEGS.map(() => ({
      min_us: SAFE_MIN,
      center_us: SAFE_CENTER,
      max_us: SAFE_MAX,
      reverse: false,
      trim_deg: 0,
    }));
  }

  function makeFrame(timeMs, servosUs, options = {}) {
    const easing = EASING_TO_WIRE[String(options.easing || "smooth")] || "smooth";
    return {
      id: options.id || uid("frame"),
      time_ms: Math.max(0, integer(timeMs)),
      duration_ms: options.easing === "step" ? 0 : clamp(integer(options.duration_ms, 600), 0, 10000),
      easing,
      servos_us: servosUs.map((value) => integer(value, SAFE_CENTER)),
      led: defaultLed(options.led),
    };
  }

  function createDefaultProject() {
    const centered = [1500, 1500, 1500, 1500];
    // Every servo array is in the physical wire order FL, FR, RR, RL.
    const stand = [1460, 1540, 1540, 1460];
    const crouch = [1560, 1440, 1440, 1560];
    const plus = 67;
    const minus = -67;
    const gaitFrames = [
      [0, [0, 0, 0, 0]],
      [800, [plus, 0, 0, 0]],
      [1600, [0, plus, 0, minus]],
      [2400, [0, 0, plus, 0]],
      [3200, [minus, 0, 0, plus]],
      [4000, [0, minus, 0, 0]],
      [4800, [plus, 0, minus, 0]],
      [5600, [0, plus, 0, minus]],
      [6400, [0, 0, 0, 0]],
    ].map(([time, offsets]) => makeFrame(
      time,
      offsets.map((offset) => SAFE_CENTER + offset),
      { duration_ms: 760, easing: "smooth", led: defaultLed({ enabled: true, brightness: 22 }) },
    ));
    const now = new Date().toISOString();
    return {
      schema: PROJECT_SCHEMA,
      version: PROJECT_VERSION,
      id: uid("project"),
      name: "四足动作工程",
      created_at: now,
      updated_at: now,
      calibration: defaultCalibration(),
      actions: [
        {
          id: "preset-center",
          name: "中位",
          duration_ms: 800,
          loop: false,
          note: "四路回到安全中位。",
          keyframes: [makeFrame(0, centered, { duration_ms: 600 })],
        },
        {
          id: "preset-stand",
          name: "站立（校准后使用）",
          duration_ms: 1000,
          loop: false,
          note: "安全小幅示例；实际站立角度取决于舵机方向与机械连杆。",
          keyframes: [makeFrame(0, stand, { duration_ms: 800 })],
        },
        {
          id: "preset-crouch",
          name: "伏低（校准后使用）",
          duration_ms: 1000,
          loop: false,
          note: "安全小幅示例；实际姿态取决于机械结构。",
          keyframes: [makeFrame(0, crouch, { duration_ms: 800 })],
        },
        {
          id: "preset-slow-gait",
          name: "慢速步态（校准后使用）",
          duration_ms: 6400,
          loop: true,
          note: "四路围绕中位约 ±12° 的慢速相位示例。4×1DOF 能否形成有效步态取决于连杆，使用前必须校准。",
          keyframes: gaitFrames,
        },
      ],
    };
  }

  function normalizeCalibration(raw, errors, strict) {
    const source = Array.isArray(raw) ? raw : defaultCalibration();
    const normalized = LEGS.map((leg, index) => {
      const fallback = defaultCalibration()[index];
      const item = source[index] && typeof source[index] === "object" ? source[index] : fallback;
      const minUs = integer(item.min_us, fallback.min_us);
      const centerUs = integer(item.center_us, fallback.center_us);
      const maxUs = integer(item.max_us, fallback.max_us);
      const trimDeg = clamp(integer(item.trim_deg, 0), -30, 30);
      const valid = (
        minUs >= SERVO_ELECTRICAL_MIN &&
        maxUs <= SERVO_ELECTRICAL_MAX &&
        minUs < centerUs &&
        centerUs < maxUs
      );
      if (!valid) errors.push(`${leg.code} 校准必须满足 500 ≤ MIN < CENTER < MAX ≤ 2500`);
      if (!valid && !strict) return fallback;
      return {
        min_us: minUs,
        center_us: centerUs,
        max_us: maxUs,
        reverse: Boolean(item.reverse),
        trim_deg: trimDeg,
      };
    });
    return normalized;
  }

  function normalizeLed(raw, errors, path, strict) {
    const source = raw && typeof raw === "object" ? raw : {};
    const originalColor = String(source.color || "#35A7FF");
    const color = normalizeHex(originalColor);
    if (strict && color !== originalColor.toUpperCase()) errors.push(`${path} 灯光颜色不是 #RRGGBB`);
    const brightnessRaw = integer(source.brightness, 40);
    const pixelsRaw = integer(source.pixel_count, DEFAULT_PIXELS);
    if (strict && (brightnessRaw < 0 || brightnessRaw > 100)) errors.push(`${path} 亮度超出 0–100`);
    if (strict && (pixelsRaw < 1 || pixelsRaw > MAX_PIXELS)) errors.push(`${path} 灯珠数超出 1–${MAX_PIXELS}`);
    return {
      enabled: Boolean(source.enabled),
      color,
      brightness: clamp(brightnessRaw, 0, 100),
      pixel_count: clamp(pixelsRaw, 1, MAX_PIXELS),
    };
  }

  function normalizeProject(raw, { strict = false } = {}) {
    const errors = [];
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) {
      throw new ProjectValidationError(["工程根节点必须是 JSON 对象"]);
    }
    if (strict && raw.schema !== PROJECT_SCHEMA) errors.push(`schema 必须是 ${PROJECT_SCHEMA}`);
    if (strict && integer(raw.version, -1) !== PROJECT_VERSION) errors.push(`只支持工程版本 ${PROJECT_VERSION}`);
    const calibration = normalizeCalibration(raw.calibration, errors, strict);
    const rawActions = Array.isArray(raw.actions) ? raw.actions : [];
    if (strict && rawActions.length > MAX_ACTIONS) errors.push(`动作数量不能超过 ${MAX_ACTIONS}`);
    const actionIds = new Set();
    const frameIds = new Set();
    const actions = rawActions.slice(0, MAX_ACTIONS).map((rawAction, actionIndex) => {
      const source = rawAction && typeof rawAction === "object" ? rawAction : {};
      let actionId = normalizeIdentifier(
        source.id,
        uid("action"),
        errors,
        strict,
        `动作 ${actionIndex + 1} ID`,
      );
      if (actionIds.has(actionId)) {
        if (strict) errors.push(`动作 ID 重复：${actionId}`);
        actionId = uid("action");
      }
      actionIds.add(actionId);
      const rawFrames = Array.isArray(source.keyframes) ? source.keyframes : [];
      if (strict && rawFrames.length > MAX_KEYFRAMES) errors.push(`${source.name || `动作 ${actionIndex + 1}`} 关键帧过多`);
      const keyframes = rawFrames.slice(0, MAX_KEYFRAMES).map((rawFrame, frameIndex) => {
        const frame = rawFrame && typeof rawFrame === "object" ? rawFrame : {};
        let frameId = normalizeIdentifier(
          frame.id,
          uid("frame"),
          errors,
          strict,
          `关键帧 ${frameIndex + 1} ID`,
        );
        if (frameIds.has(frameId)) {
          if (strict) errors.push(`关键帧 ID 重复：${frameId}`);
          frameId = uid("frame");
        }
        frameIds.add(frameId);
        const timeRaw = integer(frame.time_ms, 0);
        const durationRaw = integer(frame.duration_ms, 600);
        if (strict && (timeRaw < 0 || timeRaw > MAX_PROJECT_DURATION_MS)) errors.push(`关键帧 ${frameIndex + 1} 时间越界`);
        if (strict && (durationRaw < 0 || durationRaw > 10000)) errors.push(`关键帧 ${frameIndex + 1} 过渡时间越界`);
        const rawServos = Array.isArray(frame.servos_us) ? frame.servos_us : [];
        if (strict && rawServos.length !== LEGS.length) errors.push(`关键帧 ${frameIndex + 1} 必须包含 4 路舵机`);
        const servosUs = LEGS.map((leg, servoIndex) => {
          const pulse = integer(rawServos[servoIndex], calibration[servoIndex].center_us);
          const safePulse = clamp(pulse, calibration[servoIndex].min_us, calibration[servoIndex].max_us);
          if (strict && pulse !== safePulse) {
            errors.push(`${source.name || `动作 ${actionIndex + 1}`} 的 ${leg.code} 脉宽 ${pulse} 超出本工程安全校准`);
          }
          return safePulse;
        });
        const easingRaw = String(frame.easing || "smooth");
        if (strict && !["linear", "smooth", "ease-in-out", "ease-in", "ease-out", "step"].includes(easingRaw)) {
          errors.push(`关键帧 ${frameIndex + 1} 缓动曲线不支持`);
        }
        const wireEasing = EASING_TO_WIRE[easingRaw] || "smooth";
        return {
          id: frameId,
          time_ms: clamp(timeRaw, 0, MAX_PROJECT_DURATION_MS),
          duration_ms: easingRaw === "step" ? 0 : clamp(durationRaw, 0, 10000),
          easing: wireEasing,
          servos_us: servosUs,
          led: normalizeLed(frame.led, errors, `关键帧 ${frameIndex + 1}`, strict),
        };
      }).sort((left, right) => left.time_ms - right.time_ms);
      for (let frameIndex = 1; frameIndex < keyframes.length; frameIndex += 1) {
        if (keyframes[frameIndex].time_ms <= keyframes[frameIndex - 1].time_ms) {
          if (strict) {
            errors.push(`${source.name || `动作 ${actionIndex + 1}`} 的关键帧时间必须严格递增且不能重复`);
          } else {
            keyframes[frameIndex].time_ms = Math.min(
              MAX_PROJECT_DURATION_MS,
              keyframes[frameIndex - 1].time_ms + 20,
            );
          }
        }
      }
      const inferredDuration = keyframes.reduce(
        (maximum, frame) => Math.max(maximum, frame.time_ms + Math.max(20, frame.duration_ms)),
        1000,
      );
      const durationRaw = integer(source.duration_ms, inferredDuration);
      if (strict && (durationRaw < 20 || durationRaw > MAX_PROJECT_DURATION_MS)) {
        errors.push(`${source.name || `动作 ${actionIndex + 1}`} 总时长越界`);
      }
      return {
        id: actionId,
        name: safeText(source.name, `动作 ${actionIndex + 1}`, 40),
        duration_ms: clamp(Math.max(durationRaw, inferredDuration), 20, MAX_PROJECT_DURATION_MS),
        loop: Boolean(source.loop),
        note: safeText(source.note, "", 240),
        default_rate: clamp(number(source.default_rate, 1), .25, 4),
        end_behavior: source.end_behavior === "off" ? "off" : "hold",
        keyframes,
      };
    });
    if (!actions.length) {
      if (strict) errors.push("工程至少需要一个动作");
      actions.push(createDefaultProject().actions[0]);
    }
    const projectId = normalizeIdentifier(raw.id, uid("project"), errors, strict, "工程 ID");
    if (errors.length && strict) throw new ProjectValidationError(errors);
    const fallbackNow = new Date().toISOString();
    return {
      schema: PROJECT_SCHEMA,
      version: PROJECT_VERSION,
      id: projectId,
      name: safeText(raw.name, "四足动作工程", 64),
      created_at: safeText(raw.created_at, fallbackNow, 40),
      updated_at: safeText(raw.updated_at, fallbackNow, 40),
      calibration,
      actions,
    };
  }

  function actionDerivedDuration(action) {
    return action.keyframes.reduce(
      (maximum, frame) => Math.max(maximum, frame.time_ms + frame.duration_ms),
      0,
    );
  }

  function assertBackendReadyAction(action) {
    let previous = -1;
    for (const frame of [...action.keyframes].sort((left, right) => left.time_ms - right.time_ms)) {
      if (!IDENTIFIER_RE.test(frame.id)) throw new ProjectValidationError([`${action.name} 含无效关键帧 ID`]);
      if (frame.time_ms <= previous) {
        throw new ProjectValidationError([`${action.name} 的关键帧时间重复；请拖开至少 20 ms 后再保存`]);
      }
      previous = frame.time_ms;
    }
  }

  function toBackendAction(action) {
    assertBackendReadyAction(action);
    const frames = [...action.keyframes]
      .sort((left, right) => left.time_ms - right.time_ms)
      .map((frame) => ({
        id: frame.id,
        time_ms: frame.time_ms,
        duration_ms: frame.duration_ms,
        easing: EASING_TO_WIRE[frame.easing] || "smooth",
        servos_us: frame.servos_us.map((pulse) => integer(pulse)),
        led: deepCopy(frame.led),
      }));
    const durationMs = frames.reduce(
      (maximum, frame) => Math.max(maximum, frame.time_ms + frame.duration_ms),
      0,
    );
    return {
      id: action.id,
      name: action.name,
      description: action.note || "",
      duration_ms: durationMs,
      default_rate: clamp(number(action.default_rate, 1), .25, 4),
      end_behavior: action.end_behavior === "off" ? "off" : "hold",
      loop: {
        enabled: Boolean(action.loop) && durationMs > 0,
        start_ms: 0,
        end_ms: durationMs,
        count: action.loop ? 0 : 1,
      },
      keyframes: frames,
    };
  }

  function toBackendProject(project) {
    const normalized = normalizeProject(project, { strict: true });
    const pixelCounts = normalized.actions.flatMap((action) => action.keyframes.map((frame) => frame.led.pixel_count));
    // The V2.1 toy uses a fixed eight-pixel board.  Action keyframes may
    // intentionally address fewer pixels, but that must not silently shrink
    // the hardware profile back to the legacy single status LED.
    const profilePixels = clamp(Math.max(DEFAULT_PIXELS, ...pixelCounts), 1, MAX_PIXELS);
    return {
      format: PROJECT_SCHEMA,
      version: PROJECT_VERSION,
      id: normalized.id,
      name: normalized.name,
      created_at: normalized.created_at,
      updated_at: normalized.updated_at,
      device_profile: {
        id: "arcs-mini-v2-1-studio",
        revision: 1,
        board: "arcs_mini_v2_1",
        servo_frequency_hz: 50,
        servos: normalized.calibration.map((calibration, index) => ({
          channel: index,
          pin: LEGS[index].pad,
          name: `${LEGS[index].code} · ${LEGS[index].label}`,
          min_us: calibration.min_us,
          center_us: calibration.center_us,
          max_us: calibration.max_us,
          invert: calibration.reverse,
          // The UI already bakes trim_deg into every commanded pulse. Keep
          // firmware trim at zero so a round trip can never apply it twice.
          trim_us: 0,
        })),
        led: {
          pin: "PA08",
          protocol: "ws2812-grb",
          pixel_count: profilePixels,
          max_pixel_count: MAX_PIXELS,
        },
      },
      actions: normalized.actions.map(toBackendAction),
    };
  }

  function fromBackendProject(project, baseProject = null) {
    if (!project || typeof project !== "object" || project.format !== PROJECT_SCHEMA) {
      return normalizeProject(project || {});
    }
    const profileServos = Array.isArray(project.device_profile?.servos)
      ? project.device_profile.servos
      : [];
    const canKeepUiTrim = baseProject?.id === project.id && Array.isArray(baseProject.calibration);
    const calibration = LEGS.map((leg, index) => {
      const source = profileServos[index] || {};
      const previous = canKeepUiTrim ? baseProject.calibration[index] : null;
      return {
        min_us: integer(source.min_us, SAFE_MIN),
        center_us: integer(source.center_us, SAFE_CENTER),
        max_us: integer(source.max_us, SAFE_MAX),
        reverse: Boolean(source.invert),
        trim_deg: previous ? clamp(integer(previous.trim_deg, 0), -30, 30) : 0,
      };
    });
    const uiProject = {
      schema: PROJECT_SCHEMA,
      version: PROJECT_VERSION,
      id: project.id,
      name: project.name,
      created_at: project.created_at,
      updated_at: project.updated_at,
      calibration,
      actions: (Array.isArray(project.actions) ? project.actions : []).map((action) => ({
        id: action.id,
        name: action.name,
        duration_ms: integer(action.duration_ms, 0),
        loop: Boolean(action.loop?.enabled),
        note: safeText(action.description, "", 240),
        default_rate: clamp(number(action.default_rate, 1), .25, 4),
        end_behavior: action.end_behavior === "off" ? "off" : "hold",
        keyframes: Array.isArray(action.keyframes) ? action.keyframes.map((frame) => ({
          id: frame.id,
          time_ms: frame.time_ms,
          duration_ms: frame.duration_ms,
          easing: frame.easing,
          servos_us: frame.servos_us,
          led: frame.led,
        })) : [],
      })),
    };
    return normalizeProject(uiProject);
  }

  const studio = {
    project: createDefaultProject(),
    activeActionId: "preset-slow-gait",
    selectedFrameId: null,
    poseAngles: [90, 90, 90, 90],
    light: defaultLed(),
    playheadMs: 0,
    online: false,
    armed: false,
    estop: false,
    snapshot: null,
    power: null,
    powerOverrideAllowed: false,
    dirty: false,
    serverSeen: false,
    projectInitialized: false,
    recording: false,
    recordingInfo: { state: "idle", active: false, started_at: null, duration_ms: 0, frame_count: 0 },
    pendingRecordName: "",
    playback: { state: "stopped", position_ms: 0, duration_ms: 0 },
    poseQueued: false,
    poseSending: false,
    frameTestSending: false,
    poseTimer: null,
    lastPoseFlushAt: Number.NEGATIVE_INFINITY,
    lightQueued: false,
    lightSending: false,
    lightTimer: null,
    saveTimer: null,
    remoteSaveTimer: null,
    actionSaveTimers: new Map(),
    pollTimer: null,
    eventSource: null,
    eventFallbackUsed: false,
    sseErrors: 0,
    holdTimer: null,
    playbackOrigin: null,
    playbackRaf: null,
    preview: { state: "stopped", position_ms: 0, rate: 1 },
    previewOrigin: null,
    previewRaf: null,
    imu: {
      available: false,
      fresh: false,
      sampled_at: null,
      age_ms: null,
      raw: null,
      attitude: null,
      error: null,
    },
    // Level baselines remain persisted in the backend's raw sensor attitude
    // frame. They are transformed alongside each sample at display time, so
    // an old ~-180 degree V2.1 baseline cannot apply the Rx180 correction twice.
    imuReference: { sensorLevelBaseline: null, sensorYawZeroDeg: 0 },
    // MPU6050 has no magnetometer, so heading drift must not rotate the model
    // unless the operator explicitly opts in for this browser session.
    viewerYawFollowEnabled: false,
    imuSafetyEnabled: false,
    imuTiltLimit: 35,
    imuSafetyTripped: false,
    imuPollTimer: null,
    imuPolling: false,
    connectionEpoch: 0,
    serverConnectionEpoch: null,
    connectionFingerprint: "",
    deviceCalibration: {
      snapshot: null,
      record: null,
      step: 1,
      initialized: false,
      loading: false,
      loadPromise: null,
      saving: false,
      safetyConfirmed: false,
      uiDirty: false,
      refreshTimer: null,
      profileSyncTimer: null,
      profileSyncPending: false,
      imuRun: null,
      lastError: null,
    },
    ledCalibration: {
      snapshot: null,
      calibration: deepCopy(LED_CALIBRATION_BYPASS),
      savedCalibration: deepCopy(LED_CALIBRATION_BYPASS),
      available: false,
      applicable: false,
      loading: false,
      saving: false,
      testing: false,
      testOverrideActive: false,
      testLabel: "",
      dirty: false,
      lastError: null,
      refreshTimer: null,
    },
    postureRoutine: {
      active: false,
      cancelRequested: false,
      operation: null,
      pendingPlan: null,
      completedSteps: 0,
    },
    actionDialogMode: "new",
    actionTemplate: "custom",
    drag: null,
    dogViewer: null,
    dogViewerDisposed: false,
  };

  function activeAction() {
    return studio.project.actions.find((action) => action.id === studio.activeActionId) || studio.project.actions[0];
  }

  function selectedFrame() {
    return activeAction()?.keyframes.find((frame) => frame.id === studio.selectedFrameId) || null;
  }

  function angleToPulse(index, angle) {
    return angleToPulseWithCalibration(index, angle, studio.project.calibration);
  }

  function pulseToAngle(index, pulse) {
    return pulseToAngleWithCalibration(index, pulse, studio.project.calibration);
  }

  function currentServosUs() {
    return studio.poseAngles.map((angle, index) => angleToPulse(index, angle));
  }

  function dogViewerState() {
    const orientation = displayedImuAttitude();
    return {
      angles_deg: studio.poseAngles.map((angle) => Math.round(number(angle, 90))),
      servos_us: currentServosUs(),
      armed: studio.armed,
      online: studio.online,
      playback_state: studio.preview.state === "playing" ? "previewing" : (studio.playback?.state || "stopped"),
      led_enabled: Boolean(studio.light.enabled && studio.light.brightness > 0),
      led_color: studio.light.color,
      led_brightness: studio.light.brightness,
      led_pixel_count: studio.light.pixel_count,
      body_orientation: VIEWER_ORIENTATION.toViewerOrientation(orientation, {
        followYaw: studio.viewerYawFollowEnabled,
        fresh: studio.imu.fresh,
      }),
    };
  }

  function syncDogViewer() {
    if (!studio.dogViewer || studio.dogViewerDisposed) return;
    try {
      studio.dogViewer.update(dogViewerState());
    } catch (error) {
      console.warn("3D posture preview update failed", error);
    }
  }

  function finiteOrNull(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }

  function wrapDegrees(value) {
    let wrapped = number(value, 0) % 360;
    if (wrapped > 180) wrapped -= 360;
    if (wrapped < -180) wrapped += 360;
    return wrapped;
  }

  function imuVector(raw, directKey, axisPrefix) {
    const direct = raw?.[directKey];
    if (Array.isArray(direct) && direct.length >= 3) {
      return direct.slice(0, 3).map(finiteOrNull);
    }
    const nested = direct && typeof direct === "object" ? direct : raw;
    const keys = ["x", "y", "z"];
    return keys.map((axis) => finiteOrNull(
      nested?.[axis] ?? nested?.[`${axisPrefix}_${axis}`] ?? nested?.[`${axisPrefix}${axis}`],
    ));
  }

  function normalizeImuTelemetry(payload) {
    const source = payload?.result?.imu || payload?.result || payload?.imu || payload;
    if (!source || typeof source !== "object") return null;
    const attitudeSource = source.attitude || source.orientation || {};
    const attitude = {
      roll_deg: finiteOrNull(attitudeSource.roll_deg ?? attitudeSource.roll),
      pitch_deg: finiteOrNull(attitudeSource.pitch_deg ?? attitudeSource.pitch),
      yaw_deg: finiteOrNull(attitudeSource.yaw_deg ?? attitudeSource.yaw),
    };
    const hasAttitude = Object.values(attitude).every((value) => value !== null);
    const rawSource = source.raw || source.sample || {};
    const scaledSource = source.scaled || rawSource;
    const accelG = imuVector(scaledSource, "accel_g", "accel");
    const gyroDps = imuVector(scaledSource, "gyro_dps", "gyro");
    const raw = {
      accel_g: accelG.some((value) => value !== null) ? accelG : null,
      gyro_dps: gyroDps.some((value) => value !== null) ? gyroDps : null,
      temperature_c: finiteOrNull(
        scaledSource.temperature_c ?? scaledSource.temp_c ?? rawSource.temperature_c ?? source.temperature_c,
      ),
    };
    const ageMs = finiteOrNull(source.age_ms ?? source.sample_age_ms ?? source.freshness_ms);
    const available = Boolean(source.available ?? source.valid ?? hasAttitude);
    const fresh = Boolean(source.fresh ?? (available && (ageMs === null || ageMs <= 1500)));
    const sampledAt = source.sampled_at ?? source.timestamp_ms ?? source.timestamp ?? null;
    const sampleSequence = source.sample_seq ?? source.sample_sequence ?? source.seq ?? null;
    return {
      available,
      fresh,
      sampled_at: sampledAt,
      sample_seq: sampleSequence,
      connection_epoch: finiteOrNull(source.connection_epoch),
      filter_epoch: finiteOrNull(source.filter_epoch),
      chip_id: safeText(source.chip_id, "", 64).toLowerCase() || null,
      current_connection_epoch: finiteOrNull(source.current_connection_epoch),
      current_filter_epoch: finiteOrNull(source.current_filter_epoch),
      current_chip_id: safeText(source.current_chip_id, "", 64).toLowerCase() || null,
      identity_valid: Boolean(source.identity_valid),
      age_ms: ageMs,
      attitude: hasAttitude ? attitude : null,
      raw,
      sensor: source.sensor && typeof source.sensor === "object" ? deepCopy(source.sensor) : null,
      health: source.health ? String(source.health) : null,
      error: source.error ? String(source.error) : null,
      yaw_relative: source.yaw_relative !== false,
    };
  }

  function displayedImuAttitude() {
    const attitude = studio.imu.attitude;
    if (!studio.imu.available || !attitude) return null;
    // Bind the mounting transform to the live backend board identity. Project
    // files are portable and must never be allowed to redefine physical axes.
    const board = localDeviceIdentity().board || IMU_MOUNTING.DEFAULT_BOARD;
    return IMU_MOUNTING.displayedBodyAttitude(
      attitude,
      studio.imuReference.sensorLevelBaseline,
      studio.imuReference.sensorYawZeroDeg,
      board,
    );
  }

  function formatImuVector(values, unit, digits = 2) {
    if (!Array.isArray(values) || values.some((value) => !Number.isFinite(value))) return `— ${unit}`;
    return `${values.map((value) => value.toFixed(digits)).join(" / ")} ${unit}`;
  }

  function renderImuTelemetry() {
    const status = $("imuStatus");
    if (!status) return;
    const attitude = displayedImuAttitude();
    const mode = !studio.imu.available ? "offline" : studio.imu.fresh ? "live" : "stale";
    status.dataset.state = mode;
    status.classList.remove("offline", "stale", "error");
    if (mode !== "live") status.classList.add(studio.imu.error ? "error" : mode);
    let indicator = status.querySelector("i");
    if (!indicator) indicator = document.createElement("i");
    indicator.setAttribute("aria-hidden", "true");
    status.replaceChildren(
      indicator,
      document.createTextNode(mode === "live" ? "IMU LIVE" : mode === "stale" ? "IMU STALE" : "IMU OFFLINE"),
    );
    const readout = status.closest(".imu-readout");
    readout?.classList.remove("offline", "stale");
    if (mode !== "live") readout?.classList.add(mode);
    const freshness = $("imuFreshness");
    if (freshness) {
      freshness.textContent = studio.imu.available
        ? studio.imu.fresh
          ? `${studio.imu.age_ms == null ? "实时" : `${Math.max(0, Math.round(studio.imu.age_ms))} ms`} · YAW RELATIVE`
          : "数据已过期 · 不参与安全判断"
        : (studio.imu.error || "等待 MPU6050 数据");
    }
    if ($("imuRoll")) $("imuRoll").textContent = attitude ? `${attitude.roll_deg.toFixed(1)}°` : "—";
    if ($("imuPitch")) $("imuPitch").textContent = attitude ? `${attitude.pitch_deg.toFixed(1)}°` : "—";
    if ($("imuYaw")) $("imuYaw").textContent = attitude ? `${attitude.yaw_deg.toFixed(1)}°` : "—";
    if ($("imuAccel")) $("imuAccel").textContent = formatImuVector(studio.imu.raw?.accel_g, "g");
    if ($("imuGyro")) $("imuGyro").textContent = formatImuVector(studio.imu.raw?.gyro_dps, "°/s", 1);
    if ($("imuYawFollowToggle")) $("imuYawFollowToggle").checked = studio.viewerYawFollowEnabled;
    if ($("imuYawFollowState")) {
      $("imuYawFollowState").textContent = studio.viewerYawFollowEnabled
        ? "已开启 · 3D 使用相对 Yaw，长时间静置仍可能缓慢漂移"
        : "关闭（推荐）· 3D 航向已锁定，只跟随 Roll / Pitch";
    }
    if ($("imuSafetyToggle")) $("imuSafetyToggle").checked = studio.imuSafetyEnabled;
    if ($("imuZeroBtn")) $("imuZeroBtn").disabled = !studio.imu.available || !studio.imu.fresh;
    if ($("imuSafetyToggle")) $("imuSafetyToggle").disabled = !studio.imu.available || !studio.imu.fresh;
    if ($("imuTiltLimit") && document.activeElement !== $("imuTiltLimit")) {
      $("imuTiltLimit").value = String(studio.imuTiltLimit);
    }
    if ($("imuTiltLimit")) $("imuTiltLimit").disabled = !studio.imuSafetyEnabled;
    syncDogViewer();
  }

  function applyImuTelemetry(payload) {
    const telemetry = normalizeImuTelemetry(payload);
    if (!telemetry) return;
    studio.imu = telemetry;
    if (telemetry.fresh) studio.imuSafetyTripped = false;
    renderImuTelemetry();
    evaluateImuSafety();
    ingestImuCalibrationSample(telemetry);
  }

  function zeroImuOrientation() {
    if (!studio.imu.available || !studio.imu.attitude) {
      toast("IMU 尚未就绪", "收到有效姿态数据后才能执行 Yaw 会话归零。", "warning");
      return;
    }
    studio.imuReference.sensorYawZeroDeg = studio.imu.attitude.yaw_deg;
    renderImuTelemetry();
    toast("Yaw 会话零点已更新", "仅当前浏览器会话生效，不会写入 Roll / Pitch 水平标定。", "success");
  }

  async function evaluateImuSafety() {
    if (!studio.imuSafetyEnabled || studio.imuSafetyTripped || !studio.armed || !studio.imu.fresh) return;
    const attitude = displayedImuAttitude();
    if (!attitude) return;
    const tilt = Math.max(Math.abs(attitude.roll_deg), Math.abs(attitude.pitch_deg));
    if (tilt < studio.imuTiltLimit) return;
    studio.imuSafetyTripped = true;
    await emergencyStop({ quiet: true });
    toast(
      "IMU 倾倒保护已触发",
      `机身倾角 ${tilt.toFixed(1)}° 超过 ${studio.imuTiltLimit}°，四路 PWM 已急停。请扶稳机器后重新解锁。`,
      "error",
      9000,
    );
  }

  async function pollImu() {
    if (studio.imuPolling || !studio.serverSeen) return;
    studio.imuPolling = true;
    const requestEpoch = studio.connectionEpoch;
    try {
      const payload = await requestJson(API.imu);
      if (requestEpoch !== studio.connectionEpoch) return;
      applyImuTelemetry(payload);
    } catch (error) {
      studio.imu = { ...studio.imu, fresh: false, error: error.message || String(error) };
      renderImuTelemetry();
    } finally {
      studio.imuPolling = false;
    }
  }

  function calibrationResult(payload) {
    const result = payload?.result ?? payload;
    return result && typeof result === "object" ? result : null;
  }

  function normalizeLedOutputCalibration(value = {}) {
    const source = value && typeof value === "object" ? value : {};
    const order = String(source.color_order || LED_CALIBRATION_BYPASS.color_order).toUpperCase();
    return {
      version: Math.max(1, integer(source.version, 1)),
      color_order: LED_COLOR_ORDERS.includes(order) ? order : LED_CALIBRATION_BYPASS.color_order,
      r_gain_permille: clamp(integer(source.r_gain_permille, 1000), 0, 1000),
      g_gain_permille: clamp(integer(source.g_gain_permille, 1000), 0, 1000),
      b_gain_permille: clamp(integer(source.b_gain_permille, 1000), 0, 1000),
      gamma_x100: clamp(integer(source.gamma_x100, 100), 100, 300),
      persistent: Boolean(source.persistent),
    };
  }

  function ledCalibrationResult(payload) {
    const result = payload?.data?.led_calibration ?? payload?.data ??
      payload?.result?.led_calibration ?? payload?.result ??
      payload?.state?.led_calibration ?? payload?.led_calibration ?? payload;
    return result && typeof result === "object" ? result : null;
  }

  function ledCalibrationEqual(left, right) {
    if (!left || !right) return false;
    return ["color_order", "r_gain_permille", "g_gain_permille", "b_gain_permille", "gamma_x100"]
      .every((key) => left[key] === right[key]);
  }

  function ledCalibrationReasonText(reason, fallback = "") {
    const messages = {
      device_offline: "设备离线，恢复 ADB 连接后才能校色。",
      identity_invalid: "当前连接尚未完成设备身份识别。",
      chip_id_unavailable: "尚未读取当前设备 chip_id，校色已禁用。",
      board_mismatch: "校色记录与当前板型不匹配。",
      calibration_not_found: "当前设备还没有灯板校色记录。",
      firmware_unsupported: "当前固件不支持灯板输出校色，请更新固件后重试。",
      unsupported_firmware: "当前固件不支持灯板输出校色，请更新固件后重试。",
      unsupported: "当前固件不支持灯板输出校色，请更新固件后重试。",
      capability_unavailable: "当前固件未上报灯板校色能力。",
      command_unsupported: "当前固件未实现灯板校色命令。",
    };
    return messages[String(reason || "")] || fallback || "当前设备暂不能使用灯板校色。";
  }

  function applyLedCalibrationSnapshot(value) {
    if (!value || typeof value !== "object") throw new Error("灯板校色服务返回了无效快照。");
    const source = value.data && typeof value.data === "object" ? value.data : value;
    const current = source.current && typeof source.current === "object" ? source.current : {};
    const rawCalibration = source.calibration ?? source.record?.led_calibration ?? source.record ?? null;
    const available = Boolean(source.available ?? source.supported ?? rawCalibration);
    const normalized = normalizeLedOutputCalibration(rawCalibration || LED_CALIBRATION_BYPASS);
    const snapshot = {
      available,
      applicable: Boolean(source.applicable ?? (available && rawCalibration)),
      reason: safeText(source.reason, "", 64),
      connection_epoch: finiteOrNull(source.connection_epoch ?? current.connection_epoch),
      chip_id: safeText(source.chip_id ?? current.chip_id, "", 64).toLowerCase(),
      board: safeText(source.board ?? current.board, "", 64),
      identity_valid: Boolean(source.identity_valid ?? current.identity_valid),
      calibration: normalized,
    };
    const state = studio.ledCalibration;
    state.snapshot = snapshot;
    state.available = available;
    state.applicable = snapshot.applicable;
    state.calibration = deepCopy(normalized);
    state.savedCalibration = deepCopy(normalized);
    state.dirty = false;
    state.lastError = null;
    renderLedCalibration();
    return snapshot;
  }

  function ledCalibrationIdentityCheck() {
    const state = studio.ledCalibration;
    const local = localDeviceIdentity();
    const snapshot = state.snapshot;
    if (!studio.serverSeen || !local.online) return { ok: false, message: ledCalibrationReasonText("device_offline") };
    if (!snapshot) {
      return { ok: false, message: state.lastError || "尚未读取当前设备的灯板校色状态。" };
    }
    if (!snapshot.available) return { ok: false, message: ledCalibrationReasonText(snapshot.reason) };
    if (!local.chipId || !local.identityValid || !snapshot.identity_valid) {
      return { ok: false, message: ledCalibrationReasonText("identity_invalid") };
    }
    if (
      snapshot.connection_epoch === null ||
      local.connectionEpoch === null ||
      snapshot.connection_epoch !== local.connectionEpoch
    ) {
      return { ok: false, message: "校色快照与当前 connection_epoch 不一致，已防止写入。" };
    }
    if (!snapshot.chip_id || snapshot.chip_id !== local.chipId) {
      return { ok: false, message: "校色快照与当前 chip_id 不一致，已防止误用。" };
    }
    if (snapshot.board && local.board && snapshot.board !== local.board) {
      return { ok: false, message: ledCalibrationReasonText("board_mismatch") };
    }
    return { ok: true, local, snapshot };
  }

  function ledCalibrationControls() {
    return [
      "ledColorOrder",
      "ledRGainRange", "ledRGainNumber",
      "ledGGainRange", "ledGGainNumber",
      "ledBGainRange", "ledBGainNumber",
      "ledGammaRange", "ledGammaNumber",
      "ledCalibrationBypassPreset", "ledCalibrationTypicalPreset",
    ].map($).filter(Boolean);
  }

  function renderLedCalibration() {
    const state = studio.ledCalibration;
    const calibration = normalizeLedOutputCalibration(state.calibration);
    state.calibration = calibration;
    const identity = ledCalibrationIdentityCheck();
    const motionStopped = calibrationMotionStopped();
    const busy = state.loading || state.saving || state.testing;
    const editable = identity.ok && !busy;
    const status = $("ledCalibrationStatus");
    const notice = $("ledCalibrationNotice");
    const panel = $("ledCalibrationPanel");
    if (!status || !notice || !panel) return;

    $("ledColorOrder").value = calibration.color_order;
    for (const channel of ["R", "G", "B"]) {
      const gain = calibration[`${channel.toLowerCase()}_gain_permille`];
      $( `led${channel}GainRange` ).value = gain;
      $( `led${channel}GainNumber` ).value = (gain / 10).toFixed(gain % 10 ? 1 : 0);
    }
    $("ledGammaRange").value = calibration.gamma_x100;
    $("ledGammaNumber").value = (calibration.gamma_x100 / 100).toFixed(2);

    status.className = "led-calibration-status";
    notice.className = "led-calibration-notice";
    if (state.loading) {
      status.textContent = "读取中";
      status.classList.add("is-loading");
      notice.textContent = "正在读取当前灯板的颜色校准记录…";
    } else if (state.saving) {
      status.textContent = "保存中";
      status.classList.add("is-loading");
      notice.textContent = "正在校验设备身份并写入当前 chip_id…";
    } else if (!studio.serverSeen) {
      status.textContent = "等待连接";
      status.classList.add("is-loading");
      notice.textContent = "正在连接本地运动控制服务…";
    } else if (!identity.ok) {
      status.textContent = state.lastError ? "服务不可用" : "不可用";
      status.classList.add("is-unavailable");
      notice.classList.add("is-warning");
      notice.textContent = identity.message;
    } else if (state.testOverrideActive) {
      status.textContent = "测试输出";
      status.classList.add("is-dirty");
      notice.classList.add("is-dirty");
      notice.textContent = `${state.testLabel || "三原色"} 5% 低亮测试正保持。点击“恢复当前编排灯色”结束测试。`;
    } else if (state.dirty) {
      status.textContent = "未保存";
      status.classList.add("is-dirty");
      notice.classList.add("is-dirty");
      notice.textContent = motionStopped
        ? "输出参数有未保存修改。保存后再用 5% 三原色检查实物。"
        : "修改已保留。请先锁定舵机并停止播放、预览和录制，再保存到设备。";
    } else if (!state.applicable) {
      status.textContent = "待初始化";
      status.classList.add("is-ready");
      notice.textContent = "当前设备尚无持久校色。可从旁路默认或典型灯珠起点开始。";
    } else {
      status.textContent = calibration.persistent ? "已绑定" : "已应用";
      status.classList.add("is-ready");
      notice.classList.add("is-ready");
      notice.textContent = `当前输出 ${calibration.color_order} · R${calibration.r_gain_permille / 10}% / G${calibration.g_gain_permille / 10}% / B${calibration.b_gain_permille / 10}% · γ${(calibration.gamma_x100 / 100).toFixed(2)}`;
    }

    const snapshot = state.snapshot;
    const chip = snapshot?.chip_id || localDeviceIdentity().chipId;
    const board = snapshot?.board || localDeviceIdentity().board;
    $("ledCalibrationIdentity").textContent = chip
      ? `${board || "DEVICE"} · ${chip.toUpperCase()} · EPOCH ${snapshot?.connection_epoch ?? "—"}`
      : "DEVICE —";

    panel.classList.toggle("is-disabled", !identity.ok);
    panel.classList.toggle("is-dirty", state.dirty);
    ledCalibrationControls().forEach((control) => { control.disabled = !editable; });
    document.querySelectorAll("[data-led-primary]").forEach((button) => {
      button.disabled = !editable || state.dirty || studio.lightSending;
    });
    $("ledCalibrationRestoreBtn").disabled = !editable || studio.lightSending;
    $("ledCalibrationResetBtn").disabled = !editable || !motionStopped;
    $("ledCalibrationSaveBtn").disabled = !editable || !state.dirty || !motionStopped;
    $("ledCalibrationSaveBtn").textContent = state.saving ? "正在保存…" : "保存校色";
  }

  function updateLedCalibrationDraft(next) {
    const state = studio.ledCalibration;
    state.calibration = normalizeLedOutputCalibration({ ...state.calibration, ...next });
    state.dirty = !ledCalibrationEqual(state.calibration, state.savedCalibration);
    renderLedCalibration();
  }

  function queueLedCalibrationRefresh(delay = 100) {
    const state = studio.ledCalibration;
    window.clearTimeout(state.refreshTimer);
    state.refreshTimer = window.setTimeout(() => {
      refreshLedCalibration({ quiet: true }).catch(() => {});
    }, delay);
  }

  function resetLedCalibrationForConnection(snapshot) {
    const state = studio.ledCalibration;
    window.clearTimeout(state.refreshTimer);
    state.snapshot = null;
    state.calibration = deepCopy(LED_CALIBRATION_BYPASS);
    state.savedCalibration = deepCopy(LED_CALIBRATION_BYPASS);
    state.available = false;
    state.applicable = false;
    state.loading = false;
    state.saving = false;
    state.testing = false;
    state.testOverrideActive = false;
    state.testLabel = "";
    state.dirty = false;
    state.lastError = null;
    renderLedCalibration();
    const identity = localDeviceIdentity(snapshot);
    if (identity.online && identity.chipId && identity.identityValid) queueLedCalibrationRefresh(80);
  }

  async function refreshLedCalibration({ quiet = false } = {}) {
    const state = studio.ledCalibration;
    if (state.loading || state.saving) return state.snapshot;
    window.clearTimeout(state.refreshTimer);
    const requestEpoch = studio.connectionEpoch;
    state.loading = true;
    state.lastError = null;
    renderLedCalibration();
    try {
      const payload = await requestJson(API.ledCalibration);
      if (requestEpoch !== studio.connectionEpoch) return null;
      const result = ledCalibrationResult(payload);
      if (!result) throw new Error("灯板校色服务未返回数据。");
      return applyLedCalibrationSnapshot(result);
    } catch (error) {
      if (requestEpoch !== studio.connectionEpoch) return null;
      state.snapshot = null;
      state.available = false;
      state.applicable = false;
      state.lastError = error.message || String(error);
      if (!quiet) toast("读取灯板校色失败", state.lastError, "error");
      return null;
    } finally {
      if (requestEpoch === studio.connectionEpoch) {
        state.loading = false;
        renderLedCalibration();
      }
    }
  }

  function ledCalibrationIdentityPayload() {
    const identity = ledCalibrationIdentityCheck();
    if (!identity.ok) throw new Error(identity.message);
    return {
      connection_epoch: identity.snapshot.connection_epoch,
      chip_id: identity.snapshot.chip_id,
      board: identity.snapshot.board,
    };
  }

  async function sendLedCalibrationLight(light, label) {
    const state = studio.ledCalibration;
    if (!studio.online) throw new Error("设备离线，无法发送灯光。");
    if (studio.lightSending) throw new Error("上一条灯光命令尚未完成。");
    window.clearTimeout(studio.lightTimer);
    studio.lightQueued = false;
    studio.lightSending = true;
    state.testing = true;
    renderLedCalibration();
    setCommandBusy(true, "LIGHT CAL");
    try {
      await requestJson(API.light, { method: "POST", body: JSON.stringify(light) });
      $("lastCommand").textContent = `LIGHT CAL · ${label} · ${light.brightness}% · ${light.pixel_count}px`;
    } finally {
      studio.lightSending = false;
      state.testing = false;
      setCommandBusy(false);
      renderLedCalibration();
      if (studio.lightQueued) studio.lightTimer = window.setTimeout(flushLight, LIGHT_COALESCE_MS);
    }
  }

  async function testLedPrimary(color, name) {
    const identity = ledCalibrationIdentityCheck();
    if (!identity.ok) {
      toast("灯板测试不可用", identity.message, "warning");
      return;
    }
    if (studio.ledCalibration.dirty) {
      toast("请先保存校色", "三原色测试只使用已保存的当前设备校色。", "warning");
      return;
    }
    const sample = defaultLed({
      enabled: true,
      color,
      brightness: 5,
      pixel_count: studio.light.pixel_count,
    });
    try {
      studio.ledCalibration.testOverrideActive = true;
      studio.ledCalibration.testLabel = `纯${name}`;
      await sendLedCalibrationLight(sample, `纯${name}`);
      toast(`已发送纯${name}`, "5% 低亮测试；未修改动作关键帧或当前逻辑颜色。", "success", 4200);
    } catch (error) {
      studio.ledCalibration.testOverrideActive = false;
      studio.ledCalibration.testLabel = "";
      renderLedCalibration();
      toast("三原色测试失败", error.message, "error");
    }
  }

  async function restoreLogicalLight({ quiet = false } = {}) {
    try {
      await sendLedCalibrationLight(deepCopy(studio.light), "恢复编排灯色");
      studio.ledCalibration.testOverrideActive = false;
      studio.ledCalibration.testLabel = "";
      renderLedCalibration();
      if (!quiet) toast("已恢复编排灯色", `${studio.light.color} · ${studio.light.brightness}%`, "success");
      return true;
    } catch (error) {
      if (!quiet) toast("恢复灯色失败", error.message, "error");
      return false;
    }
  }

  async function saveLedCalibration() {
    const state = studio.ledCalibration;
    if (state.saving || !state.dirty) return;
    const requestEpoch = studio.connectionEpoch;
    let identity;
    try {
      identity = ledCalibrationIdentityPayload();
    } catch (error) {
      toast("保存灯板校色失败", error.message, "error");
      return;
    }
    const calibration = normalizeLedOutputCalibration(state.calibration);
    state.saving = true;
    renderLedCalibration();
    try {
      const payload = await requestJson(API.ledCalibration, {
        method: "PUT",
        body: JSON.stringify({
          ...identity,
          color_order: calibration.color_order,
          r_gain_permille: calibration.r_gain_permille,
          g_gain_permille: calibration.g_gain_permille,
          b_gain_permille: calibration.b_gain_permille,
          gamma_x100: calibration.gamma_x100,
        }),
      });
      if (requestEpoch !== studio.connectionEpoch) throw new Error("保存期间设备连接已变化，结果已作废。");
      const result = ledCalibrationResult(payload);
      if (result && ("available" in result || "calibration" in result || "color_order" in result)) {
        applyLedCalibrationSnapshot("color_order" in result
          ? { ...identity, identity_valid: true, available: true, applicable: true, calibration: result }
          : { ...identity, identity_valid: true, ...result });
      } else {
        applyLedCalibrationSnapshot({
          ...identity,
          identity_valid: true,
          available: true,
          applicable: true,
          calibration: { ...calibration, persistent: true },
        });
      }
      state.testOverrideActive = false;
      state.testLabel = "";
      toast("灯板校色已保存", `${calibration.color_order} · 已绑定 ${identity.chip_id.toUpperCase()}`, "success");
      if (!result?.light_resent) await restoreLogicalLight({ quiet: true });
    } catch (error) {
      if (requestEpoch === studio.connectionEpoch) {
        state.lastError = error.message || String(error);
        toast("保存灯板校色失败", state.lastError, "error");
      }
    } finally {
      if (requestEpoch === studio.connectionEpoch) {
        state.saving = false;
        renderLedCalibration();
      }
    }
  }

  async function resetLedCalibration() {
    const state = studio.ledCalibration;
    if (state.saving) return;
    const requestEpoch = studio.connectionEpoch;
    let identity;
    try {
      identity = ledCalibrationIdentityPayload();
    } catch (error) {
      toast("重置灯板校色失败", error.message, "error");
      return;
    }
    state.saving = true;
    renderLedCalibration();
    try {
      const payload = await requestJson(API.ledCalibration, {
        method: "PUT",
        body: JSON.stringify({ ...identity, reset: true }),
      });
      if (requestEpoch !== studio.connectionEpoch) throw new Error("重置期间设备连接已变化，结果已作废。");
      const result = ledCalibrationResult(payload);
      if (result && ("available" in result || "calibration" in result || "color_order" in result)) {
        applyLedCalibrationSnapshot("color_order" in result
          ? { ...identity, identity_valid: true, available: true, applicable: true, calibration: result }
          : { ...identity, identity_valid: true, ...result });
      } else {
        applyLedCalibrationSnapshot({
          ...identity,
          identity_valid: true,
          available: true,
          applicable: true,
          calibration: { ...LED_CALIBRATION_BYPASS, persistent: true },
        });
      }
      state.testOverrideActive = false;
      state.testLabel = "";
      toast("设备校色已重置", "已恢复 GRB · 100 / 100 / 100% · γ1.00。", "success");
      if (!result?.light_resent) await restoreLogicalLight({ quiet: true });
    } catch (error) {
      if (requestEpoch === studio.connectionEpoch) {
        state.lastError = error.message || String(error);
        toast("重置灯板校色失败", state.lastError, "error");
      }
    } finally {
      if (requestEpoch === studio.connectionEpoch) {
        state.saving = false;
        renderLedCalibration();
      }
    }
  }

  function localDeviceIdentity(snapshot = studio.snapshot) {
    const source = snapshot && typeof snapshot === "object" ? snapshot : {};
    const online = extractOnline(source);
    const chipId = safeText(source.chip_id || source.device_id, "", 64).toLowerCase();
    const boardValue = source.board?.board || source.board;
    const board = safeText(boardValue, "", 64);
    const transportId = source.transport_id == null ? "" : String(source.transport_id);
    const connectionEpoch = finiteOrNull(source.connection_epoch);
    const identityEpoch = finiteOrNull(source.identity_epoch);
    const identityValid = Boolean(
      source.identity_valid ??
      (online && chipId && connectionEpoch !== null && identityEpoch === connectionEpoch)
    );
    return { online, chipId, board, transportId, connectionEpoch, identityEpoch, identityValid };
  }

  function connectionFingerprint(snapshot = studio.snapshot) {
    const identity = localDeviceIdentity(snapshot);
    if (!identity.online) return "offline";
    return `online:${identity.connectionEpoch ?? "?"}:${identity.transportId || "?"}:` +
      `${identity.identityValid ? identity.chipId : "pending"}:${identity.board || "unknown"}`;
  }

  function calibrationEpochs(snapshot) {
    const current = snapshot?.current || {};
    return {
      connection: finiteOrNull(
        snapshot?.connection_epoch ?? snapshot?.current_connection_epoch ?? current.connection_epoch,
      ),
      filter: finiteOrNull(snapshot?.filter_epoch ?? snapshot?.current_filter_epoch ?? current.filter_epoch),
      sample: finiteOrNull(snapshot?.sample_seq ?? current.sample_seq),
      chipId: safeText(current.chip_id ?? snapshot?.current_chip_id, "", 64).toLowerCase(),
      identityValid: Boolean(current.identity_valid ?? snapshot?.identity_valid),
    };
  }

  function sensorIdentity(telemetry = studio.imu) {
    const sensor = telemetry?.sensor;
    if (!sensor || typeof sensor !== "object") return "";
    const who = safeText(sensor.who_am_i, "", 32);
    if (!who) return "";
    return [sensor.bus, sensor.address, who].map((value) => safeText(value, "?", 32)).join(":");
  }

  function calibrationMotionStopped() {
    return Boolean(
      !studio.armed &&
      !studio.recording &&
      (studio.playback?.state || "stopped") === "stopped" &&
      (studio.preview?.state || "stopped") === "stopped"
    );
  }

  function calibrationIdentityCheck(snapshot, { allowMissing = false } = {}) {
    const local = localDeviceIdentity();
    if (!studio.serverSeen || !local.online) return { ok: false, message: "设备离线，请先恢复 ADB 连接。" };
    if (!local.chipId || !local.identityValid) return { ok: false, message: "正在识别当前设备 chip_id。" };
    if (!snapshot || typeof snapshot !== "object") return { ok: false, message: "尚未读取设备校准状态。" };
    const current = snapshot.current;
    if (!current || typeof current !== "object" || !current.online) {
      return { ok: false, message: "校准服务报告设备离线。" };
    }
    const epochs = calibrationEpochs(snapshot);
    const currentChip = epochs.chipId;
    const currentBoard = safeText(current.board, "", 64);
    if (!epochs.identityValid) {
      return { ok: false, message: "校准服务尚未在当前连接 epoch 完成身份识别。" };
    }
    if (epochs.connection === null || local.connectionEpoch === null || epochs.connection !== local.connectionEpoch) {
      return { ok: false, message: "校准 connection_epoch 与当前连接不一致。" };
    }
    if (!currentChip || currentChip !== local.chipId) {
      return { ok: false, message: "当前 chip_id 与校准快照不一致，已禁用旧记录。" };
    }
    if (!currentBoard || (local.board && currentBoard !== local.board)) {
      return { ok: false, message: "当前板型与校准快照不一致。" };
    }
    if (snapshot.applicable && snapshot.record) return { ok: true, bootstrap: false };
    if (allowMissing && snapshot.reason === "calibration_not_found") return { ok: true, bootstrap: true };
    const reasons = {
      device_offline: "设备离线，校准不可用。",
      chip_id_unavailable: "尚未识别 chip_id，校准不可用。",
      calibration_not_found: "当前设备尚未创建舵机校准。",
      board_mismatch: "校准记录与当前板型不匹配。",
    };
    return { ok: false, message: reasons[snapshot.reason] || "当前校准记录不可用。" };
  }

  function syncProjectCalibrationFromRecord(record) {
    if (!record || !Array.isArray(record.servo_calibration) || record.servo_calibration.length !== LEGS.length) return;
    const nextCalibration = record.servo_calibration.map((item) => ({
      min_us: integer(item.min_us, SAFE_MIN),
      center_us: integer(item.center_us, SAFE_CENTER),
      max_us: integer(item.max_us, SAFE_MAX),
      reverse: Boolean(item.reverse),
      trim_deg: clamp(integer(item.trim_deg, 0), -30, 30),
    }));
    const profileChanged = nextCalibration.some((item, index) => {
      const previous = studio.project.calibration?.[index];
      return !previous || ["min_us", "center_us", "max_us", "trim_deg"].some(
        (key) => integer(previous[key], NaN) !== integer(item[key], NaN),
      ) || Boolean(previous.reverse) !== item.reverse;
    });
    studio.project.calibration = nextCalibration;
    const baseline = record.imu_mounting_baseline_deg;
    studio.imuReference.sensorLevelBaseline = (
      baseline && Number.isFinite(Number(baseline.roll)) && Number.isFinite(Number(baseline.pitch))
    ) ? { roll: Number(baseline.roll), pitch: Number(baseline.pitch) } : null;
    renderCalibration();
    renderPose();
    renderImuTelemetry();
    if (profileChanged) {
      studio.deviceCalibration.profileSyncPending = true;
      queueCalibrationProfileSync();
    }
  }

  function applyCalibrationSnapshot(snapshot, { syncUi = true } = {}) {
    if (!snapshot || typeof snapshot !== "object") throw new Error("校准服务返回了无效快照。");
    const calibration = studio.deviceCalibration;
    calibration.snapshot = deepCopy(snapshot);
    calibration.record = snapshot.applicable && snapshot.record ? deepCopy(snapshot.record) : null;
    calibration.lastError = null;
    if (calibration.record && syncUi) syncProjectCalibrationFromRecord(calibration.record);
    if (!calibration.record) {
      studio.project.calibration = defaultCalibration();
      studio.imuReference.sensorLevelBaseline = null;
      renderCalibration();
      renderPose();
      renderImuTelemetry();
    }
    if (!calibration.initialized) {
      const steps = snapshot.completed_steps || {};
      calibration.step = !steps.servo ? 1 : !steps.home ? 2 : 3;
      calibration.initialized = true;
    }
    renderCalibrationWizard();
    renderHomeButton();
  }

  function cancelImuCalibration(reason = "", { quiet = false } = {}) {
    const calibration = studio.deviceCalibration;
    const run = calibration.imuRun;
    if (!run) return;
    run.cancelled = true;
    window.clearInterval(run.timer);
    calibration.imuRun = null;
    if ($("calibrationCountdown")) $("calibrationCountdown").textContent = "3.0";
    if ($("calibrationCountdownLabel")) $("calibrationCountdownLabel").textContent = "等待开始";
    renderCalibrationWizard();
    if (!quiet && reason) toast("IMU 水平标定已取消", reason, "warning", 6000);
  }

  function updateConnectionCalibrationEpoch(snapshot) {
    const nextFingerprint = connectionFingerprint(snapshot);
    if (nextFingerprint === studio.connectionFingerprint) return;
    const calibration = studio.deviceCalibration;
    const hadIdentity = Boolean(studio.connectionFingerprint);
    studio.connectionFingerprint = nextFingerprint;
    studio.connectionEpoch += 1;
    studio.serverConnectionEpoch = localDeviceIdentity(snapshot).connectionEpoch;
    cancelImuCalibration("设备连接或身份已变化。", { quiet: !hadIdentity });
    calibration.snapshot = null;
    calibration.record = null;
    calibration.initialized = false;
    calibration.loading = false;
    calibration.loadPromise = null;
    calibration.saving = false;
    calibration.safetyConfirmed = false;
    calibration.uiDirty = false;
    calibration.profileSyncPending = false;
    window.clearTimeout(calibration.profileSyncTimer);
    calibration.lastError = null;
    if ($("calibrationSafetyConfirm")) $("calibrationSafetyConfirm").checked = false;
    studio.imuReference = { sensorLevelBaseline: null, sensorYawZeroDeg: 0 };
    studio.project.calibration = defaultCalibration();
    renderCalibration();
    renderPose();
    renderImuTelemetry();
    renderCalibrationWizard();
    resetLedCalibrationForConnection(snapshot);
    const identity = localDeviceIdentity(snapshot);
    if (identity.online && identity.chipId && identity.identityValid) queueCalibrationRefresh(60);
  }

  function queueCalibrationRefresh(delay = 120) {
    const calibration = studio.deviceCalibration;
    window.clearTimeout(calibration.refreshTimer);
    calibration.refreshTimer = window.setTimeout(() => {
      refreshDeviceCalibration({ quiet: true }).catch(() => {});
    }, delay);
  }

  function queueCalibrationProfileSync(delay = 180) {
    const calibration = studio.deviceCalibration;
    window.clearTimeout(calibration.profileSyncTimer);
    if (!calibration.profileSyncPending) return;
    calibration.profileSyncTimer = window.setTimeout(async () => {
      if (!calibration.profileSyncPending || !calibrationMotionStopped() || !studio.serverSeen) return;
      const identity = calibrationIdentityCheck(calibration.snapshot);
      if (!identity.ok || !servoCalibrationEqual(
        servoCalibrationFromUi(),
        calibration.snapshot?.record?.servo_calibration,
      )) return;
      const saved = await saveProjectRemote({ quiet: true, force: true });
      if (saved) calibration.profileSyncPending = false;
    }, delay);
  }

  async function refreshDeviceCalibration({ syncUi = true, quiet = false } = {}) {
    const calibration = studio.deviceCalibration;
    if (calibration.loadPromise) return calibration.loadPromise;
    const requestEpoch = studio.connectionEpoch;
    calibration.loading = true;
    renderCalibrationWizard();
    let promise;
    promise = (async () => {
      try {
        const payload = await requestJson(API.calibration);
        if (requestEpoch !== studio.connectionEpoch) return null;
        const snapshot = calibrationResult(payload);
        applyCalibrationSnapshot(snapshot, { syncUi });
        return snapshot;
      } catch (error) {
        if (requestEpoch !== studio.connectionEpoch) return null;
        calibration.lastError = error.message || String(error);
        if (!quiet) toast("读取校准失败", calibration.lastError, "error");
        return null;
      } finally {
        if (requestEpoch === studio.connectionEpoch && calibration.loadPromise === promise) {
          calibration.loading = false;
          calibration.loadPromise = null;
          renderCalibrationWizard();
        }
      }
    })();
    calibration.loadPromise = promise;
    return promise;
  }

  function servoCalibrationFromUi({ showErrors = false } = {}) {
    const values = readCalibrationFromUi({ showErrors });
    return values?.map((item, index) => ({
      channel: index,
      min_us: item.min_us,
      center_us: item.center_us,
      max_us: item.max_us,
      reverse: Boolean(item.reverse),
      trim_deg: item.trim_deg,
    })) || null;
  }

  function servoCalibrationEqual(left, right) {
    if (!Array.isArray(left) || !Array.isArray(right) || left.length !== LEGS.length || right.length !== LEGS.length) {
      return false;
    }
    return left.every((item, index) => servoCalibrationItemEqual(item, right[index], index));
  }

  function servoCalibrationItemEqual(left, right, index) {
    return Boolean(
      left &&
      right &&
      integer(left.channel, index) === integer(right.channel, index) &&
      ["min_us", "center_us", "max_us", "trim_deg"].every(
        (key) => integer(left[key], NaN) === integer(right[key], NaN),
      ) &&
      Boolean(left.reverse) === Boolean(right.reverse)
    );
  }

  function coreCalibrationRecord(snapshot, servoCalibration, overrides = {}) {
    const current = snapshot?.current || {};
    const existing = snapshot?.applicable ? snapshot.record : null;
    return {
      chip_id: String(current.chip_id || "").toLowerCase(),
      board: String(current.board || ""),
      mounting_revision: integer(existing?.mounting_revision ?? current.mounting_revision, 1),
      servo_calibration: deepCopy(servoCalibration),
      home_pose_us: overrides.home_pose_us !== undefined
        ? deepCopy(overrides.home_pose_us)
        : deepCopy(existing?.home_pose_us ?? null),
      imu_mounting_baseline_deg: overrides.imu_mounting_baseline_deg !== undefined
        ? deepCopy(overrides.imu_mounting_baseline_deg)
        : deepCopy(existing?.imu_mounting_baseline_deg ?? null),
    };
  }

  async function saveCalibrationRecord(record, { allowMissing = false } = {}) {
    const calibration = studio.deviceCalibration;
    if (calibration.saving) throw new Error("校准记录正在保存。");
    const requestEpoch = studio.connectionEpoch;
    const preflight = await refreshDeviceCalibration({ syncUi: false, quiet: true });
    if (requestEpoch !== studio.connectionEpoch) throw new Error("设备身份在保存前已变化。");
    const identity = calibrationIdentityCheck(preflight, { allowMissing });
    if (!identity.ok) throw new Error(identity.message);
    if (!calibration.safetyConfirmed) throw new Error("请先确认机身已架空并清场。");
    if (!calibrationMotionStopped()) throw new Error("保存前必须锁定舵机并停止播放、预览和录制。");
    const current = preflight.current;
    if (record.chip_id !== String(current.chip_id).toLowerCase() || record.board !== current.board) {
      throw new Error("待保存记录与当前设备身份不一致。");
    }
    calibration.saving = true;
    renderCalibrationWizard();
    try {
      const connectionEpoch = calibrationEpochs(preflight).connection;
      if (!Number.isInteger(connectionEpoch) || connectionEpoch < 1) {
        throw new Error("校准快照缺少有效 connection_epoch。");
      }
      const payload = await requestJson(API.calibration, {
        method: "PUT",
        body: JSON.stringify({ ...record, connection_epoch: connectionEpoch }),
      });
      if (requestEpoch !== studio.connectionEpoch) throw new Error("保存期间设备连接已变化，结果已作废。");
      const result = calibrationResult(payload);
      const afterSave = calibrationIdentityCheck(result);
      if (!afterSave.ok) throw new Error(afterSave.message);
      applyCalibrationSnapshot(result, { syncUi: true });
      calibration.uiDirty = false;
      return result;
    } finally {
      if (requestEpoch === studio.connectionEpoch) {
        calibration.saving = false;
        renderCalibrationWizard();
      }
    }
  }

  function setCalibrationMetric(id, text, percent = 0) {
    const value = $(id);
    if (!value) return;
    value.textContent = text;
    const bar = value.parentElement?.querySelector("i > b");
    if (bar) bar.style.width = `${clamp(number(percent, 0), 0, 100)}%`;
  }

  function setCalibrationStep(step, { focus = false } = {}) {
    const calibration = studio.deviceCalibration;
    const steps = calibration.snapshot?.completed_steps || {};
    const requested = clamp(integer(step, 1), 1, 3);
    if (requested >= 2 && !steps.servo) {
      toast("请先完成舵机校准", "Step 1 保存成功后才能进入 Home 姿态。", "warning");
      return;
    }
    if (requested >= 3 && !steps.home) {
      toast("请先保存 Home", "当前安全站立姿态尚未写入设备校准。", "warning");
      return;
    }
    calibration.step = requested;
    renderCalibrationWizard();
    if (focus) $( `calibrationStep${requested}Panel` )?.focus();
  }

  function renderCalibrationWizard() {
    const wizard = $("calibrationWizard");
    if (!wizard) return;
    const calibration = studio.deviceCalibration;
    const snapshot = calibration.snapshot;
    const steps = snapshot?.completed_steps || {};
    const uiServo = servoCalibrationFromUi();
    const servoMatches = Boolean(
      snapshot?.applicable &&
      steps.servo &&
      uiServo &&
      servoCalibrationEqual(uiServo, snapshot.record?.servo_calibration)
    );
    const servoComplete = Boolean(steps.servo && servoMatches);
    const homeComplete = Boolean(servoComplete && steps.home && Array.isArray(snapshot?.record?.home_pose_us));
    const imuComplete = Boolean(homeComplete && steps.imu && snapshot?.record?.imu_mounting_baseline_deg);
    if (calibration.step > 1 && !steps.servo) calibration.step = 1;
    if (calibration.step > 2 && !steps.home) calibration.step = 2;
    const identity = calibrationIdentityCheck(snapshot, { allowMissing: true });
    const local = localDeviceIdentity();
    const safe = Boolean(calibration.safetyConfirmed && identity.ok);
    const stopped = calibrationMotionStopped();
    wizard.dataset.calibrationStep = String(calibration.step);
    wizard.setAttribute("aria-busy", String(calibration.loading || calibration.saving || Boolean(calibration.imuRun)));

    const status = $("calibrationWizardStatus");
    if (status) {
      status.className = "calibration-wizard-status";
      if (calibration.loading) {
        status.classList.add("pending");
        status.lastChild.textContent = "读取中";
      } else if (calibration.saving) {
        status.classList.add("pending");
        status.lastChild.textContent = "保存中";
      } else if (!local.online || !local.chipId) {
        status.classList.add("pending");
        status.lastChild.textContent = local.online ? "识别设备" : "等待设备";
      } else if (imuComplete) {
        status.classList.add("complete");
        status.lastChild.textContent = "校准完成";
      } else if (!identity.ok) {
        status.classList.add("pending");
        status.lastChild.textContent = "不可用";
      } else {
        status.classList.add("pending");
        status.lastChild.textContent = `第 ${calibration.step} / 3 步`;
      }
    }

    const safety = $("calibrationSafetyConfirm");
    if (safety) {
      safety.checked = calibration.safetyConfirmed;
      safety.disabled = !identity.ok || calibration.loading || calibration.saving;
    }

    [1, 2, 3].forEach((step) => {
      const tab = $( `calibrationStep${step}Tab` );
      const panel = $( `calibrationStep${step}Panel` );
      const active = calibration.step === step;
      const accessible = step === 1 || (step === 2 ? steps.servo : steps.home);
      tab?.closest("li")?.classList.toggle("active", active);
      if (tab) {
        tab.setAttribute("aria-selected", String(active));
        tab.tabIndex = active ? 0 : -1;
        tab.disabled = !accessible;
      }
      if (panel) {
        panel.hidden = !active;
        panel.classList.toggle("active", active);
      }
    });

    const stepStates = [
      { complete: servoComplete, active: calibration.step === 1 },
      { complete: homeComplete, active: calibration.step === 2 },
      { complete: imuComplete, active: calibration.step === 3 },
    ];
    stepStates.forEach((value, index) => {
      const node = $( `calibrationStep${index + 1}State` );
      if (!node) return;
      node.className = value.complete ? "complete" : value.active ? "pending" : "";
      node.textContent = value.complete ? "已完成" : value.active ? "进行中" : "未开始";
    });

    LEGS.forEach((leg, index) => {
      const item = uiServo?.[index];
      if ($(`calibrationServo${index}Center`)) {
        $(`calibrationServo${index}Center`).textContent = item ? `${item.center_us} µs` : "—";
      }
      if ($(`calibrationServo${index}Direction`)) {
        $(`calibrationServo${index}Direction`).textContent = item ? (item.reverse ? "反向" : "正向") : "无效";
      }
      if ($(`calibrationServo${index}Limits`)) {
        $(`calibrationServo${index}Limits`).textContent = item ? `${item.min_us}–${item.max_us}` : "—";
      }
      const state = $(`calibrationServo${index}State`);
      if (state) {
        const saved = servoCalibrationItemEqual(item, snapshot?.record?.servo_calibration?.[index], index);
        state.className = saved ? "complete" : "";
        state.textContent = !item ? "格式错误" : saved ? "已保存" : "待保存";
      }
    });

    const home = snapshot?.record?.home_pose_us;
    const homeAngles = $("calibrationHomeAngles")?.querySelectorAll("strong") || [];
    homeAngles.forEach((node, index) => {
      if (Array.isArray(home) && uiServo && home[index] != null) {
        node.textContent = `${pulseToAngleWithCalibration(index, home[index], uiServo)}°`;
      } else {
        node.textContent = "—°";
      }
    });
    if ($("calibrationHomeState")) {
      $("calibrationHomeState").textContent = homeComplete ? "已保存" : "未捕获";
      $("calibrationHomeState").className = homeComplete ? "complete" : "";
    }
    if ($("calibrationHomeLimitsQuality")) {
      $("calibrationHomeLimitsQuality").textContent = homeComplete ? "通过" : "待检查";
    }
    if ($("calibrationHomeSymmetryQuality")) {
      $("calibrationHomeSymmetryQuality").textContent = homeComplete ? "已记录" : "—°";
    }
    if ($("calibrationHomeStableQuality")) {
      $("calibrationHomeStableQuality").textContent = homeComplete ? "已确认" : "— s";
    }

    if ($("calibrationCenterAllBtn")) $("calibrationCenterAllBtn").disabled = !safe || calibration.saving;
    if ($("calibrationCaptureHomeBtn")) {
      $("calibrationCaptureHomeBtn").disabled = !safe || !servoComplete || !stopped || calibration.saving;
    }
    if ($("calibrationPreviewHomeBtn")) {
      $("calibrationPreviewHomeBtn").disabled = !safe || !homeComplete;
    }
    if ($("calibrationImuStartBtn")) {
      $("calibrationImuStartBtn").disabled = !safe || !homeComplete || !stopped || !studio.imu.available ||
        !studio.imu.fresh || !sensorIdentity() || calibration.saving || Boolean(calibration.imuRun);
      $("calibrationImuStartBtn").textContent = calibration.imuRun ? "正在采集…" : "开始 3 秒水平标定";
    }
    if ($("calibrationYawZeroBtn")) {
      $("calibrationYawZeroBtn").disabled = !safe || !studio.imu.available || !studio.imu.fresh;
    }
    if ($("calibrationBackBtn")) $("calibrationBackBtn").disabled = calibration.step === 1 || calibration.saving || Boolean(calibration.imuRun);

    const next = $("calibrationNextBtn");
    const hint = $("calibrationStepHint");
    if (next && hint) {
      const pendingStepLabel = calibration.step === 1
        ? (servoComplete ? "继续：保存 Home" : "保存舵机校准")
        : calibration.step === 2
          ? (homeComplete ? "继续：IMU 水平标定" : "捕获并保存 Home")
          : (imuComplete ? "三步校准已完成" : "开始 3 秒水平标定");
      if (!identity.ok) {
        next.disabled = true;
        next.textContent = "等待设备";
        hint.lastChild.textContent = identity.message;
      } else if (!calibration.safetyConfirmed) {
        next.disabled = true;
        next.textContent = pendingStepLabel;
        hint.lastChild.textContent = "先确认机身已架空并清空机械行程。";
      } else if (!stopped) {
        next.disabled = true;
        next.textContent = pendingStepLabel;
        hint.lastChild.textContent = "保存前请锁定舵机，并停止播放、预览与录制。";
      } else if (calibration.step === 1) {
        next.disabled = !uiServo || calibration.saving;
        next.textContent = calibration.saving ? "正在保存…" : servoComplete ? "继续：保存 Home" : "保存舵机校准";
        hint.lastChild.textContent = servoComplete
          ? "舵机参数已与当前 chip_id 绑定。"
          : "确认 4 路中位、方向和软限位，锁定输出后保存。";
      } else if (calibration.step === 2) {
        next.disabled = !servoComplete || calibration.saving;
        next.textContent = homeComplete ? "继续：IMU 水平标定" : "捕获并保存 Home";
        hint.lastChild.textContent = homeComplete
          ? "Home 姿态已保存，可进入 IMU 水平标定。"
          : "当前目标姿态将以原始脉宽写入 home_pose_us。";
      } else {
        next.disabled = imuComplete || !homeComplete || !studio.imu.fresh || !sensorIdentity() || Boolean(calibration.imuRun);
        next.textContent = imuComplete ? "三步校准已完成" : calibration.imuRun ? "正在采集…" : "开始 3 秒水平标定";
        hint.lastChild.textContent = imuComplete
          ? "Roll / Pitch 已按当前设备保存；Yaw 仍仅支持会话归零。"
          : "保持机身水平静置；采集中不要触碰机身或线缆。";
      }
    }

    if (!calibration.imuRun) {
      if ($("calibrationCountdown")) $("calibrationCountdown").textContent = imuComplete ? "0.0" : "3.0";
      if ($("calibrationCountdownLabel")) {
        $("calibrationCountdownLabel").textContent = imuComplete ? "标定已保存" : "等待开始";
      }
      setCalibrationMetric("calibrationStationaryMetric", imuComplete ? "已通过" : "—", imuComplete ? 100 : 0);
      setCalibrationMetric("calibrationVibrationMetric", "— mg", 0);
      setCalibrationMetric("calibrationGyroMetric", "— °/s", 0);
      setCalibrationMetric("calibrationSampleMetric", imuComplete ? "≥6 / 已保存" : "0 / ≥6", imuComplete ? 100 : 0);
      setCalibrationMetric("calibrationSpanMetric", imuComplete ? "≥3.0 / 3.0 s" : "— / 3.0 s", imuComplete ? 100 : 0);
    }
    if (calibration.profileSyncPending && stopped) queueCalibrationProfileSync();
    renderSafePostureControls();
  }

  function angleToPulseWithCalibration(index, angle, calibration) {
    const item = calibration[index];
    const visibleAngle = clamp(number(angle, 90), 0, 180);
    const directed = item.reverse ? 180 - visibleAngle : visibleAngle;
    const physical = clamp(directed + item.trim_deg, 0, 180);
    if (physical <= 90) {
      return Math.round(item.min_us + (physical / 90) * (item.center_us - item.min_us));
    }
    return Math.round(item.center_us + ((physical - 90) / 90) * (item.max_us - item.center_us));
  }

  function pulseToAngleWithCalibration(index, pulse, calibration) {
    const item = calibration[index];
    const bounded = clamp(number(pulse, item.center_us), item.min_us, item.max_us);
    let physical;
    if (bounded <= item.center_us) {
      physical = 90 * (bounded - item.min_us) / Math.max(1, item.center_us - item.min_us);
    } else {
      physical = 90 + 90 * (bounded - item.center_us) / Math.max(1, item.max_us - item.center_us);
    }
    const directed = clamp(physical - item.trim_deg, 0, 180);
    return Math.round(item.reverse ? 180 - directed : directed);
  }

  async function saveServoCalibrationStep() {
    const calibration = studio.deviceCalibration;
    try {
      const servo = servoCalibrationFromUi({ showErrors: true });
      if (!servo) return;
      const snapshot = await refreshDeviceCalibration({ syncUi: false, quiet: true });
      const identity = calibrationIdentityCheck(snapshot, { allowMissing: true });
      if (!identity.ok) throw new Error(identity.message);
      if (!calibration.safetyConfirmed) throw new Error("请先确认机身已架空并清场。");
      if (!calibrationMotionStopped()) {
        throw new Error("请先锁定舵机并停止播放、预览与录制，再保存 Step 1。");
      }
      const changed = !servoCalibrationEqual(servo, snapshot?.record?.servo_calibration);
      const record = coreCalibrationRecord(snapshot, servo, changed ? {
        home_pose_us: null,
        imu_mounting_baseline_deg: null,
      } : {});
      await saveCalibrationRecord(record, { allowMissing: true });
      calibration.step = 2;
      calibration.uiDirty = false;
      renderCalibrationWizard();
      toast(
        "舵机校准已保存",
        changed ? "新的中位、方向与软限位已绑定当前 chip_id；下游 Home / IMU 需重新确认。" : "设备校准记录已确认。",
        "success",
        6000,
      );
    } catch (error) {
      toast("舵机校准未保存", error.message || String(error), "error", 7000);
    }
  }

  function centerAllForCalibration() {
    const calibration = studio.deviceCalibration;
    const identity = calibrationIdentityCheck(calibration.snapshot, { allowMissing: true });
    if (!identity.ok || !calibration.safetyConfirmed) {
      toast("安全确认未通过", identity.ok ? "请先确认机身已架空并清场。" : identity.message, "warning");
      return;
    }
    studio.poseAngles = [90, 90, 90, 90];
    renderPose({ schedule: studio.armed, source: "calibration-center" });
    toast(
      studio.armed ? "四路中位命令已下发" : "四路中位仅预览",
      studio.armed ? "观察机构方向；如有顶死或嗡鸣请立即急停。" : "舵机未解锁，本次不会驱动真机。",
      studio.armed ? "warning" : "info",
      5200,
    );
  }

  function currentHomePulses(record) {
    const servo = record?.servo_calibration;
    if (!Array.isArray(servo) || servo.length !== LEGS.length) return null;
    if (!servoCalibrationEqual(servoCalibrationFromUi(), servo)) return null;
    const pulses = studio.poseAngles.map((angle, index) => angleToPulseWithCalibration(index, angle, servo));
    const valid = pulses.every((pulse, index) => (
      Number.isInteger(pulse) && pulse >= servo[index].min_us && pulse <= servo[index].max_us
    ));
    return valid ? pulses : null;
  }

  async function captureHomeCalibration() {
    const calibration = studio.deviceCalibration;
    try {
      const snapshot = await refreshDeviceCalibration({ syncUi: false, quiet: true });
      const identity = calibrationIdentityCheck(snapshot);
      if (!identity.ok) throw new Error(identity.message);
      if (!calibration.safetyConfirmed) throw new Error("请先确认机身已架空并清场。");
      if (!calibrationMotionStopped()) throw new Error("捕获 Home 前必须锁定舵机并停止所有播放、预览与录制。");
      if (!snapshot.completed_steps?.servo || !snapshot.record) throw new Error("舵机校准尚未完成。");
      const home = currentHomePulses(snapshot.record);
      if (!home) throw new Error("当前姿态未使用已保存的舵机校准，或脉宽超出软限位。");
      const changed = !Array.isArray(snapshot.record.home_pose_us) || home.some(
        (pulse, index) => pulse !== snapshot.record.home_pose_us[index],
      );
      const record = coreCalibrationRecord(snapshot, snapshot.record.servo_calibration, {
        home_pose_us: home,
        imu_mounting_baseline_deg: changed ? null : snapshot.record.imu_mounting_baseline_deg,
      });
      await saveCalibrationRecord(record);
      calibration.step = 3;
      renderCalibrationWizard();
      toast(
        "Home / 站立姿态已保存",
        `${home.join(" / ")} µs${changed ? "；请继续重新确认 IMU 水平基准。" : "。"}`,
        "success",
        6200,
      );
    } catch (error) {
      toast("Home 姿态未保存", error.message || String(error), "error", 7000);
    }
  }

  function calibratedHomePose() {
    const snapshot = studio.deviceCalibration.snapshot;
    const identity = calibrationIdentityCheck(snapshot);
    const record = snapshot?.record;
    if (!identity.ok || !snapshot?.completed_steps?.servo || !snapshot?.completed_steps?.home || !record) {
      return { ok: false, message: identity.ok ? "当前设备没有可用的 home_pose_us。" : identity.message };
    }
    if (!Array.isArray(record.home_pose_us) || record.home_pose_us.length !== LEGS.length) {
      return { ok: false, message: "Home 记录格式无效，已拒绝使用默认 1500 µs 回退。" };
    }
    if (!servoCalibrationEqual(servoCalibrationFromUi(), record.servo_calibration)) {
      return { ok: false, message: "当前舵机参数与该 Home 记录不一致。" };
    }
    const pulses = record.home_pose_us.map((pulse, index) => integer(pulse, NaN));
    const withinLimits = pulses.every((pulse, index) => (
      Number.isInteger(pulse) &&
      pulse >= record.servo_calibration[index].min_us &&
      pulse <= record.servo_calibration[index].max_us
    ));
    if (!withinLimits) return { ok: false, message: "Home 脉宽超出当前设备软限位。" };
    return { ok: true, record, pulses };
  }

  function derivePronePulses(homePulses, servoCalibration, record) {
    if (!Array.isArray(homePulses) || !Array.isArray(servoCalibration) ||
        homePulses.length !== LEGS.length || servoCalibration.length !== LEGS.length) {
      throw new Error("起身基准或舵机标定不完整。");
    }
    const chipId = String(record?.chip_id || "").toLowerCase();
    const profile = SAFE_PRONE_PROFILE_BY_CHIP[chipId];
    if (!profile) {
      throw new Error("当前设备尚未单独标定整机趴姿。");
    }
    const servoSignature = servoCalibration.map((item) => (
      `${item.min_us}/${item.center_us}/${item.max_us}/${item.reverse ? 1 : 0}/${item.trim_deg}`
    )).join("|");
    const profileMatches = (
      record?.board === profile.board &&
      integer(record?.mounting_revision, -1) === profile.mountingRevision &&
      homePulses.every((pulse, index) => pulse === profile.homePulses[index]) &&
      servoSignature === profile.servoSignature
    );
    if (!profileMatches) {
      throw new Error("当前 Home / 舵机标定已变更，整机趴姿候选已失效。");
    }
    if (!Array.isArray(profile.pronePulses) || profile.pronePulses.length !== LEGS.length) {
      throw new Error("当前设备的整机趴姿记录不完整。");
    }
    return profile.pronePulses.map((rawPulse, index) => {
      const pulse = Number(rawPulse);
      if (!Number.isInteger(pulse)) throw new Error(`通道 ${index} 的趴姿脉宽不是整数。`);
      return pulse;
    });
  }

  function calibratedPronePose() {
    const home = calibratedHomePose();
    if (!home.ok) return home;
    let pulses;
    try {
      pulses = derivePronePulses(
        home.pulses,
        home.record.servo_calibration,
        home.record,
      );
    } catch (error) {
      return { ok: false, message: error.message || String(error) };
    }
    const withinLimits = pulses.every((pulse, index) => {
      const item = home.record.servo_calibration[index];
      const homeTravel = Math.abs(home.pulses[index] - item.center_us);
      return Number.isInteger(pulse) &&
        homeTravel >= 100 &&
        pulse >= Math.max(SAFE_POSTURE_MIN_US, item.min_us) &&
        pulse <= Math.min(SAFE_POSTURE_MAX_US, item.max_us);
    });
    if (!withinLimits) {
      return {
        ok: false,
        message: "派生趴姿超出当前设备的同步测试范围，已拒绝执行。",
      };
    }
    return { ok: true, record: home.record, pulses };
  }

  function safePosturePlan(operation) {
    const routine = SAFE_POSTURE_ROUTINES[operation];
    if (!routine) return { ok: false, message: "未知的安全姿态命令。" };
    if (studio.postureRoutine.active) return { ok: false, message: "已有四腿同步姿态命令正在执行。" };
    if (!studio.serverSeen || !studio.online) return { ok: false, message: "设备离线，请先恢复 ADB 连接。" };
    if (studio.armed) return { ok: false, message: "同步姿态命令要求 OUTPUT 保持 LOCKED。" };
    if (studio.recording) return { ok: false, message: "动作录制未停止。" };
    if ((studio.playback?.state || "stopped") !== "stopped") {
      return { ok: false, message: "动作播放未停止。" };
    }
    const identity = localDeviceIdentity();
    if (!identity.identityValid || !identity.chipId || !identity.board || identity.connectionEpoch === null) {
      return { ok: false, message: "当前设备 chip_id / connection_epoch 尚未确认。" };
    }
    const target = operation === "stand" ? calibratedHomePose() : calibratedPronePose();
    if (!target.ok) return target;
    return {
      ok: true,
      operation,
      label: routine.label,
      pulses: [...target.pulses],
      record: target.record,
      identity: {
        connectionEpoch: identity.connectionEpoch,
        chipId: identity.chipId,
        board: identity.board,
        fingerprint: connectionFingerprint(),
      },
      startLabel: operation === "stand" ? "实体已趴稳" : "实体已站稳",
      targetLabel: operation === "stand" ? "已标定 Home" : "标定中点趴姿",
      startRequirement: operation === "stand"
        ? "执行前必须确认实体已稳定趴下，不能从未知姿态开始"
        : "实体必须稳定站立；四路中点趴姿尚未完成实体验证，不能从未知姿态开始",
      confirmLabel: operation === "stand"
        ? "已趴稳并扶稳机身，急停可触达"
        : "已站稳并扶稳机身，急停可触达",
    };
  }

  function renderSafePostureControls() {
    ["stand", "prone"].forEach((operation) => {
      const button = operation === "stand" ? $("safeStandBtn") : $("safeProneBtn");
      if (!button) return;
      const routine = SAFE_POSTURE_ROUTINES[operation];
      const running = studio.postureRoutine.active && studio.postureRoutine.operation === operation;
      const plan = safePosturePlan(operation);
      button.disabled = studio.postureRoutine.active || !plan.ok;
      button.classList.toggle("running", running);
      button.textContent = running
        ? `${routine.label}执行中…`
        : routine.label;
      button.title = running
        ? "四条腿正在同一平滑过渡中运动"
        : (plan.ok ? "四条腿同帧运动，完成后自动关闭全部输出" : plan.message);
    });
  }

  function openSafePostureDialog(operation) {
    stopOfflinePreview({ keepPosition: true });
    const plan = safePosturePlan(operation);
    if (!plan.ok) {
      toast("同步姿态命令不可用", plan.message, "warning", 6500);
      renderSafePostureControls();
      return;
    }
    studio.postureRoutine.pendingPlan = plan;
    $("safePostureDialogTitle").textContent = `确认执行${plan.label}`;
    $("safePostureDialogDescription").textContent =
      "上位机将一次同时驱动四条腿，过渡完成后在服务端关闭全部 PWM 并复读状态。";
    $("safePostureStartLabel").textContent = plan.startLabel;
    $("safePostureMode").textContent = "FL + FR + RR + RL";
    $("safePostureTargetLabel").textContent = plan.targetLabel;
    $("safePosturePulses").textContent = `${plan.pulses.join(" / ")} µs`;
    $("safePostureStartRequirement").textContent = plan.startRequirement;
    $("safePostureConfirmLabel").textContent = plan.confirmLabel;
    $("safePostureConfirm").checked = false;
    $("safePostureRunBtn").disabled = true;
    $("safePostureRunBtn").textContent = `执行${plan.label}`;
    $("safePostureDialog").showModal();
  }

  function cancelSafePostureRoutine() {
    if (!studio.postureRoutine.active) return;
    studio.postureRoutine.cancelRequested = true;
    setPoseAck("同步姿态取消中 · 关闭全部输出", "error");
  }

  function buildSafePostureRequest(plan) {
    return {
      connection_epoch: plan.identity.connectionEpoch,
      chip_id: plan.identity.chipId,
      board: plan.identity.board,
      servos_us: [...plan.pulses],
      duration_ms: SAFE_POSTURE_DURATION_MS,
      safety_confirmed: true,
    };
  }

  function safePostureResultIsSafe(result, expectedPulses) {
    return Boolean(
      result?.accepted &&
      result?.target_reached &&
      result?.outputs_safe &&
      Array.isArray(result?.servos_us) &&
      result.servos_us.length === LEGS.length &&
      result.servos_us.every((pulse, index) => pulse === expectedPulses[index]) &&
      Array.isArray(result?.servo_state?.current_us) &&
      result.servo_state.current_us.every((pulse, index) => pulse === expectedPulses[index]) &&
      result?.servo_state?.enabled_mask === 0 &&
      !result?.servo_state?.active &&
      result?.servo_state?.watchdog_ms === 0
    );
  }

  async function runSafePostureRoutine() {
    const pending = studio.postureRoutine.pendingPlan;
    if (!pending || studio.postureRoutine.active || !$("safePostureConfirm").checked) return;
    const fresh = safePosturePlan(pending.operation);
    const unchanged = fresh.ok &&
      fresh.identity.fingerprint === pending.identity.fingerprint &&
      fresh.pulses.every((pulse, index) => pulse === pending.pulses[index]);
    if (!unchanged) {
      toast("执行前状态已变化", fresh.ok ? "设备身份或标定目标已更新，请重新确认。" : fresh.message, "warning", 6500);
      $("safePostureDialog").close();
      studio.postureRoutine.pendingPlan = null;
      renderSafePostureControls();
      return;
    }

    const state = studio.postureRoutine;
    state.active = true;
    state.cancelRequested = false;
    state.operation = fresh.operation;
    state.completedSteps = 0;
    state.pendingPlan = null;
    $("safePostureDialog").close();
    renderSafePostureControls();
    setCommandBusy(true, fresh.label.toUpperCase());
    let failure = null;
    try {
      if (state.cancelRequested) throw new Error("已由急停取消同步姿态命令。");
      const identity = localDeviceIdentity();
      if (connectionFingerprint() !== fresh.identity.fingerprint || !identity.identityValid) {
        throw new Error("执行前设备连接或 chip_id 已变化。");
      }
      setPoseAck(`${fresh.label} · 四腿同帧运动中`, "pending");
      $("lastCommand").textContent =
        `${fresh.label.toUpperCase()} · ${fresh.pulses.join("/")} µs · SMOOTH`;
      const response = await requestJson(API.postureTest, {
        method: "POST",
        body: JSON.stringify(buildSafePostureRequest(fresh)),
      });
      if (!safePostureResultIsSafe(response.result, fresh.pulses)) {
        throw new Error("同步姿态结束后未确认四路 PWM 与看门狗已全部关闭。");
      }
      studio.poseAngles = fresh.pulses.map((pulse, index) => (
        pulseToAngleWithCalibration(index, pulse, fresh.record.servo_calibration)
      ));
      renderPose({ schedule: false, source: `safe-${fresh.operation}` });
      setPoseAck(`${fresh.label}完成 · PWM 已关闭`, "ok");
      $("lastCommand").textContent =
        `${fresh.label.toUpperCase()} · ${fresh.pulses.join("/")} µs · OUTPUTS OFF`;
      toast(
        `${fresh.label}完成`,
        `四条腿已同时完成过渡，PWM 和看门狗均已关闭；当前仍为 LOCKED。`,
        "success",
        6200,
      );
    } catch (error) {
      failure = error;
      try {
        await requestJson(API.estop, { method: "POST", body: "{}" });
      } catch (stopError) {
        failure = new Error(`${error.message || String(error)}；急停状态未确认: ${stopError.message || String(stopError)}`);
      }
      setPoseAck(`${fresh.label}已中止`, "error");
      toast(
        `${fresh.label}已中止`,
        `${failure.message || String(failure)} 已要求关闭全部输出。`,
        "error",
        8000,
      );
    } finally {
      state.active = false;
      state.cancelRequested = false;
      state.operation = null;
      state.completedSteps = 0;
      state.pendingPlan = null;
      setCommandBusy(false);
      renderSafePostureControls();
    }
  }

  function applyCalibratedHome({ allowPhysical = true, source = "home" } = {}) {
    const home = calibratedHomePose();
    if (!home.ok) {
      toast("Home 姿态不可用", home.message, "warning", 6500);
      return false;
    }
    studio.poseAngles = home.pulses.map((pulse, index) => (
      pulseToAngleWithCalibration(index, pulse, home.record.servo_calibration)
    ));
    const send = Boolean(
      allowPhysical &&
      studio.armed &&
      studio.online &&
      studio.deviceCalibration.safetyConfirmed
    );
    renderPose({ schedule: send, source });
    toast(
      send ? "Home 姿态已下发" : "Home 姿态仅预览",
      send ? `${home.pulses.join(" / ")} µs` : "未解锁或尚未完成安全确认，真机不会运动。",
      send ? "success" : "info",
      4800,
    );
    return true;
  }

  function homeCommandReadiness() {
    if (!studio.serverSeen || !studio.online) return { ok: false, message: "设备离线，不能发送 Home。" };
    if (!studio.armed || studio.estop) return { ok: false, message: "请先解锁舵机输出。" };
    if ((studio.playback?.state || "stopped") !== "stopped") {
      return { ok: false, message: "动作播放中，停止后才能发送 Home。" };
    }
    if (studio.recording) return { ok: false, message: "动作录制中，停止并保存后才能发送 Home。" };
    if (studio.poseSending || studio.frameTestSending) {
      return { ok: false, message: "已有姿态命令正在执行。" };
    }
    if (studio.postureRoutine.active) return { ok: false, message: "四腿同步姿态命令正在执行。" };
    if (studio.snapshot?.safety_off_pending) return { ok: false, message: "正在确认安全断输出。" };
    const home = calibratedHomePose();
    if (!home.ok) return home;
    return { ok: true, home };
  }

  function renderHomeButton() {
    const button = $("homeButton");
    if (!button) return;
    const readiness = homeCommandReadiness();
    button.disabled = !readiness.ok;
    button.title = readiness.ok
      ? `发送当前设备校准的 Home 姿态（${HOME_BUTTON_DURATION_MS} ms 平滑）`
      : readiness.message;
  }

  async function sendHomePose() {
    const readiness = homeCommandReadiness();
    if (!readiness.ok) {
      toast("Home 暂不可用", readiness.message, "warning", 6000);
      renderHomeButton();
      return;
    }
    const { home } = readiness;
    if (studio.poseTimer !== null) {
      window.clearTimeout(studio.poseTimer);
      studio.poseTimer = null;
    }
    studio.poseQueued = false;
    studio.poseSending = true;
    studio.poseAngles = home.pulses.map((pulse, index) => (
      pulseToAngleWithCalibration(index, pulse, home.record.servo_calibration)
    ));
    renderPose({ schedule: false, source: "home-button" });
    renderHomeButton();
    setPoseAck("Home 下发中", "pending");
    setCommandBusy(true, "HOME");
    const payload = {
      servos_us: [...home.pulses],
      duration_ms: HOME_BUTTON_DURATION_MS,
      easing: "smooth",
      led: deepCopy(studio.light),
      source: "home-button",
    };
    const started = performance.now();
    try {
      await requestJson(API.pose, { method: "POST", body: JSON.stringify(payload) });
      setPoseAck("Home 命令已确认", "ok");
      $("lastCommand").textContent =
        `HOME · ${payload.servos_us.join("/")} µs · ${Math.round(performance.now() - started)} ms`;
      toast("Home 姿态已下发", `${payload.servos_us.join(" / ")} µs · 500 ms smooth`, "success", 4800);
    } catch (error) {
      setPoseAck("Home 下发失败", "error");
      toast("Home 下发失败", error.message, "error", 7000);
    } finally {
      studio.poseSending = false;
      setCommandBusy(false);
      renderHomeButton();
      queuePoseFlush();
    }
  }

  const average = (values) => values.reduce((sum, value) => sum + value, 0) / Math.max(1, values.length);

  function populationVariance(values) {
    const mean = average(values);
    return average(values.map((value) => (value - mean) ** 2));
  }

  function circularStatistics(values) {
    const radians = values.map((value) => number(value, 0) * Math.PI / 180);
    const meanRadians = Math.atan2(average(radians.map(Math.sin)), average(radians.map(Math.cos)));
    const meanDegrees = wrapDegrees(meanRadians * 180 / Math.PI);
    const residuals = values.map((value) => wrapDegrees(value - meanDegrees));
    return { mean: meanDegrees, variance: average(residuals.map((value) => value ** 2)) };
  }

  function imuCalibrationQuality(samples) {
    const accelMagnitudes = samples.map((sample) => Math.hypot(...sample.accel));
    const gyroMagnitudes = samples.map((sample) => Math.hypot(...sample.gyro));
    const accelVariance = populationVariance(accelMagnitudes);
    const gyroMean = average(gyroMagnitudes);
    const gyroPeak = Math.max(...gyroMagnitudes);
    const gyroAxisVariance = Math.max(...[0, 1, 2].map((axis) => populationVariance(
      samples.map((sample) => sample.gyro[axis]),
    )));
    const roll = circularStatistics(samples.map((sample) => sample.roll));
    const pitch = circularStatistics(samples.map((sample) => sample.pitch));
    const spanMs = samples.length > 1 ? samples[samples.length - 1].timestamp - samples[0].timestamp : 0;
    const checks = {
      enoughSamples: samples.length >= CALIBRATION_MIN_SAMPLES,
      enoughSpan: spanMs >= CALIBRATION_MIN_SPAN_MS,
      accelRange: accelMagnitudes.every((value) => value >= CALIBRATION_ACCEL_MIN_G && value <= CALIBRATION_ACCEL_MAX_G),
      accelVariance: accelVariance <= CALIBRATION_ACCEL_VARIANCE_MAX,
      gyroMean: gyroMean <= CALIBRATION_GYRO_MEAN_MAX_DPS,
      gyroPeak: gyroPeak <= CALIBRATION_GYRO_PEAK_MAX_DPS,
      gyroVariance: gyroAxisVariance <= CALIBRATION_GYRO_AXIS_VARIANCE_MAX,
      rollVariance: roll.variance <= CALIBRATION_ATTITUDE_VARIANCE_MAX,
      pitchVariance: pitch.variance <= CALIBRATION_ATTITUDE_VARIANCE_MAX,
    };
    const pass = Object.values(checks).every(Boolean);
    return {
      pass,
      checks,
      spanMs,
      accelVariance,
      accelStdMg: Math.sqrt(Math.max(0, accelVariance)) * 1000,
      gyroMean,
      gyroPeak,
      gyroAxisVariance,
      roll,
      pitch,
    };
  }

  function renderImuCalibrationRun(run, quality = null) {
    if (!run || studio.deviceCalibration.imuRun !== run) return;
    const samples = run.samples;
    const spanMs = quality?.spanMs ?? (samples.length > 1
      ? samples[samples.length - 1].timestamp - samples[0].timestamp
      : 0);
    const remaining = Math.max(0, CALIBRATION_MIN_SPAN_MS - spanMs) / 1000;
    if ($("calibrationCountdown")) $("calibrationCountdown").textContent = remaining.toFixed(1);
    if ($("calibrationCountdownLabel")) {
      $("calibrationCountdownLabel").textContent = samples.length ? "请保持静置" : "等待新样本";
    }
    const stable = quality?.pass || (samples.length > 0 && !run.lastFailure);
    setCalibrationMetric("calibrationStationaryMetric", stable ? "稳定" : "请保持静止", stable ? 100 : 24);
    setCalibrationMetric(
      "calibrationVibrationMetric",
      quality ? `${quality.accelStdMg.toFixed(1)} mg` : "— mg",
      quality ? 100 * (1 - quality.accelVariance / CALIBRATION_ACCEL_VARIANCE_MAX) : 0,
    );
    setCalibrationMetric(
      "calibrationGyroMetric",
      quality ? `${quality.gyroPeak.toFixed(2)} °/s` : "— °/s",
      quality ? 100 * (1 - quality.gyroPeak / CALIBRATION_GYRO_PEAK_MAX_DPS) : 0,
    );
    setCalibrationMetric(
      "calibrationSampleMetric",
      `${samples.length} / ≥${CALIBRATION_MIN_SAMPLES}`,
      samples.length / CALIBRATION_MIN_SAMPLES * 100,
    );
    setCalibrationMetric(
      "calibrationSpanMetric",
      `${(spanMs / 1000).toFixed(1)} / 3.0 s`,
      spanMs / CALIBRATION_MIN_SPAN_MS * 100,
    );
  }

  function imuSampleValidity(telemetry, run) {
    if (studio.connectionEpoch !== run.clientEpoch) return "设备连接世代已变化。";
    const local = localDeviceIdentity();
    if (!local.online || !local.identityValid || local.chipId !== run.chipId) return "当前设备身份已失效。";
    if (local.connectionEpoch !== run.connectionEpoch) return "connection_epoch 已变化。";
    if (!telemetry.available || !telemetry.fresh || !telemetry.identity_valid) return "IMU 数据离线、过期或身份未确认。";
    if (telemetry.connection_epoch !== run.connectionEpoch || telemetry.current_connection_epoch !== run.connectionEpoch) {
      return "IMU 样本不属于当前 connection_epoch。";
    }
    if (telemetry.filter_epoch !== run.filterEpoch || telemetry.current_filter_epoch !== run.filterEpoch) {
      return "IMU 滤波器已重置，filter_epoch 发生变化。";
    }
    if (telemetry.chip_id !== run.chipId || telemetry.current_chip_id !== run.chipId) {
      return "IMU 样本 chip_id 与当前设备不一致。";
    }
    if (sensorIdentity(telemetry) !== run.sensorId) return "IMU 芯片身份在采集中变化。";
    if (!Number.isInteger(Number(telemetry.sample_seq)) || !Number.isFinite(Number(telemetry.sampled_at))) {
      return "IMU 样本缺少 sample_seq 或 timestamp_ms。";
    }
    return "";
  }

  function ingestImuCalibrationSample(telemetry) {
    const run = studio.deviceCalibration.imuRun;
    if (!run || run.cancelled) return;
    const fatal = imuSampleValidity(telemetry, run);
    if (fatal) {
      cancelImuCalibration(fatal);
      return;
    }
    const sequence = integer(telemetry.sample_seq, -1);
    const timestamp = number(telemetry.sampled_at, NaN);
    const key = `${run.connectionEpoch}:${run.filterEpoch}:${sequence}:${timestamp}`;
    if (run.seen.has(key)) return;
    if (sequence <= run.lastSequence || timestamp <= run.lastTimestamp) {
      cancelImuCalibration("IMU 样本序列回退，已拒绝混用旧样本。");
      return;
    }
    run.seen.add(key);
    run.lastSequence = sequence;
    run.lastTimestamp = timestamp;
    const accel = telemetry.raw?.accel_g;
    const gyro = telemetry.raw?.gyro_dps;
    // Calibration deliberately samples the unchanged sensor-frame attitude.
    // Its persisted baseline is converted with the same board transform as
    // live telemetry, preserving v1 records and avoiding double compensation.
    const attitude = telemetry.attitude;
    if (!Array.isArray(accel) || accel.length !== 3 || accel.some((value) => !Number.isFinite(value)) ||
        !Array.isArray(gyro) || gyro.length !== 3 || gyro.some((value) => !Number.isFinite(value)) ||
        !attitude || !Number.isFinite(attitude.roll_deg) || !Number.isFinite(attitude.pitch_deg)) {
      cancelImuCalibration("IMU 样本缺少完整的加速度、角速度或姿态数据。");
      return;
    }
    const accelMagnitude = Math.hypot(...accel);
    const gyroMagnitude = Math.hypot(...gyro);
    if (accelMagnitude < CALIBRATION_ACCEL_MIN_G || accelMagnitude > CALIBRATION_ACCEL_MAX_G ||
        gyroMagnitude > CALIBRATION_GYRO_PEAK_MAX_DPS) {
      run.samples = [];
      run.lastFailure = accelMagnitude < CALIBRATION_ACCEL_MIN_G || accelMagnitude > CALIBRATION_ACCEL_MAX_G
        ? `|a|=${accelMagnitude.toFixed(3)} g 超出 0.80–1.20 g`
        : `|ω|=${gyroMagnitude.toFixed(2)} °/s 超出 5 °/s`;
      renderImuCalibrationRun(run);
      return;
    }
    run.lastFailure = "";
    run.samples.push({
      sequence,
      timestamp,
      accel: [...accel],
      gyro: [...gyro],
      roll: attitude.roll_deg,
      pitch: attitude.pitch_deg,
    });
    while (run.samples.length > 1 && timestamp - run.samples[0].timestamp > 4000) run.samples.shift();
    const quality = imuCalibrationQuality(run.samples);
    run.lastQuality = quality;
    renderImuCalibrationRun(run, quality);
    if (quality.pass) completeImuCalibration(run, quality);
  }

  async function completeImuCalibration(run, quality) {
    const calibration = studio.deviceCalibration;
    if (calibration.imuRun !== run || run.cancelled) return;
    window.clearInterval(run.timer);
    calibration.imuRun = null;
    try {
      const snapshot = await refreshDeviceCalibration({ syncUi: false, quiet: true });
      const identity = calibrationIdentityCheck(snapshot);
      if (!identity.ok) throw new Error(identity.message);
      const epochs = calibrationEpochs(snapshot);
      if (studio.connectionEpoch !== run.clientEpoch || epochs.connection !== run.connectionEpoch ||
          epochs.filter !== run.filterEpoch || epochs.chipId !== run.chipId) {
        throw new Error("保存前 connection/filter epoch 或 chip_id 已变化。");
      }
      if (!calibrationMotionStopped()) throw new Error("保存前运动状态发生变化。");
      const baseline = {
        roll: Number(quality.roll.mean.toFixed(6)),
        pitch: Number(quality.pitch.mean.toFixed(6)),
      };
      const record = coreCalibrationRecord(snapshot, snapshot.record.servo_calibration, {
        imu_mounting_baseline_deg: baseline,
      });
      const result = await saveCalibrationRecord(record);
      const savedEpochs = calibrationEpochs(result);
      if (savedEpochs.connection !== run.connectionEpoch || savedEpochs.filter !== run.filterEpoch ||
          savedEpochs.chipId !== run.chipId) {
        const rollback = coreCalibrationRecord(result, result.record.servo_calibration, {
          imu_mounting_baseline_deg: null,
        });
        await saveCalibrationRecord(rollback);
        throw new Error("保存时 IMU filter_epoch 发生变化，已撤销本次水平基准。");
      }
      studio.imuReference.sensorLevelBaseline = { ...baseline };
      renderImuTelemetry();
      renderCalibrationWizard();
      toast(
        "IMU Roll / Pitch 水平基准已保存",
        `传感器基准 Roll ${baseline.roll.toFixed(2)}° · Pitch ${baseline.pitch.toFixed(2)}° · ${run.samples.length} 帧 / ${(quality.spanMs / 1000).toFixed(1)} s；机身显示会再应用板型安装变换，Yaw 未持久化。`,
        "success",
        8000,
      );
    } catch (error) {
      renderCalibrationWizard();
      toast("IMU 水平基准未保存", error.message || String(error), "error", 8000);
    }
  }

  async function startImuCalibration() {
    const calibration = studio.deviceCalibration;
    if (calibration.imuRun) return;
    try {
      const snapshot = await refreshDeviceCalibration({ syncUi: false, quiet: true });
      const identity = calibrationIdentityCheck(snapshot);
      if (!identity.ok) throw new Error(identity.message);
      if (!calibration.safetyConfirmed) throw new Error("请先确认机身已架空并清场。");
      if (!snapshot.completed_steps?.servo || !snapshot.completed_steps?.home) {
        throw new Error("必须先完成舵机与 Home 两步校准。");
      }
      if (!calibrationMotionStopped()) throw new Error("必须锁定舵机并停止播放、预览和录制。");
      const epochs = calibrationEpochs(snapshot);
      const telemetry = studio.imu;
      const sensorId = sensorIdentity(telemetry);
      const probe = {
        clientEpoch: studio.connectionEpoch,
        connectionEpoch: epochs.connection,
        filterEpoch: epochs.filter,
        chipId: epochs.chipId,
        sensorId,
      };
      const invalid = imuSampleValidity(telemetry, probe);
      if (invalid) throw new Error(invalid);
      const run = {
        ...probe,
        samples: [],
        seen: new Set(),
        lastSequence: -1,
        lastTimestamp: -Infinity,
        lastFailure: "",
        lastQuality: null,
        startedAt: performance.now(),
        deadline: performance.now() + CALIBRATION_TIMEOUT_MS,
        timer: null,
        cancelled: false,
      };
      calibration.imuRun = run;
      run.timer = window.setInterval(() => {
        if (calibration.imuRun !== run || run.cancelled) return;
        if (studio.connectionEpoch !== run.clientEpoch) {
          cancelImuCalibration("设备连接世代已变化。");
          return;
        }
        if (performance.now() >= run.deadline) {
          const quality = run.lastQuality;
          const detail = run.lastFailure || (quality
            ? `质量未达标：振动 ${quality.accelStdMg.toFixed(1)} mg，角速率峰值 ${quality.gyroPeak.toFixed(2)} °/s，Roll/Pitch 方差 ${quality.roll.variance.toFixed(3)}/${quality.pitch.variance.toFixed(3)} deg²。`
            : "8 秒内未收到足够的唯一新样本。");
          cancelImuCalibration(detail);
          return;
        }
        renderImuCalibrationRun(run, run.lastQuality);
      }, 100);
      renderCalibrationWizard();
      renderImuCalibrationRun(run);
      ingestImuCalibrationSample(telemetry);
    } catch (error) {
      toast("IMU 水平标定无法开始", error.message || String(error), "error", 7500);
    }
  }

  async function initializeDogViewer() {
    const mount = $("dogViewerMount");
    if (!mount || studio.dogViewer || studio.dogViewerDisposed) return;
    try {
      const viewerModule = await import("/assets/dog-viewer.js");
      if (studio.dogViewerDisposed) return;
      if (typeof viewerModule.mountDogViewer !== "function") {
        throw new TypeError("dog-viewer.js 未导出 mountDogViewer");
      }
      studio.dogViewer = viewerModule.mountDogViewer(mount, {
        modelUrl: "/assets/models/unitree-go1-animated.glb",
        centerUs: studio.project.calibration.map((item) => item.center_us),
        minUs: SERVO_ELECTRICAL_MIN,
        maxUs: SERVO_ELECTRICAL_MAX,
        initialState: dogViewerState(),
        onReady: () => {
          mount.classList.add("viewer-ready");
          mount.classList.remove("viewer-fallback");
        },
        onFallback: () => {
          mount.classList.add("viewer-fallback");
          mount.classList.remove("viewer-ready");
        },
      });
      syncDogViewer();
    } catch (error) {
      mount.classList.add("viewer-fallback");
      console.warn("3D posture preview unavailable; keeping 2D fallback", error);
    }
  }

  function currentMotionParameters(source = "manual") {
    if (source === "manual-live" || source === "recording-live") {
      return { duration_ms: LIVE_DRAG_DURATION_MS, easing: "linear" };
    }
    if (source === "keyboard-home" || source === "home-button") {
      return { duration_ms: HOME_BUTTON_DURATION_MS, easing: "smooth" };
    }
    const speed = clamp(number($("speedRange").value, 100) / 100, .25, 2);
    const easingUi = $("easingSelect").value;
    const rawDuration = clamp(integer($("transitionInput").value, 600), 40, 10000);
    return {
      duration_ms: easingUi === "step" ? 0 : Math.round(rawDuration / speed),
      easing: EASING_TO_WIRE[easingUi] || "smooth",
    };
  }

  function uiEasing(wireValue, duration = 1) {
    if (duration === 0) return "step";
    if (wireValue === "linear" || wireValue === "ease-in" || wireValue === "ease-out") return wireValue;
    return "ease-in-out";
  }

  function renderPose({ schedule = false, source = "manual" } = {}) {
    const pulses = currentServosUs();
    studio.poseAngles.forEach((angle, index) => {
      $( `leg${index}Range` ).value = String(Math.round(angle));
      $( `leg${index}Angle` ).textContent = `${Math.round(angle)}°`;
      $( `leg${index}Pulse` ).textContent = `${pulses[index]} µs`;
      $( `diagramAngle${index}` ).textContent = `${Math.round(angle)}°`;
    });
    $("poseSummary").textContent = studio.poseAngles.map((angle) => `${Math.round(angle)}°`).join(" · ");
    syncDogViewer();
    if (schedule) schedulePose(source);
  }

  function renderCalibration() {
    studio.project.calibration.forEach((calibration, index) => {
      $( `cal${index}Min` ).value = calibration.min_us;
      $( `cal${index}Center` ).value = calibration.center_us;
      $( `cal${index}Max` ).value = calibration.max_us;
      $( `leg${index}Reverse` ).checked = calibration.reverse;
      $( `leg${index}Trim` ).value = calibration.trim_deg;
    });
  }

  function readCalibrationFromUi({ showErrors = true } = {}) {
    let valid = true;
    const next = LEGS.map((leg, index) => {
      const minInput = $( `cal${index}Min` );
      const centerInput = $( `cal${index}Center` );
      const maxInput = $( `cal${index}Max` );
      const minUs = integer(minInput.value, SAFE_MIN);
      const centerUs = integer(centerInput.value, SAFE_CENTER);
      const maxUs = integer(maxInput.value, SAFE_MAX);
      const rowValid = (
        minUs >= SERVO_ELECTRICAL_MIN &&
        maxUs <= SERVO_ELECTRICAL_MAX &&
        minUs < centerUs &&
        centerUs < maxUs
      );
      [minInput, centerInput, maxInput].forEach((input) => input.classList.toggle("invalid", !rowValid));
      if (!rowValid) valid = false;
      return {
        min_us: minUs,
        center_us: centerUs,
        max_us: maxUs,
        reverse: $( `leg${index}Reverse` ).checked,
        trim_deg: clamp(integer($( `leg${index}Trim` ).value, 0), -30, 30),
      };
    });
    if (!valid) {
      if (showErrors) toast("校准未保存", "每路必须满足 500 ≤ MIN < CENTER < MAX ≤ 2500。", "error");
      return null;
    }
    return next;
  }

  function applyCalibrationFromUi() {
    const calibration = readCalibrationFromUi();
    if (!calibration) return;
    studio.project.calibration = calibration;
    studio.deviceCalibration.uiDirty = true;
    studio.project.actions.forEach((action) => {
      action.keyframes.forEach((frame) => {
        frame.servos_us = frame.servos_us.map((pulse, index) => clamp(
          integer(pulse, calibration[index].center_us),
          calibration[index].min_us,
          calibration[index].max_us,
        ));
      });
    });
    markDirty("安全校准已修改");
    studio.project.actions.forEach((action) => queueActionRemoteSave(action));
    const canSend = Boolean(studio.armed && studio.deviceCalibration.safetyConfirmed);
    renderPose({ schedule: canSend, source: "calibration" });
    if (studio.armed && !canSend) {
      toast("校准参数仅预览", "请先在三步校准向导中确认机身已架空并清场。", "warning");
    }
    renderTimeline();
    renderCalibrationWizard();
  }

  function renderLight({ schedule = false } = {}) {
    if (schedule && studio.ledCalibration.testOverrideActive) {
      studio.ledCalibration.testOverrideActive = false;
      studio.ledCalibration.testLabel = "";
      renderLedCalibration();
    }
    studio.light.color = normalizeHex(studio.light.color);
    studio.light.brightness = clamp(integer(studio.light.brightness, 40), 0, 100);
    studio.light.pixel_count = clamp(integer(studio.light.pixel_count, DEFAULT_PIXELS), 1, MAX_PIXELS);
    $("lightEnabled").checked = studio.light.enabled;
    $("lightPowerText").textContent = studio.light.enabled ? "ON" : "OFF";
    $("lightColor").value = studio.light.color.toLowerCase();
    $("lightHex").value = studio.light.color;
    $("colorSwatch").style.background = studio.light.color;
    $("brightnessRange").value = studio.light.brightness;
    $("brightnessValue").textContent = `${studio.light.brightness}%`;
    $("pixelCount").value = studio.light.pixel_count;
    const preview = $("lightPreview");
    preview.classList.toggle("active", studio.light.enabled && studio.light.brightness > 0);
    preview.style.setProperty("--pixel-color", studio.light.color);
    preview.style.setProperty("--pixel-brightness", String(studio.light.brightness / 100));
    const visiblePixels = Math.min(studio.light.pixel_count, 24);
    $("lightPixels").replaceChildren(...Array.from({ length: visiblePixels }, () => document.createElement("i")));
    $("lightPreviewLabel").textContent = `${studio.light.color} · ${studio.light.brightness}% · ${studio.light.pixel_count} PIXELS`;
    syncDogViewer();
    if (schedule) scheduleLight();
  }

  function markDirty(reason = "工程有未保存修改") {
    studio.dirty = true;
    studio.project.updated_at = new Date().toISOString();
    $("projectSaveState").textContent = "未保存";
    $("projectSaveState").classList.add("dirty");
    window.clearTimeout(studio.saveTimer);
    studio.saveTimer = window.setTimeout(persistLocalProject, 180);
    window.clearTimeout(studio.remoteSaveTimer);
    studio.remoteSaveTimer = window.setTimeout(() => saveProjectRemote({ quiet: true }), 900);
    if (reason) $("recordingHint").textContent = reason;
  }

  function persistLocalProject() {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(studio.project));
    } catch (error) {
      toast("本地保存失败", error.message || String(error), "error");
    }
  }

  function loadLocalProject() {
    const text = localStorage.getItem(STORAGE_KEY);
    if (!text) return false;
    try {
      studio.project = normalizeProject(JSON.parse(text));
      studio.activeActionId = studio.project.actions[0].id;
      return true;
    } catch (error) {
      console.warn("Ignoring invalid local project", error);
      return false;
    }
  }

  async function requestJson(url, options = {}) {
    const response = await fetch(url, {
      cache: "no-store",
      ...options,
      headers: {
        Accept: "application/json",
        ...(options.body ? { "Content-Type": "application/json" } : {}),
        ...(options.headers || {}),
      },
    });
    let payload = {};
    const text = await response.text();
    if (text) {
      try {
        payload = JSON.parse(text);
      } catch {
        throw new Error(`服务返回了无效 JSON（HTTP ${response.status}）`);
      }
    }
    if (!response.ok || payload.ok === false) {
      throw new Error(payload.error || `请求失败（HTTP ${response.status}）`);
    }
    if (payload.state) applySnapshot(payload.state);
    return payload;
  }

  async function getState() {
    try {
      const response = await requestJson(API.state);
      applySnapshot(response.state || response.result || response);
      return true;
    } catch (primaryError) {
      try {
        const response = await requestJson(API.statusCompat);
        applySnapshot(response.state || response.result || response);
        return true;
      } catch (compatError) {
        setServerOffline(compatError.message || primaryError.message);
        return false;
      }
    }
  }

  function unwrapProject(payload) {
    return payload?.state?.project || payload?.result?.project || payload?.project || payload?.result || null;
  }

  async function loadRemoteProject() {
    try {
      const payload = await requestJson(API.project);
      const remote = unwrapProject(payload);
      if (!remote) return;
      if (remote.format === PROJECT_SCHEMA && Array.isArray(remote.actions) && remote.actions.length === 0) {
        // A fresh backend intentionally starts with no actions. Seed the full
        // commercial preset library before the user can arm or press Play.
        await saveProjectRemote({ quiet: true, force: true });
        studio.projectInitialized = true;
        return;
      }
      const normalized = remote.format === PROJECT_SCHEMA
        ? fromBackendProject(remote, studio.project)
        : normalizeProject(remote);
      const remoteTime = Date.parse(normalized.updated_at) || 0;
      const localTime = Date.parse(studio.project.updated_at) || 0;
      if (!studio.dirty && (!studio.projectInitialized || remoteTime >= localTime)) {
        studio.project = normalized;
        studio.activeActionId = normalized.actions[0].id;
        studio.selectedFrameId = null;
        renderProject();
      }
      studio.projectInitialized = true;
    } catch (error) {
      console.info("Remote project is not available yet", error);
    }
  }

  async function saveProjectRemote({ quiet = false, force = false } = {}) {
    persistLocalProject();
    if (!studio.serverSeen) return false;
    if (!force && (studio.armed || studio.playback?.state !== "stopped")) {
      $("projectSaveState").textContent = "锁定后同步";
      if (!quiet) toast("工程已保存到本浏览器", "锁定舵机并停止播放后，工程资料才能同步到主机工程文件。", "warning");
      return false;
    }
    try {
      const backendProject = toBackendProject(studio.project);
      await requestJson(API.project, {
        method: "PUT",
        body: JSON.stringify(backendProject),
      });
      studio.dirty = false;
      $("projectSaveState").textContent = "已保存";
      $("projectSaveState").classList.remove("dirty");
      if (!quiet) toast("工程已保存", "已写入本浏览器并同步到主机工程文件。", "success");
      return true;
    } catch (error) {
      $("projectSaveState").textContent = "仅本机";
      if (!quiet) toast("仅保存到本机", error.message, "warning");
      return false;
    }
  }

  async function saveActionRemote(action, { create = false, quiet = true } = {}) {
    if (!studio.serverSeen || !action) return;
    try {
      const payload = toBackendAction(action);
      const path = create ? API.actions : `${API.actions}/${encodeURIComponent(action.id)}`;
      await requestJson(path, {
        method: create ? "POST" : "PUT",
        body: JSON.stringify(payload),
      });
      if (!quiet) toast("动作已同步", action.name, "success");
    } catch (error) {
      $("projectSaveState").textContent = "动作未同步";
      if (!quiet) toast("动作同步失败", error.message, "error");
    }
  }

  function queueActionRemoteSave(action, delay = 450) {
    if (!action) return;
    const oldTimer = studio.actionSaveTimers.get(action.id);
    window.clearTimeout(oldTimer);
    const timer = window.setTimeout(() => {
      studio.actionSaveTimers.delete(action.id);
      saveActionRemote(action);
    }, delay);
    studio.actionSaveTimers.set(action.id, timer);
  }

  function extractOnline(snapshot) {
    if (!snapshot || typeof snapshot !== "object") return false;
    if (typeof snapshot.online === "boolean") return snapshot.online;
    if (typeof snapshot.running === "boolean") return snapshot.running && !snapshot.error;
    return Boolean(snapshot.transport_id != null && !snapshot.error);
  }

  function extractPower(snapshot) {
    const source = snapshot?.power || snapshot?.device?.power || null;
    if (!source || typeof source !== "object") return null;
    const millivolt = integer(source.millivolt ?? source.servo_millivolt ?? source.vbat_mv, 0);
    const explicit = source.servo_power_present ?? source.present;
    const present = typeof explicit === "boolean" ? explicit : millivolt >= 3000;
    return {
      present,
      millivolt,
      label: safeText(source.battery_label || source.status || source.battery, present ? "电源已检测" : "未检测到舵机电源", 48),
    };
  }

  function applySnapshot(snapshot) {
    if (!snapshot || typeof snapshot !== "object") return;
    studio.snapshot = snapshot;
    studio.serverSeen = true;
    studio.online = extractOnline(snapshot);
    studio.armed = Boolean(snapshot.armed ?? snapshot.servo_armed);
    studio.estop = Boolean(snapshot.estop ?? snapshot.emergency_stopped);
    studio.power = extractPower(snapshot);
    studio.powerOverrideAllowed = Boolean(snapshot.power_override_allowed);
    if (snapshot.imu && typeof snapshot.imu === "object") applyImuTelemetry(snapshot.imu);

    const recording = snapshot.recording;
    if (recording && typeof recording === "object") {
      studio.recordingInfo = {
        ...studio.recordingInfo,
        ...deepCopy(recording),
      };
      studio.recording = Boolean(recording.active ?? recording.state === "recording");
    } else {
      studio.recording = Boolean(recording);
      studio.recordingInfo = {
        ...studio.recordingInfo,
        state: studio.recording ? "recording" : "idle",
        active: studio.recording,
      };
    }
    if (snapshot.playback && typeof snapshot.playback === "object") {
      studio.playback = {
        ...studio.playback,
        ...snapshot.playback,
      };
      if (Number.isFinite(Number(snapshot.playback.position_ms))) {
        studio.playheadMs = Math.max(0, Number(snapshot.playback.position_ms));
      }
    }
    updateConnectionCalibrationEpoch(snapshot);

    if (
      snapshot.project &&
      !studio.dirty &&
      !studio.recording &&
      (!Array.isArray(snapshot.project.actions) || snapshot.project.actions.length > 0)
    ) {
      try {
        const incoming = snapshot.project.format === PROJECT_SCHEMA
          ? fromBackendProject(snapshot.project, studio.project)
          : normalizeProject(snapshot.project);
        const previousAction = studio.activeActionId;
        studio.project = incoming;
        studio.activeActionId = incoming.actions.some((action) => action.id === previousAction)
          ? previousAction
          : incoming.actions[0].id;
        if (studio.deviceCalibration.record) {
          syncProjectCalibrationFromRecord(studio.deviceCalibration.record);
        } else {
          studio.project.calibration = defaultCalibration();
          renderCalibration();
          renderPose();
        }
      } catch (error) {
        console.warn("Ignoring invalid project snapshot", error);
      }
    }

    renderDeviceState();
    renderArmState();
    renderRecordingState();
    renderPlaybackState();
    renderProjectControls();
    renderTimeline();
    renderCalibrationWizard();

    const confirmed = snapshot.confirmed_pose || snapshot.confirmed;
    const deviceLed = confirmed?.led || snapshot.target_pose?.led;
    if (
      deviceLed &&
      !studio.lightSending &&
      !studio.lightQueued &&
      !studio.ledCalibration.testOverrideActive
    ) {
      try {
        const nextLight = normalizeLed(deviceLed, [], "device LED", false);
        if (JSON.stringify(nextLight) !== JSON.stringify(studio.light)) {
          studio.light = nextLight;
          renderLight();
        }
      } catch (error) {
        console.warn("Ignoring invalid device light snapshot", error);
      }
    }
    const hasDeviceConfirmation = Boolean(
      studio.online &&
      confirmed?.servos_us?.length === LEGS.length &&
      (number(confirmed.seq, 0) > 0 || confirmed.confirmed_at)
    );
    if (hasDeviceConfirmation) {
      const target = currentServosUs();
      const matches = target.every((pulse, index) => Math.abs(pulse - number(confirmed.servos_us[index], 0)) <= 2);
      setPoseAck(matches ? "命令已确认" : "设备状态已更新", matches ? "ok" : "pending");
    } else if (!studio.armed) {
      setPoseAck("预览模式", "pending");
    }
  }

  function setServerOffline(message) {
    studio.serverSeen = false;
    studio.online = false;
    studio.armed = false;
    studio.snapshot = { ...(studio.snapshot || {}), online: false, identity_valid: false };
    studio.imu = { ...studio.imu, fresh: false, error: message || "上位机连接中断" };
    updateConnectionCalibrationEpoch(studio.snapshot);
    $("connectionBanner").classList.add("show", "error");
    $("connectionMessage").textContent = `上位机连接中断：${message || "正在重试"}`;
    renderDeviceState();
    renderArmState();
    renderImuTelemetry();
  }

  function renderDeviceState() {
    const snapshot = studio.snapshot || {};
    const connectionDot = $("connectionDot");
    connectionDot.classList.toggle("online", studio.online);
    connectionDot.classList.toggle("degraded", studio.serverSeen && !studio.online);
    const board = snapshot.board?.board || snapshot.board || "ARCS-MINI V2.1";
    const chipId = snapshot.chip_id || snapshot.device_id || "";
    $("deviceName").textContent = studio.online ? board : "等待设备";
    $("deviceMeta").textContent = studio.online
      ? `ADB t${snapshot.transport_id ?? "?"}${chipId ? ` · ${chipId}` : ""}`
      : "ADB transport —";
    const latency = snapshot.latency_ms == null ? -1 : number(snapshot.latency_ms, -1);
    $("latencyValue").textContent = latency >= 0 ? `${Math.round(latency)} ms` : "—";
    $("linkValue").textContent = studio.online ? "ONLINE" : studio.serverSeen ? "NO DEVICE" : "OFFLINE";
    $("linkValue").style.color = studio.online ? "var(--green)" : "";
    $("firmwareDot").classList.toggle("online", studio.online);
    const firmware = snapshot.firmware || snapshot.version || "";
    $("firmwareText").textContent = studio.online ? `固件在线${firmware ? ` · ${firmware}` : ""}` : "固件未连接";
    const limits = snapshot.servo_limits;
    $("servoLimits").textContent = limits
      ? `PWM ${limits.hard_min_us ?? SERVO_ELECTRICAL_MIN}–${limits.hard_max_us ?? SERVO_ELECTRICAL_MAX} µs · ${limits.frequency_hz ?? 50} Hz`
      : "PWM 500–2500 µs · 50 Hz";
    if (studio.online) {
      $("connectionBanner").classList.remove("show", "error");
    } else if (studio.serverSeen) {
      $("connectionBanner").classList.add("show");
      $("connectionBanner").classList.remove("error");
      $("connectionMessage").textContent = "上位机已启动，等待开发板进入 ADB…";
    } else {
      $("connectionBanner").classList.add("show", "error");
      $("connectionMessage").textContent = "无法连接本地运动控制服务，正在自动重试…";
    }
    syncDogViewer();
  }

  function renderArmState() {
    const button = $("armButton");
    button.classList.toggle("armed", studio.armed);
    button.setAttribute("aria-pressed", String(studio.armed));
    button.disabled = !studio.serverSeen;
    $("armText").textContent = studio.armed ? "ARMED" : "LOCKED";
    document.body.classList.toggle("output-armed", studio.armed);
    if (!studio.armed && studio.poseSending) studio.poseQueued = false;
    updatePowerInterlock();
    syncDogViewer();
    renderCalibrationWizard();
    renderHomeButton();
  }

  function updatePowerInterlock() {
    const interlock = $("powerInterlock");
    interlock.classList.remove("checking", "ok", "blocked");
    const holdLabel = $("holdToArm").querySelector("span");
    if (!studio.serverSeen || !studio.online || !studio.power) {
      interlock.classList.add("blocked");
      $("powerInterlockTitle").textContent = "无法确认舵机电源";
      $("powerInterlockDetail").textContent = "设备离线或板端未上报 VBAT / servo power，禁止解锁。";
      $("holdToArm").disabled = true;
      holdLabel.textContent = "按住 1.2 秒解锁";
      return;
    }
    if (!studio.power.present) {
      if (studio.powerOverrideAllowed) {
        interlock.classList.add("override");
        $("powerInterlockTitle").textContent = "外部舵机电源人工确认模式";
        $("powerInterlockDetail").textContent = "VBAT 未检测到电压。仅当舵机确已使用独立电源并与主板共地时，才长按确认解锁。";
        $("holdToArm").disabled = false;
        holdLabel.textContent = "长按确认外部电源并解锁";
        return;
      }
      interlock.classList.add("blocked");
      $("powerInterlockTitle").textContent = "未检测到舵机电源";
      $("powerInterlockDetail").textContent = `${studio.power.label}${studio.power.millivolt ? ` · ${studio.power.millivolt} mV` : ""}，请检查独立电源与共地。`;
      $("holdToArm").disabled = true;
      holdLabel.textContent = "按住 1.2 秒解锁";
      return;
    }
    interlock.classList.add("ok");
    $("powerInterlockTitle").textContent = "舵机电源已检测";
    $("powerInterlockDetail").textContent = `${studio.power.label}${studio.power.millivolt ? ` · ${studio.power.millivolt} mV` : ""}`;
    $("holdToArm").disabled = false;
    holdLabel.textContent = "按住 1.2 秒解锁";
  }

  function setPoseAck(text, kind = "pending") {
    const node = $("poseAck");
    node.textContent = text;
    node.className = `ack-badge ${kind === "ok" ? "" : kind}`.trim();
  }

  function setCommandBusy(busy, label = "POSE") {
    $("commandQueue").textContent = busy ? `COMMAND QUEUE · ${label}` : "COMMAND QUEUE · IDLE";
    $("commandQueue").classList.toggle("busy", busy);
  }

  function queuePoseFlush() {
    if (
      studio.poseTimer !== null ||
      studio.poseSending ||
      studio.frameTestSending ||
      !studio.poseQueued ||
      !studio.armed ||
      !studio.online
    ) return;
    const elapsed = performance.now() - studio.lastPoseFlushAt;
    const delay = Math.max(0, POSE_COALESCE_MS - elapsed);
    studio.poseTimer = window.setTimeout(() => {
      studio.poseTimer = null;
      flushPose();
    }, delay);
  }

  function schedulePose(source = "manual") {
    setPoseAck(studio.armed ? "等待固件确认" : "预览模式", "pending");
    if (studio.frameTestSending) {
      setPoseAck("真机单步进行中", "pending");
      return;
    }
    if (!studio.armed || !studio.online) return;
    studio.poseQueued = true;
    studio.pendingPoseSource = source;
    queuePoseFlush();
  }

  async function flushPose() {
    if (studio.poseSending || studio.frameTestSending || !studio.poseQueued || !studio.armed || !studio.online) return;
    studio.poseQueued = false;
    studio.poseSending = true;
    studio.lastPoseFlushAt = performance.now();
    setCommandBusy(true, "POSE");
    const source = studio.pendingPoseSource || "manual";
    const motion = currentMotionParameters(source);
    const payload = {
      servos_us: currentServosUs(),
      duration_ms: motion.duration_ms,
      easing: motion.easing,
      led: deepCopy(studio.light),
      source,
    };
    const started = performance.now();
    try {
      await requestJson(API.pose, { method: "POST", body: JSON.stringify(payload) });
      setPoseAck("命令已确认", "ok");
      $("lastCommand").textContent = `POSE · ${payload.servos_us.join("/")} µs · ${Math.round(performance.now() - started)} ms`;
    } catch (error) {
      setPoseAck("下发失败", "error");
      toast("姿态下发失败", error.message, "error");
    } finally {
      studio.poseSending = false;
      setCommandBusy(false);
      renderHomeButton();
      queuePoseFlush();
    }
  }

  function scheduleLight() {
    if (!studio.online) return;
    studio.lightQueued = true;
    window.clearTimeout(studio.lightTimer);
    studio.lightTimer = window.setTimeout(flushLight, LIGHT_COALESCE_MS);
  }

  async function flushLight() {
    if (studio.lightSending || !studio.lightQueued || !studio.online) return;
    studio.lightQueued = false;
    studio.lightSending = true;
    setCommandBusy(true, "LIGHT");
    try {
      await requestJson(API.light, {
        method: "POST",
        body: JSON.stringify(studio.light),
      });
      $("lastCommand").textContent = `LIGHT · ${studio.light.enabled ? studio.light.color : "OFF"} · ${studio.light.brightness}% · ${studio.light.pixel_count}px`;
    } catch (error) {
      toast("灯光下发失败", error.message, "error");
    } finally {
      studio.lightSending = false;
      setCommandBusy(false);
      if (studio.lightQueued) studio.lightTimer = window.setTimeout(flushLight, LIGHT_COALESCE_MS);
    }
  }

  async function setArmed(armed) {
    const usingPowerOverride = Boolean(
      armed &&
      studio.online &&
      studio.power &&
      !studio.power.present &&
      studio.powerOverrideAllowed
    );
    if (armed && (!studio.online || (!studio.power?.present && !usingPowerOverride))) {
      updatePowerInterlock();
      toast("安全联锁未通过", "未检测到有效舵机电源，不能开启输出。", "error");
      return;
    }
    try {
      const payload = { armed: Boolean(armed) };
      if (usingPowerOverride) payload.external_power_confirmed = true;
      await requestJson(API.arm, { method: "POST", body: JSON.stringify(payload) });
      studio.armed = Boolean(armed);
      studio.estop = false;
      renderArmState();
      if (armed) {
        $("armDialog").close();
        toast("舵机输出已开启", "解锁不会自动下发当前姿态；请在确认目标后再主动操作。", "success");
      } else {
        toast("舵机输出已锁定", "四路 PWM 输出已关闭。", "success");
        if (studio.dirty) saveProjectRemote({ quiet: true });
      }
    } catch (error) {
      toast(armed ? "解锁失败" : "锁定失败", error.message, "error");
    }
  }

  async function emergencyStop({ quiet = false } = {}) {
    cancelHoldToArm();
    cancelSafePostureRoutine();
    stopOfflinePreview({ keepPosition: true });
    studio.armed = false;
    studio.estop = true;
    const recordingWasActive = studio.recording;
    studio.recording = false;
    if (recordingWasActive) {
      studio.recordingInfo = {
        ...studio.recordingInfo,
        state: "idle",
        active: false,
        interrupted: true,
      };
    }
    studio.playback = { ...studio.playback, state: "stopped", position_ms: studio.playheadMs };
    studio.poseQueued = false;
    studio.lightQueued = false;
    renderArmState();
    renderRecordingState();
    renderPlaybackState();
    setPoseAck("急停 · 输出关闭", "error");
    try {
      await requestJson(API.estop, { method: "POST", body: "{}" });
      $("lastCommand").textContent = "EMERGENCY STOP · ALL OUTPUTS OFF";
      if (!quiet) toast("急停已执行", "所有舵机输出与动作播放已停止。", "error", 5000);
    } catch (error) {
      if (!quiet) toast("急停请求未确认", `${error.message}；请立即断开舵机电源。`, "error", 7000);
    }
  }

  function startHoldToArm() {
    if ($("holdToArm").disabled || studio.holdTimer) return;
    $("holdToArm").classList.add("holding");
    studio.holdTimer = window.setTimeout(() => {
      studio.holdTimer = null;
      $("holdToArm").classList.remove("holding");
      setArmed(true);
    }, HOLD_TO_ARM_MS);
  }

  function cancelHoldToArm() {
    window.clearTimeout(studio.holdTimer);
    studio.holdTimer = null;
    $("holdToArm").classList.remove("holding");
  }

  function renderProjectControls() {
    $("projectName").value = studio.project.name;
    const select = $("actionSelect");
    const previousValue = studio.activeActionId;
    select.replaceChildren(...studio.project.actions.map((action) => {
      const option = document.createElement("option");
      option.value = action.id;
      option.textContent = action.name;
      return option;
    }));
    studio.activeActionId = studio.project.actions.some((action) => action.id === previousValue)
      ? previousValue
      : studio.project.actions[0].id;
    select.value = studio.activeActionId;
    const action = activeAction();
    $("loopToggle").checked = Boolean(action?.loop);
  }

  function renderProject() {
    renderProjectControls();
    renderCalibration();
    renderPose();
    renderLight();
    renderTimeline();
  }

  function actionDuration(action = activeAction()) {
    if (!action) return 4000;
    const last = action.keyframes.reduce(
      (maximum, frame) => Math.max(maximum, frame.time_ms + Math.max(20, frame.duration_ms)),
      0,
    );
    return clamp(Math.max(1000, action.duration_ms || 0, last), 1000, MAX_PROJECT_DURATION_MS);
  }

  function easingProgress(easing, value) {
    const x = clamp(number(value, 0), 0, 1);
    if (easing === "smooth") return x * x * (3 - 2 * x);
    if (easing === "ease-in") return x * x;
    if (easing === "ease-out") return x * (2 - x);
    return x;
  }

  function sampleSegment(segment, timeMs) {
    if (!segment || segment.duration_ms <= 0 || timeMs >= segment.time_ms + segment.duration_ms) {
      return segment ? [...segment.target] : [SAFE_CENTER, SAFE_CENTER, SAFE_CENTER, SAFE_CENTER];
    }
    if (timeMs <= segment.time_ms) return [...segment.start];
    const progress = easingProgress(segment.easing, (timeMs - segment.time_ms) / segment.duration_ms);
    return segment.start.map((value, index) => Math.round(
      value + (segment.target[index] - value) * progress,
    ));
  }

  function sampleAction(action, timeMs) {
    const frames = [...(action?.keyframes || [])].sort((left, right) => left.time_ms - right.time_ms);
    if (!frames.length) return null;
    const time = clamp(number(timeMs, 0), 0, actionDuration(action));
    const first = frames[0];
    let segment = {
      time_ms: first.time_ms,
      duration_ms: 0,
      easing: first.easing,
      start: [...first.servos_us],
      target: [...first.servos_us],
    };
    let led = deepCopy(first.led);
    if (time < first.time_ms) return { servos_us: [...first.servos_us], led, frame_id: first.id };
    let frameId = first.id;
    for (let index = 1; index < frames.length; index += 1) {
      const frame = frames[index];
      if (time < frame.time_ms) break;
      const start = sampleSegment(segment, frame.time_ms);
      segment = {
        time_ms: frame.time_ms,
        duration_ms: Math.max(0, integer(frame.duration_ms, 0)),
        easing: frame.easing,
        start,
        target: [...frame.servos_us],
      };
      led = deepCopy(frame.led);
      frameId = frame.id;
    }
    return { servos_us: sampleSegment(segment, time), led, frame_id: frameId };
  }

  function updatePlayheadReadout(positionMs) {
    const duration = actionDuration();
    studio.playheadMs = clamp(number(positionMs, 0), 0, duration);
    $("timelinePlayhead").style.left = `${clamp(studio.playheadMs / duration, 0, 1) * 100}%`;
    $("timelineScrubber").value = String(studio.playheadMs);
    $("timeCurrent").textContent = formatTime(studio.playheadMs);
  }

  function applySampledPose(sample) {
    if (!sample) return;
    studio.poseAngles = sample.servos_us.map((pulse, index) => pulseToAngle(index, pulse));
    const nextLight = normalizeLed(sample.led, [], "preview", false);
    const lightChanged = JSON.stringify(nextLight) !== JSON.stringify(studio.light);
    studio.light = nextLight;
    renderPose();
    if (lightChanged) renderLight();
  }

  function renderPreviewState() {
    const button = $("previewBtn");
    if (!button) return;
    const playing = studio.preview.state === "playing";
    button.classList.toggle("active", playing);
    button.setAttribute("aria-pressed", String(playing));
    const label = $("previewBtnText");
    if (label) label.textContent = playing ? "停止 3D 预览" : "3D 安全预览";
    if ($("previewStatus")) $("previewStatus").textContent = playing ? "仅模型播放 · 不驱动真机" : "预览不会驱动真机";
    syncDogViewer();
    renderCalibrationWizard();
  }

  function stopOfflinePreview({ keepPosition = true } = {}) {
    if (studio.previewRaf) cancelAnimationFrame(studio.previewRaf);
    studio.previewRaf = null;
    studio.previewOrigin = null;
    studio.preview = {
      ...studio.preview,
      state: "stopped",
      position_ms: keepPosition ? studio.playheadMs : 0,
    };
    if (!keepPosition) {
      updatePlayheadReadout(0);
      applySampledPose(sampleAction(activeAction(), 0));
    }
    renderPreviewState();
  }

  function startOfflinePreview() {
    if (studio.recording) {
      toast("录制进行中", "停止并保存录制后才能预览动作。", "warning");
      return;
    }
    if (studio.playback?.state !== "stopped") {
      toast("真机动作正在运行", "请先停止真机播放，再启动离线 3D 预览。", "warning");
      return;
    }
    const action = activeAction();
    if (!action?.keyframes?.length) {
      toast("动作为空", "先添加至少一个关键帧。", "warning");
      return;
    }
    const duration = actionDuration(action);
    const rate = clamp(number($("playbackSpeed").value, 1), .25, 4);
    if (studio.playheadMs >= duration - 1) studio.playheadMs = 0;
    studio.preview = { state: "playing", position_ms: studio.playheadMs, rate };
    studio.previewOrigin = performance.now() - studio.playheadMs / rate;
    renderPreviewState();
    const tick = () => {
      if (studio.preview.state !== "playing") {
        studio.previewRaf = null;
        return;
      }
      let position = (performance.now() - studio.previewOrigin) * rate;
      if (position >= duration) {
        if ($("loopToggle").checked) {
          position %= duration;
          studio.previewOrigin = performance.now() - position / rate;
        } else {
          updatePlayheadReadout(duration);
          applySampledPose(sampleAction(action, duration));
          stopOfflinePreview({ keepPosition: true });
          return;
        }
      }
      studio.preview.position_ms = position;
      updatePlayheadReadout(position);
      applySampledPose(sampleAction(action, position));
      studio.previewRaf = requestAnimationFrame(tick);
    };
    studio.previewRaf = requestAnimationFrame(tick);
  }

  function toggleOfflinePreview() {
    if (studio.preview.state === "playing") stopOfflinePreview({ keepPosition: true });
    else startOfflinePreview();
  }

  function availableFrameTime(action, desiredMs, ignoreFrameId = null) {
    const occupied = new Set(
      action.keyframes
        .filter((frame) => frame.id !== ignoreFrameId)
        .map((frame) => integer(frame.time_ms)),
    );
    let candidate = clamp(Math.round(integer(desiredMs, 0) / 20) * 20, 0, MAX_PROJECT_DURATION_MS);
    while (occupied.has(candidate) && candidate <= MAX_PROJECT_DURATION_MS - 20) candidate += 20;
    if (!occupied.has(candidate)) return candidate;
    candidate = clamp(Math.round(integer(desiredMs, 0) / 20) * 20, 0, MAX_PROJECT_DURATION_MS);
    while (occupied.has(candidate) && candidate >= 20) candidate -= 20;
    if (!occupied.has(candidate)) return candidate;
    throw new ProjectValidationError(["时间线没有可用位置，请删除关键帧或缩短工程"]);
  }

  function formatTime(ms) {
    const safe = Math.max(0, Math.round(number(ms, 0)));
    const minutes = Math.floor(safe / 60000);
    const seconds = Math.floor((safe % 60000) / 1000);
    const millis = safe % 1000;
    return `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}.${String(millis).padStart(3, "0")}`;
  }

  function renderTimeline() {
    const action = activeAction();
    if (!action) return;
    const duration = actionDuration(action);
    studio.playheadMs = clamp(studio.playheadMs, 0, duration);
    $("timelineScrubber").max = duration;
    $("timelineScrubber").value = studio.playheadMs;
    $("timelinePlayhead").style.left = `${(studio.playheadMs / duration) * 100}%`;
    $("timeCurrent").textContent = formatTime(studio.playheadMs);
    $("timeTotal").textContent = formatTime(duration);
    $("frameCount").textContent = `${action.keyframes.length} 个关键帧`;

    const ruler = $("timelineRuler");
    const majorCount = 8;
    const rulerNodes = [];
    for (let index = 0; index <= majorCount; index += 1) {
      const tick = document.createElement("i");
      tick.className = "ruler-tick";
      tick.style.left = `${(index / majorCount) * 100}%`;
      const label = document.createElement("span");
      label.textContent = formatTime(duration * index / majorCount).slice(0, 5);
      tick.append(label);
      rulerNodes.push(tick);
      if (index < majorCount) {
        const minor = document.createElement("i");
        minor.className = "ruler-tick minor";
        minor.style.left = `${((index + .5) / majorCount) * 100}%`;
        rulerNodes.push(minor);
      }
    }
    ruler.replaceChildren(...rulerNodes);

    $("timelineGrid").replaceChildren(makePoseTimelineTrack(action, duration));
    renderKeyframeInspector();
  }

  function makePoseTimelineTrack(action, duration) {
    const track = document.createElement("div");
    track.className = "timeline-track pose-track";
    track.dataset.track = "pose";
    track.setAttribute("role", "group");
    track.setAttribute("aria-label", "四腿与灯光组合关键帧轨道");
    action.keyframes.forEach((frame, frameIndex) => {
      const node = document.createElement("button");
      node.type = "button";
      node.className = `keyframe-node composite${frame.id === studio.selectedFrameId ? " selected" : ""}`;
      node.style.left = `${clamp(frame.time_ms / duration, 0, 1) * 100}%`;
      node.style.setProperty("--node-light", frame.led.enabled ? frame.led.color : "#59636f");
      node.dataset.frameId = frame.id;
      node.dataset.track = "pose";
      node.disabled = studio.recording;
      const servoSummary = LEGS.map((leg, index) => `${leg.code} ${frame.servos_us[index]} µs`).join(" · ");
      const lightSummary = frame.led.enabled ? `灯光 ${frame.led.color}` : "灯光关闭";
      node.setAttribute("aria-label", `关键帧 ${frameIndex + 1}，${formatTime(frame.time_ms)}，${servoSummary}，${lightSummary}`);
      node.title = `关键帧 ${frameIndex + 1} · ${formatTime(frame.time_ms)}\n${servoSummary}\n${lightSummary}`;
      const numberLabel = document.createElement("span");
      numberLabel.textContent = String(frameIndex + 1);
      const lightDot = document.createElement("i");
      lightDot.setAttribute("aria-hidden", "true");
      node.append(numberLabel, lightDot);
      node.addEventListener("click", (event) => {
        event.stopPropagation();
        selectFrame(frame.id);
      });
      node.addEventListener("pointerdown", (event) => beginFrameDrag(event, frame.id, duration));
      track.append(node);
    });
    track.addEventListener("click", (event) => {
      const rect = track.getBoundingClientRect();
      studio.playheadMs = clamp(((event.clientX - rect.left) / rect.width) * duration, 0, duration);
      renderTimeline();
    });
    return track;
  }

  function beginFrameDrag(event, frameId, duration) {
    if (studio.recording || event.button !== 0) return;
    event.preventDefault();
    event.stopPropagation();
    selectFrame(frameId);
    const rect = $("timelineGrid").getBoundingClientRect();
    studio.drag = { frameId, duration, rect };
  }

  function moveFrameDrag(event) {
    if (!studio.drag) return;
    const frame = activeAction().keyframes.find((item) => item.id === studio.drag.frameId);
    if (!frame) return;
    const ratio = clamp((event.clientX - studio.drag.rect.left) / studio.drag.rect.width, 0, 1);
    frame.time_ms = Math.round((ratio * studio.drag.duration) / 20) * 20;
    document.querySelectorAll("[data-frame-id]").forEach((node) => {
      if (node.dataset.frameId === frame.id) {
        node.style.left = `${(frame.time_ms / studio.drag.duration) * 100}%`;
      }
    });
    studio.playheadMs = frame.time_ms;
    $("timelinePlayhead").style.left = `${(frame.time_ms / studio.drag.duration) * 100}%`;
    $("timeCurrent").textContent = formatTime(frame.time_ms);
    $("keyframeTime").value = frame.time_ms;
  }

  function endFrameDrag() {
    if (!studio.drag) return;
    const action = activeAction();
    const frame = action.keyframes.find((item) => item.id === studio.drag.frameId);
    if (frame) frame.time_ms = availableFrameTime(action, frame.time_ms, frame.id);
    studio.drag = null;
    action.keyframes.sort((left, right) => left.time_ms - right.time_ms);
    markDirty("关键帧时间已调整");
    queueActionRemoteSave(action);
    renderTimeline();
  }

  function selectFrame(frameId) {
    studio.selectedFrameId = frameId;
    const frame = selectedFrame();
    if (frame) applyFrame(frame, { send: false });
    else renderTimeline();
  }

  function keyframeTestDuration(frame) {
    // A timeline Step or a short dynamic frame is unsafe for first-contact
    // tuning.  Single-step tests intentionally slow it down and round to the
    // firmware's 20 ms interpolation cadence.  Full playback keeps the authored
    // duration and remains a separate, explicit operation.
    const rate = clamp(number($("playbackSpeed")?.value, .25), .25, 1);
    const authoredMs = clamp(Math.max(40, integer(frame?.duration_ms, 40)), 40, 10000);
    return clamp(Math.ceil(Math.max(1000, authoredMs / rate) / 20) * 20, 1000, 10000);
  }

  async function testKeyframeOnDevice(frame) {
    if (!frame) {
      toast("未选择关键帧", "先选择一个姿态帧，再做真机单步。", "warning");
      return;
    }
    if (studio.recording) {
      toast("录制进行中", "停止并保存录制后才能做真机单步。", "warning");
      return;
    }
    if ((studio.playback?.state || "stopped") !== "stopped") {
      toast("动作尚未停止", "先停止真机播放，再做逐帧测试。", "warning");
      return;
    }
    if (!studio.online) {
      toast("设备离线", "ADB 恢复在线后才能做真机单步。", "warning");
      return;
    }
    if (!studio.armed) {
      $("armDialog").showModal();
      updatePowerInterlock();
      toast("舵机仍处于锁定", "完成电源和机械安全确认后，才能测试当前帧。", "warning");
      return;
    }
    if (studio.poseSending || studio.poseQueued || studio.frameTestSending) {
      toast("姿态命令尚未空闲", "等待上一条姿态命令确认后再试。", "warning");
      return;
    }

    stopOfflinePreview({ keepPosition: true });
    applyFrame(frame, { send: false });
    const durationMs = keyframeTestDuration(frame);
    const payload = {
      // Use the exact stored pulses. A pulse -> angle -> pulse round trip may
      // otherwise move an asymmetrically calibrated endpoint by one count.
      servos_us: frame.servos_us.map((pulse) => integer(pulse)),
      duration_ms: durationMs,
      easing: frame.duration_ms === 0 ? "smooth" : (EASING_TO_WIRE[frame.easing] || "smooth"),
      led: deepCopy(frame.led),
      source: "keyframe-test",
    };
    studio.frameTestSending = true;
    setPoseAck("单步等待固件确认", "pending");
    setCommandBusy(true, "FRAME TEST");
    renderKeyframeInspector();
    const started = performance.now();
    try {
      await requestJson(API.pose, { method: "POST", body: JSON.stringify(payload) });
      setPoseAck("单步命令已确认", "ok");
      $("lastCommand").textContent = `FRAME TEST · ${payload.servos_us.join("/")} µs · ${durationMs} ms`;
      toast(
        "真机单步已下发",
        `${durationMs} ms 慢速过渡；这里只确认命令接收，不代表舵机带载后真实到位。`,
        "success",
      );
      // /api/pose acknowledges queueing, not physical arrival. Keep other
      // frame-test controls interlocked for the requested transition window;
      // Esc/disarm remains available throughout this host-side wait.
      await new Promise((resolve) => window.setTimeout(resolve, durationMs + 120));
      setPoseAck("单步等待目视确认", "ok");
    } catch (error) {
      setPoseAck("单步下发失败", "error");
      toast("真机单步失败", error.message, "error");
    } finally {
      studio.frameTestSending = false;
      setCommandBusy(false);
      renderKeyframeInspector();
      $("lastCommand").dataset.elapsedMs = String(Math.round(performance.now() - started));
    }
  }

  async function selectAndTestKeyframe(direction) {
    const frame = direction === 0 ? selectedFrame() : nearestFrame(direction);
    if (!frame) {
      toast("没有可测试的关键帧", "先在当前动作中添加姿态帧。", "warning");
      return;
    }
    if (frame.id !== studio.selectedFrameId) selectFrame(frame.id);
    await testKeyframeOnDevice(frame);
  }

  function renderKeyframeInspector() {
    const frame = selectedFrame();
    const controls = [$("keyframeTime"), $("keyframeDuration"), $("keyframeEasing")];
    controls.forEach((control) => { control.disabled = studio.recording || !frame; });
    $("duplicateKeyframeBtn").disabled = studio.recording || !frame;
    $("deleteKeyframeBtn").disabled = studio.recording || !frame;
    if ($("updateKeyframeBtn")) $("updateKeyframeBtn").disabled = studio.recording || !frame;
    $("addKeyframeBtn").disabled = studio.recording;
    const frameTestBusy = Boolean(studio.recording || studio.poseSending || studio.poseQueued || studio.frameTestSending);
    if ($("testSelectedFrameBtn")) $("testSelectedFrameBtn").disabled = frameTestBusy || !frame;
    if ($("testPreviousFrameBtn")) $("testPreviousFrameBtn").disabled = frameTestBusy || !(activeAction()?.keyframes?.length);
    if ($("testNextFrameBtn")) $("testNextFrameBtn").disabled = frameTestBusy || !(activeAction()?.keyframes?.length);
    const label = $("keyframeInspector").querySelector(":scope > span");
    if (!frame) {
      label.textContent = "未选择关键帧";
      return;
    }
    const action = activeAction();
    const index = action.keyframes.findIndex((item) => item.id === frame.id);
    label.textContent = `关键帧 ${index + 1}`;
    $("keyframeTime").value = frame.time_ms;
    $("keyframeDuration").value = frame.duration_ms;
    $("keyframeEasing").value = uiEasing(frame.easing, frame.duration_ms);
  }

  function addKeyframe() {
    if (studio.recording) return;
    const action = activeAction();
    const frameTime = availableFrameTime(action, studio.playheadMs);
    const frame = makeFrame(frameTime, currentServosUs(), {
      duration_ms: currentMotionParameters().duration_ms,
      easing: currentMotionParameters().easing,
      led: studio.light,
    });
    action.keyframes.push(frame);
    action.keyframes.sort((left, right) => left.time_ms - right.time_ms);
    action.duration_ms = Math.max(action.duration_ms, frame.time_ms + Math.max(20, frame.duration_ms));
    studio.selectedFrameId = frame.id;
    studio.playheadMs = frame.time_ms;
    markDirty("已添加当前姿态与灯光关键帧");
    queueActionRemoteSave(action);
    renderTimeline();
    toast("关键帧已添加", `${formatTime(frame.time_ms)} · 四路姿态与灯光已保存。`, "success");
  }

  function duplicateKeyframe() {
    if (studio.recording) return;
    const source = selectedFrame();
    if (!source) return;
    const action = activeAction();
    const copy = deepCopy(source);
    copy.id = uid("frame");
    copy.time_ms = availableFrameTime(
      action,
      clamp(source.time_ms + Math.max(100, source.duration_ms), 0, MAX_PROJECT_DURATION_MS),
    );
    action.keyframes.push(copy);
    action.keyframes.sort((left, right) => left.time_ms - right.time_ms);
    action.duration_ms = Math.max(action.duration_ms, copy.time_ms + Math.max(20, copy.duration_ms));
    studio.selectedFrameId = copy.id;
    studio.playheadMs = copy.time_ms;
    markDirty("关键帧已复制");
    queueActionRemoteSave(action);
    renderTimeline();
  }

  function updateSelectedFramePose() {
    if (studio.recording) return;
    const frame = selectedFrame();
    if (!frame) {
      toast("未选择关键帧", "先单击时间线中的一个姿态帧，再更新它。", "warning");
      return;
    }
    const motion = currentMotionParameters();
    frame.servos_us = currentServosUs();
    frame.led = deepCopy(studio.light);
    frame.duration_ms = motion.duration_ms;
    frame.easing = motion.easing;
    activeAction().duration_ms = actionDuration(activeAction());
    markDirty("已用当前姿态更新关键帧");
    queueActionRemoteSave(activeAction());
    renderTimeline();
    toast("关键帧已更新", `${formatTime(frame.time_ms)} · 四路姿态与灯光已覆盖。`, "success");
  }

  function deleteSelectedFrame() {
    if (studio.recording) return;
    const frame = selectedFrame();
    if (!frame) return;
    const action = activeAction();
    action.keyframes = action.keyframes.filter((item) => item.id !== frame.id);
    studio.selectedFrameId = null;
    markDirty("关键帧已删除");
    queueActionRemoteSave(action);
    renderTimeline();
  }

  function updateSelectedFrame(field, value) {
    if (studio.recording) return;
    const frame = selectedFrame();
    if (!frame) return;
    if (field === "time_ms") {
      frame.time_ms = availableFrameTime(activeAction(), value, frame.id);
      studio.playheadMs = frame.time_ms;
      activeAction().keyframes.sort((left, right) => left.time_ms - right.time_ms);
    } else if (field === "duration_ms") {
      frame.duration_ms = clamp(integer(value, frame.duration_ms), 0, 10000);
    } else if (field === "easing") {
      frame.duration_ms = value === "step" ? 0 : Math.max(40, frame.duration_ms || 600);
      frame.easing = EASING_TO_WIRE[value] || "smooth";
    }
    activeAction().duration_ms = actionDuration(activeAction());
    markDirty("关键帧参数已修改");
    queueActionRemoteSave(activeAction());
    renderTimeline();
  }

  function applyFrame(frame, { send = false } = {}) {
    if (!frame) return;
    studio.poseAngles = frame.servos_us.map((pulse, index) => pulseToAngle(index, pulse));
    studio.light = normalizeLed(frame.led, [], "frame", false);
    studio.playheadMs = frame.time_ms;
    $("transitionInput").value = Math.max(40, frame.duration_ms || 40);
    $("transitionRange").value = clamp(Math.max(40, frame.duration_ms || 40), 40, 5000);
    $("easingSelect").value = uiEasing(frame.easing, frame.duration_ms);
    renderPose({ schedule: send, source: "keyframe" });
    renderLight({ schedule: send });
    renderTimeline();
  }

  function nearestFrame(direction) {
    const frames = activeAction()?.keyframes || [];
    if (!frames.length) return null;
    if (direction < 0) {
      return [...frames].reverse().find((frame) => frame.time_ms < studio.playheadMs - 1) || frames[0];
    }
    return frames.find((frame) => frame.time_ms > studio.playheadMs + 1) || frames[frames.length - 1];
  }

  async function playback(operation) {
    if (studio.recording) {
      toast("录制进行中", "请先停止并保存录制，再使用播放控制。", "warning");
      return;
    }
    if (studio.frameTestSending && ["play", "resume"].includes(operation)) {
      toast("真机单步尚未结束", "等待当前慢速过渡窗口结束后再播放连续动作。", "warning");
      return;
    }
    const action = activeAction();
    if (!action || !action.keyframes.length) {
      toast("动作为空", "先添加至少一个关键帧。", "warning");
      return;
    }
    if (["play", "resume"].includes(operation)) stopOfflinePreview({ keepPosition: true });
    if (["play", "resume"].includes(operation) && !studio.armed) {
      $("armDialog").showModal();
      updatePowerInterlock();
      toast("舵机仍处于锁定", "通过安全确认后才能播放真实动作。", "warning");
      return;
    }
    const payload = {
      operation,
      action_id: action.id,
      rate: number($("playbackSpeed").value, 1),
      loop: $("loopToggle").checked,
      loop_count: $("loopToggle").checked ? 0 : 1,
      off: false,
    };
    try {
      await requestJson(API.playback, { method: "POST", body: JSON.stringify(payload) });
      if (operation === "play") studio.playheadMs = 0;
      studio.playback = {
        ...studio.playback,
        state: operation === "pause" ? "paused" : operation === "stop" ? "stopped" : "playing",
        position_ms: studio.playheadMs,
        duration_ms: actionDuration(action),
        rate: payload.rate,
      };
      studio.playbackOrigin = performance.now() - studio.playheadMs / payload.rate;
      renderPlaybackState();
    } catch (error) {
      toast("播放控制失败", error.message, "error");
    }
  }

  function renderPlaybackState() {
    const state = studio.playback?.state || "stopped";
    $("playBtn").classList.toggle("active", state === "playing");
    $("pauseBtn").classList.toggle("active", state === "paused");
    syncDogViewer();
    if (state === "playing") startPlaybackClock();
    else stopPlaybackClock();
    renderCalibrationWizard();
    renderHomeButton();
  }

  function startPlaybackClock() {
    if (studio.playbackRaf) return;
    const rate = number(studio.playback.rate ?? $("playbackSpeed").value, 1);
    if (studio.playbackOrigin == null) studio.playbackOrigin = performance.now() - studio.playheadMs / rate;
    const tick = () => {
      if (studio.playback.state !== "playing") {
        studio.playbackRaf = null;
        return;
      }
      const duration = actionDuration();
      let position = (performance.now() - studio.playbackOrigin) * rate;
      if (position > duration) {
        if (activeAction().loop || $("loopToggle").checked) {
          position %= duration;
          studio.playbackOrigin = performance.now() - position / rate;
        } else {
          position = duration;
        }
      }
      studio.playheadMs = position;
      $("timelinePlayhead").style.left = `${clamp(position / duration, 0, 1) * 100}%`;
      $("timelineScrubber").value = position;
      $("timeCurrent").textContent = formatTime(position);
      studio.playbackRaf = requestAnimationFrame(tick);
    };
    studio.playbackRaf = requestAnimationFrame(tick);
  }

  function stopPlaybackClock() {
    if (studio.playbackRaf) cancelAnimationFrame(studio.playbackRaf);
    studio.playbackRaf = null;
    studio.playbackOrigin = null;
    renderTimeline();
  }

  function togglePlayback() {
    if (studio.recording) {
      toast("录制进行中", "请先停止并保存录制，再播放动作。", "warning");
      return;
    }
    const state = studio.playback?.state || "stopped";
    if (state === "playing") playback("pause");
    else if (state === "paused") playback("resume");
    else playback("play");
  }

  async function startRecording() {
    if (studio.recording) return;
    if (studio.frameTestSending) {
      toast("真机单步尚未结束", "等待当前慢速过渡窗口结束后再开始录制。", "warning");
      return;
    }
    const rawName = $("recordNameInput").value.trim();
    if (!rawName) {
      toast("请先命名动作", "在第 1 步输入动作名称，再开始录制。", "warning");
      $("recordNameInput").focus();
      return;
    }
    if (!studio.armed) {
      $("armDialog").showModal();
      updatePowerInterlock();
      toast("录制需要开启输出", "通过安全确认后再开始录制。", "warning");
      return;
    }
    if (studio.playback?.state !== "stopped") {
      toast("播放尚未停止", "停止当前动作后才能开始录制。", "warning");
      return;
    }
    const name = safeText(rawName, "新动作", 40);
    try {
      const payload = await requestJson(API.recordStart, {
        method: "POST",
        body: JSON.stringify({ name }),
      });
      studio.recording = true;
      studio.pendingRecordName = name;
      studio.recordingInfo = {
        ...studio.recordingInfo,
        ...(payload.result && typeof payload.result === "object" ? payload.result : {}),
        state: "recording",
        active: true,
        name,
        frame_count: integer(payload.result?.frame_count, studio.recordingInfo.frame_count || 0),
        duration_ms: integer(payload.result?.duration_ms, 0),
        saved_action_id: null,
        interrupted: false,
      };
      renderRecordingState();
      toast("动作录制已开始", `正在录制“${name}”；拖动舵机和调整灯光即可生成关键帧。`, "success");
    } catch (error) {
      toast("开始录制失败", error.message, "error");
    }
  }

  async function stopRecording() {
    if (!studio.recording) return;
    const name = safeText(studio.pendingRecordName || $("recordNameInput").value, "新动作", 40);
    $("recordStopBtn").disabled = true;
    $("recordStopBtn").textContent = "正在保存…";
    try {
      // Deliberately omit action_id: every recording creates a new action and
      // never overwrites the action the operator was viewing before recording.
      const payload = await requestJson(API.recordStop, {
        method: "POST",
        body: JSON.stringify({ name }),
      });
      studio.recording = false;
      if (payload.state?.project) {
        studio.project = fromBackendProject(payload.state.project, studio.project);
        studio.projectInitialized = true;
        studio.dirty = false;
      } else {
        await loadRemoteProject();
      }
      const createdAction = payload.result && typeof payload.result === "object" ? payload.result : null;
      if (createdAction?.id) {
        studio.activeActionId = createdAction.id;
      }
      studio.selectedFrameId = null;
      studio.playheadMs = 0;
      const frameCount = Array.isArray(createdAction?.keyframes)
        ? createdAction.keyframes.length
        : integer(studio.recordingInfo.frame_count, 0);
      const durationMs = integer(
        createdAction?.duration_ms,
        studio.recordingInfo.duration_ms || 0,
      );
      studio.recordingInfo = {
        ...studio.recordingInfo,
        state: "idle",
        active: false,
        name,
        frame_count: frameCount,
        duration_ms: durationMs,
        saved_action_id: createdAction?.id || studio.activeActionId,
        saved_at: new Date().toISOString(),
        interrupted: false,
      };
      persistLocalProject();
      renderProjectControls();
      renderTimeline();
      renderRecordingState();
      toast("录制已保存为新动作", `“${name}” · ${frameCount} 帧 · ${formatTime(durationMs)}`, "success", 5600);
    } catch (error) {
      $("recordStopBtn").disabled = false;
      $("recordStopBtn").textContent = "停止并保存";
      toast("停止录制失败", error.message, "error");
    }
  }

  function toggleRecording() {
    return studio.recording ? stopRecording() : startRecording();
  }

  function renderRecordingState() {
    const recording = studio.recording;
    const info = studio.recordingInfo || {};
    $("recordWorkflow").classList.toggle("recording", recording);
    $("recordNameInput").disabled = recording;
    $("recordStartBtn").disabled = recording;
    $("recordStartBtn").setAttribute("aria-pressed", String(recording));
    $("recordStopBtn").disabled = !recording;
    $("recordStopBtn").textContent = "停止并保存";
    const name = safeText(info.name || studio.pendingRecordName, "", 40);
    const frameCount = Math.max(0, integer(info.frame_count, 0));
    const durationMs = Math.max(0, integer(info.duration_ms, 0));
    if (recording) {
      $("recordingInfo").textContent = `正在录制${name ? `“${name}”` : ""} · ${frameCount} 帧 · ${formatTime(durationMs)}`;
    } else if (info.interrupted) {
      $("recordingInfo").textContent = "录制已中断，未创建动作";
    } else if (info.saved_action_id) {
      $("recordingInfo").textContent = `已保存${name ? `“${name}”` : "新动作"} · ${frameCount} 帧 · ${formatTime(durationMs)}`;
    } else {
      $("recordingInfo").textContent = "尚未开始录制";
    }

    [
      "actionSelect", "importBtn", "projectImport", "newActionBtn", "renameActionBtn", "deleteActionBtn",
      "previewBtn", "playBtn", "pauseBtn", "stopBtn", "previousFrameBtn", "nextFrameBtn", "loopToggle", "playbackSpeed",
      "timelineScrubber", "addKeyframeBtn", "updateKeyframeBtn",
    ].forEach((id) => {
      const control = $(id);
      if (control) control.disabled = recording;
    });
    renderKeyframeInspector();
    $("recordingHint").textContent = studio.recording
      ? "正在录制：姿态和灯光变化会生成带时间戳的关键帧。"
      : "单击关键帧即可载入；调整后用“更新当前姿态”，或另存为新姿态。";
    renderCalibrationWizard();
    renderHomeButton();
  }

  function gaitPhaseOffsets(pattern) {
    // Channel order is FL, FR, RR, RL (PA04..PA07), not anatomical
    // FL, FR, RL, RR. Keep every phase vector in that physical wire order.
    if (pattern === "trot") return [0, .5, 0, .5];
    if (pattern === "pace") return [0, .5, .5, 0];
    // gaitWave() is sampled with cycle + phase, so an offset P reaches its
    // lift peak at cycle (1 - P) % 1.  These wire-order offsets therefore
    // produce the chronological crawl FL -> RR -> FR -> RL.
    return [0, .5, .75, .25];
  }

  function gaitWave(phase, stanceRatio = .78) {
    const unit = ((phase % 1) + 1) % 1;
    if (unit < stanceRatio) {
      return 1 - 2 * easingProgress("smooth", unit / stanceRatio);
    }
    return -1 + 2 * easingProgress("smooth", (unit - stanceRatio) / (1 - stanceRatio));
  }

  function gaitDialogValues() {
    return {
      pattern: $("gaitPattern")?.value || "crawl",
      stroke: clamp(number($("gaitStroke")?.value, 10), 5, 30),
      cadence: clamp(number($("gaitCadence")?.value, .3), .2, 1),
      cycles: clamp(integer($("gaitCycles")?.value, 2), 1, 12),
      turn: clamp(number($("gaitTurn")?.value, 0), -50, 50),
    };
  }

  function renderGaitDialog() {
    document.querySelectorAll("[data-action-template]").forEach((button) => {
      button.classList.toggle("active", button.dataset.actionTemplate === studio.actionTemplate);
      button.setAttribute("aria-pressed", String(button.dataset.actionTemplate === studio.actionTemplate));
    });
    const isGenerator = studio.actionTemplate !== "custom";
    if ($("gaitParameters")) $("gaitParameters").hidden = !isGenerator || studio.actionDialogMode !== "new";
    if ($("actionTemplateGrid")) $("actionTemplateGrid").hidden = studio.actionDialogMode !== "new";
    const values = gaitDialogValues();
    if ($("gaitStrokeValue")) $("gaitStrokeValue").textContent = `${Math.round(values.stroke)}%`;
    if ($("gaitCadenceValue")) $("gaitCadenceValue").textContent = `${values.cadence.toFixed(2)} Hz`;
    if ($("gaitTurnValue")) {
      $("gaitTurnValue").textContent = values.turn === 0 ? "直行" : `${values.turn < 0 ? "左" : "右"} ${Math.abs(Math.round(values.turn))}%`;
    }
  }

  function selectActionTemplate(templateId) {
    if (!ACTION_TEMPLATES[templateId] || studio.actionDialogMode !== "new") return;
    studio.actionTemplate = templateId;
    const template = ACTION_TEMPLATES[templateId];
    $("actionNameInput").value = template.name;
    if ($("gaitTurn")) $("gaitTurn").value = String(template.turn);
    if ($("gaitStroke") && templateId === "march") $("gaitStroke").value = "6";
    else if ($("gaitStroke")) $("gaitStroke").value = "10";
    renderGaitDialog();
  }

  function generateGaitAction(name, templateId) {
    const template = ACTION_TEMPLATES[templateId];
    if (!template || templateId === "custom") {
      return {
        id: uid("action"), name, duration_ms: 4000, loop: false, note: "", keyframes: [],
      };
    }
    const values = gaitDialogValues();
    const phases = gaitPhaseOffsets(values.pattern);
    const samplesPerCycle = 8;
    const periodMs = 1000 / values.cadence;
    const stepMs = Math.max(20, Math.round((periodMs / samplesPerCycle) / 20) * 20);
    const totalSamples = values.cycles * samplesPerCycle;
    const baseAmplitudeDeg = 90 * values.stroke / 100;
    const templateTurn = templateId === "turn-left"
      ? -Math.max(5, Math.abs(values.turn))
      : templateId === "turn-right"
        ? Math.max(5, Math.abs(values.turn))
        : values.turn;
    const turnMix = templateTurn / 100;
    const keyframes = [];
    for (let sampleIndex = 0; sampleIndex <= totalSamples; sampleIndex += 1) {
      const progress = sampleIndex / Math.max(1, totalSamples);
      const envelope = Math.min(1, progress * 4, (1 - progress) * 4);
      const cycle = sampleIndex / samplesPerCycle;
      const angles = phases.map((phase, legIndex) => {
        const leftSide = legIndex === 0 || legIndex === 3;
        const sideGain = leftSide ? 1 + turnMix : 1 - turnMix;
        const direction = template.direction || 1;
        const wave = gaitWave(direction * cycle + phase);
        return clamp(90 + wave * baseAmplitudeDeg * sideGain * envelope, 0, 180);
      });
      const timeMs = sampleIndex * stepMs;
      keyframes.push(makeFrame(
        timeMs,
        angles.map((angle, legIndex) => angleToPulse(legIndex, angle)),
        {
          duration_ms: sampleIndex === 0 ? 0 : stepMs,
          easing: sampleIndex === 0 ? "step" : "smooth",
          led: deepCopy(studio.light),
        },
      ));
    }
    const durationMs = keyframes[keyframes.length - 1]?.time_ms || stepMs;
    return {
      id: uid("action"),
      name,
      duration_ms: durationMs,
      loop: false,
      note: `${template.note} 生成参数：${values.pattern} / 行程 ${Math.round(values.stroke)}% / ${values.cadence.toFixed(2)} Hz / ${values.cycles} 周期 / 转向 ${Math.round(templateTurn)}%。`,
      default_rate: 1,
      end_behavior: "hold",
      keyframes,
    };
  }

  function openActionDialog(mode) {
    if (studio.recording) {
      toast("录制进行中", "停止并保存后才能新建或重命名动作。", "warning");
      return;
    }
    studio.actionDialogMode = mode;
    const action = activeAction();
    $("actionDialogTitle").textContent = mode === "new" ? "新建动作" : "重命名动作";
    if (mode === "new") studio.actionTemplate = "custom";
    $("actionNameInput").value = mode === "new" ? ACTION_TEMPLATES[studio.actionTemplate].name : action.name;
    renderGaitDialog();
    $("actionDialog").showModal();
    requestAnimationFrame(() => $("actionNameInput").select());
  }

  function commitActionDialog() {
    const name = safeText($("actionNameInput").value, "新动作", 40);
    if (studio.actionDialogMode === "new") {
      const action = generateGaitAction(name, studio.actionTemplate);
      studio.project.actions.push(action);
      studio.activeActionId = action.id;
      studio.selectedFrameId = null;
      studio.playheadMs = 0;
      saveActionRemote(action, { create: true, quiet: false });
    } else {
      activeAction().name = name;
      queueActionRemoteSave(activeAction(), 0);
    }
    markDirty(studio.actionDialogMode === "new" ? "已新建动作" : "动作已重命名");
    renderProjectControls();
    renderTimeline();
  }

  function deleteActiveAction() {
    if (studio.recording) {
      toast("录制进行中", "停止并保存后才能删除动作。", "warning");
      return;
    }
    if (studio.project.actions.length <= 1) {
      toast("不能删除", "工程至少需要保留一个动作。", "warning");
      return;
    }
    const deleted = activeAction();
    studio.project.actions = studio.project.actions.filter((action) => action.id !== deleted.id);
    studio.activeActionId = studio.project.actions[0].id;
    studio.selectedFrameId = null;
    studio.playheadMs = 0;
    markDirty(`已删除动作“${deleted.name}”`);
    if (studio.serverSeen) {
      requestJson(`${API.actions}/${encodeURIComponent(deleted.id)}`, { method: "DELETE" })
        .catch((error) => toast("远端动作删除失败", error.message, "error"));
    }
    renderProjectControls();
    renderTimeline();
  }

  function exportProject() {
    studio.project.name = safeText($("projectName").value, "四足动作工程", 64);
    studio.project.updated_at = new Date().toISOString();
    const blob = new Blob([`${JSON.stringify(studio.project, null, 2)}\n`], { type: "application/json;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${studio.project.name.replace(/[\\/:*?"<>|\s]+/g, "-").slice(0, 48) || "arcs-motion"}.arcsmotion.json`;
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    URL.revokeObjectURL(url);
    toast("工程已导出", anchor.download, "success");
  }

  async function importProject(file) {
    if (!file) return;
    if (studio.recording) {
      toast("录制进行中", "停止并保存后才能打开其他工程。", "warning");
      $("projectImport").value = "";
      return;
    }
    try {
      if (file.size > 5 * 1024 * 1024) throw new Error("工程文件不能超过 5 MB");
      const parsed = JSON.parse(await file.text());
      const imported = normalizeProject(parsed, { strict: true });
      studio.project = imported;
      studio.activeActionId = imported.actions[0].id;
      studio.selectedFrameId = null;
      studio.poseAngles = [90, 90, 90, 90];
      studio.playheadMs = 0;
      studio.dirty = true;
      renderProject();
      persistLocalProject();
      await saveProjectRemote({ quiet: true });
      toast("工程导入成功", `${imported.actions.length} 个动作，所有脉宽均通过安全校验。`, "success");
    } catch (error) {
      const detail = error instanceof ProjectValidationError ? error.errors.slice(0, 4).join("；") : error.message;
      toast("工程导入被拒绝", detail || "文件格式不正确", "error", 8000);
    } finally {
      $("projectImport").value = "";
    }
  }

  function toast(title, detail = "", kind = "info", duration = 3600) {
    const node = document.createElement("div");
    node.className = `toast ${kind}`;
    const copy = document.createElement("div");
    const heading = document.createElement("strong");
    heading.textContent = title;
    const description = document.createElement("small");
    description.textContent = detail;
    copy.append(heading, description);
    const close = document.createElement("button");
    close.type = "button";
    close.setAttribute("aria-label", "关闭通知");
    close.textContent = "×";
    node.append(copy, close);
    const remove = () => {
      node.classList.add("removing");
      window.setTimeout(() => node.remove(), 190);
    };
    close.addEventListener("click", remove);
    $("toastRegion").append(node);
    window.setTimeout(remove, duration);
  }

  function connectEvents(path = API.events) {
    studio.eventSource?.close();
    const source = new EventSource(path);
    studio.eventSource = source;
    source.onopen = () => {
      studio.sseErrors = 0;
      studio.serverSeen = true;
      if (studio.online) $("connectionBanner").classList.remove("show", "error");
    };
    source.onmessage = (event) => {
      try {
        const payload = JSON.parse(event.data);
        const snapshot = payload.state || payload.result || payload;
        applySnapshot(snapshot);
      } catch (error) {
        console.warn("Ignoring malformed SSE event", error);
      }
    };
    source.onerror = () => {
      studio.sseErrors += 1;
      if (studio.sseErrors >= 2 && path === API.events && !studio.eventFallbackUsed) {
        studio.eventFallbackUsed = true;
        source.close();
        connectEvents(API.eventsCompat);
      }
    };
  }

  function bindServoControls() {
    LEGS.forEach((leg, index) => {
      const range = $( `leg${index}Range` );
      range.addEventListener("input", () => {
        studio.poseAngles[index] = clamp(integer(range.value, 90), 0, 180);
        if ($("linkedEdit").checked) {
          const mate = index % 2 === 0 ? index + 1 : index - 1;
          studio.poseAngles[mate] = studio.poseAngles[index];
        }
        renderPose({ schedule: true, source: studio.recording ? "recording-live" : "manual-live" });
      });
      range.addEventListener("change", () => (
        schedulePose(studio.recording ? "recording-live" : "manual-live")
      ));
      $( `leg${index}Reverse` ).addEventListener("change", applyCalibrationFromUi);
      $( `leg${index}Trim` ).addEventListener("change", applyCalibrationFromUi);
    });

    document.querySelectorAll(".center-leg").forEach((button) => {
      button.addEventListener("click", () => {
        const index = integer(button.dataset.leg, 0);
        studio.poseAngles[index] = 90;
        renderPose({ schedule: true, source: "center-leg" });
      });
    });

    document.querySelectorAll("[data-focus-leg]").forEach((node) => {
      node.addEventListener("click", () => {
        const index = integer(node.dataset.focusLeg, 0);
        document.querySelector(`[data-leg-card="${index}"]`)?.classList.add("focused");
        $( `leg${index}Range` ).focus();
        window.setTimeout(() => document.querySelector(`[data-leg-card="${index}"]`)?.classList.remove("focused"), 900);
      });
    });

    document.querySelectorAll("[data-pose-preset]").forEach((button) => {
      button.addEventListener("click", () => {
        studio.poseAngles = [90, 90, 90, 90];
        renderPose({ schedule: true, source: "preset-center" });
        if (!studio.armed) {
          toast("舵机中位仅预览", "输出尚未解锁，真机不会运动。", "info");
        }
      });
    });

    for (let index = 0; index < LEGS.length; index += 1) {
      for (const suffix of ["Min", "Center", "Max"]) {
        $( `cal${index}${suffix}` ).addEventListener("change", applyCalibrationFromUi);
      }
    }
    $("resetCalibrationBtn").addEventListener("click", () => {
      studio.project.calibration = defaultCalibration();
      renderCalibration();
      applyCalibrationFromUi();
      toast("已恢复保守校准", "四路安全范围为 1000 / 1500 / 2000 µs。", "success");
    });
  }

  function bindSafePostureControls() {
    document.querySelectorAll("[data-safe-posture]").forEach((button) => {
      button.addEventListener("click", () => openSafePostureDialog(button.dataset.safePosture));
    });
    $("safePostureConfirm")?.addEventListener("change", () => {
      $("safePostureRunBtn").disabled = !$("safePostureConfirm").checked;
    });
    $("safePostureRunBtn")?.addEventListener("click", runSafePostureRoutine);
    $("safePostureDialog")?.addEventListener("close", () => {
      if (!studio.postureRoutine.active) studio.postureRoutine.pendingPlan = null;
      if ($("safePostureConfirm")) $("safePostureConfirm").checked = false;
      if ($("safePostureRunBtn")) $("safePostureRunBtn").disabled = true;
    });
    renderSafePostureControls();
  }

  function bindMotionControls() {
    const syncDuration = (source) => {
      const value = clamp(integer(source.value, 600), 40, source === $("transitionInput") ? 10000 : 5000);
      $("transitionInput").value = value;
      $("transitionRange").value = clamp(value, 40, 5000);
    };
    $("transitionRange").addEventListener("input", (event) => syncDuration(event.currentTarget));
    $("transitionInput").addEventListener("change", (event) => syncDuration(event.currentTarget));
    $("speedRange").addEventListener("input", () => {
      $("speedValue").textContent = `${(number($("speedRange").value, 100) / 100).toFixed(2)}×`;
    });
  }

  function bindLightControls() {
    $("lightEnabled").addEventListener("change", () => {
      studio.light.enabled = $("lightEnabled").checked;
      renderLight({ schedule: true });
    });
    $("lightColor").addEventListener("input", () => {
      studio.light.color = normalizeHex($("lightColor").value);
      renderLight({ schedule: true });
    });
    $("lightHex").addEventListener("change", () => {
      const normalized = normalizeHex($("lightHex").value, "");
      if (!normalized) {
        $("lightHex").value = studio.light.color;
        toast("颜色格式无效", "请输入 #RRGGBB，例如 #35A7FF。", "warning");
        return;
      }
      studio.light.color = normalized;
      renderLight({ schedule: true });
    });
    $("brightnessRange").addEventListener("input", () => {
      studio.light.brightness = clamp(integer($("brightnessRange").value, 40), 0, 100);
      renderLight({ schedule: true });
    });
    $("pixelCount").addEventListener("change", () => {
      studio.light.pixel_count = clamp(integer($("pixelCount").value, DEFAULT_PIXELS), 1, MAX_PIXELS);
      renderLight({ schedule: true });
    });
    document.querySelectorAll("[data-light-preset]").forEach((button) => {
      button.addEventListener("click", () => {
        studio.light.enabled = true;
        studio.light.color = normalizeHex(button.dataset.lightPreset);
        studio.light.brightness = clamp(integer(button.dataset.brightness, 40), 0, 100);
        renderLight({ schedule: true });
      });
    });

    $("ledColorOrder").addEventListener("change", () => {
      updateLedCalibrationDraft({ color_order: $("ledColorOrder").value });
    });
    const bindGain = (channel) => {
      const key = `${channel.toLowerCase()}_gain_permille`;
      const range = $( `led${channel}GainRange` );
      const numeric = $( `led${channel}GainNumber` );
      range.addEventListener("input", () => {
        updateLedCalibrationDraft({ [key]: clamp(integer(range.value, 1000), 0, 1000) });
      });
      numeric.addEventListener("change", () => {
        updateLedCalibrationDraft({ [key]: clamp(integer(number(numeric.value, 100) * 10, 1000), 0, 1000) });
      });
    };
    ["R", "G", "B"].forEach(bindGain);
    $("ledGammaRange").addEventListener("input", () => {
      updateLedCalibrationDraft({ gamma_x100: clamp(integer($("ledGammaRange").value, 100), 100, 300) });
    });
    $("ledGammaNumber").addEventListener("change", () => {
      updateLedCalibrationDraft({
        gamma_x100: clamp(integer(number($("ledGammaNumber").value, 1) * 100, 100), 100, 300),
      });
    });
    $("ledCalibrationBypassPreset").addEventListener("click", () => {
      updateLedCalibrationDraft(LED_CALIBRATION_BYPASS);
      toast("已载入旁路默认", "点击“保存校色”后才会写入当前设备。", "info");
    });
    $("ledCalibrationTypicalPreset").addEventListener("click", () => {
      updateLedCalibrationDraft(LED_CALIBRATION_TYPICAL);
      toast("已载入典型灯珠起点", "这是 datasheet 典型白点估算，保存后请用三原色肉眼微调。", "warning", 6000);
    });
    document.querySelectorAll("[data-led-primary]").forEach((button) => {
      button.addEventListener("click", () => {
        testLedPrimary(button.dataset.ledPrimary, button.dataset.ledPrimaryName || "原色");
      });
    });
    $("ledCalibrationRestoreBtn").addEventListener("click", () => restoreLogicalLight());
    $("ledCalibrationSaveBtn").addEventListener("click", saveLedCalibration);
    $("ledCalibrationResetBtn").addEventListener("click", resetLedCalibration);
    $("ledCalibrationPanel").addEventListener("toggle", () => {
      if ($("ledCalibrationPanel").open && !studio.ledCalibration.snapshot && !studio.ledCalibration.loading) {
        queueLedCalibrationRefresh(0);
      }
    });
  }

  function bindTimelineControls() {
    $("addKeyframeBtn").addEventListener("click", addKeyframe);
    $("updateKeyframeBtn")?.addEventListener("click", updateSelectedFramePose);
    $("duplicateKeyframeBtn").addEventListener("click", duplicateKeyframe);
    $("deleteKeyframeBtn").addEventListener("click", deleteSelectedFrame);
    $("keyframeTime").addEventListener("change", (event) => updateSelectedFrame("time_ms", event.currentTarget.value));
    $("keyframeDuration").addEventListener("change", (event) => updateSelectedFrame("duration_ms", event.currentTarget.value));
    $("keyframeEasing").addEventListener("change", (event) => updateSelectedFrame("easing", event.currentTarget.value));
    $("timelineScrubber").addEventListener("input", () => {
      stopOfflinePreview({ keepPosition: true });
      updatePlayheadReadout(number($("timelineScrubber").value, 0));
      applySampledPose(sampleAction(activeAction(), studio.playheadMs));
    });
    $("timelineScrubber").addEventListener("change", () => {
      applySampledPose(sampleAction(activeAction(), studio.playheadMs));
    });
    $("recordStartBtn").addEventListener("click", startRecording);
    $("recordStopBtn").addEventListener("click", stopRecording);
    $("previewBtn")?.addEventListener("click", toggleOfflinePreview);
    $("playBtn").addEventListener("click", togglePlayback);
    $("pauseBtn").addEventListener("click", () => playback("pause"));
    $("stopBtn").addEventListener("click", () => playback("stop"));
    $("previousFrameBtn").addEventListener("click", () => {
      const frame = nearestFrame(-1);
      if (frame) selectFrame(frame.id);
    });
    $("nextFrameBtn").addEventListener("click", () => {
      const frame = nearestFrame(1);
      if (frame) selectFrame(frame.id);
    });
    $("testPreviousFrameBtn")?.addEventListener("click", () => selectAndTestKeyframe(-1));
    $("testSelectedFrameBtn")?.addEventListener("click", () => selectAndTestKeyframe(0));
    $("testNextFrameBtn")?.addEventListener("click", () => selectAndTestKeyframe(1));
    $("loopToggle").addEventListener("change", () => {
      activeAction().loop = $("loopToggle").checked;
      markDirty("循环设置已修改");
      queueActionRemoteSave(activeAction());
    });
    $("actionSelect").addEventListener("change", () => {
      if (studio.recording) {
        $("actionSelect").value = studio.activeActionId;
        return;
      }
      studio.activeActionId = $("actionSelect").value;
      stopOfflinePreview({ keepPosition: false });
      studio.selectedFrameId = null;
      studio.playheadMs = 0;
      $("loopToggle").checked = activeAction().loop;
      renderTimeline();
      if (activeAction().id === "preset-slow-gait") {
        toast("慢速步态是机构相关示例", "4×1DOF 的实际行走效果依赖连杆。请先逐路校准，并架空验证方向。", "warning", 7000);
      }
    });
    $("newActionBtn").addEventListener("click", () => openActionDialog("new"));
    $("renameActionBtn").addEventListener("click", () => openActionDialog("rename"));
    $("deleteActionBtn").addEventListener("click", deleteActiveAction);
  }

  function bindImuControls() {
    $("imuZeroBtn")?.addEventListener("click", zeroImuOrientation);
    $("imuYawFollowToggle")?.addEventListener("change", () => {
      studio.viewerYawFollowEnabled = Boolean($("imuYawFollowToggle").checked);
      renderImuTelemetry();
      if (studio.viewerYawFollowEnabled) {
        toast(
          "3D 航向跟随已开启",
          "MPU6050 没有磁力计，相对 Yaw 会随时间漂移；模型缓慢转向时请关闭此开关。",
          "warning",
          6500,
        );
      } else {
        toast("3D 航向已锁定", "模型仍会跟随实机 Roll / Pitch，不再跟随漂移的 Yaw。", "success");
      }
    });
    $("imuSafetyToggle")?.addEventListener("change", () => {
      studio.imuSafetyEnabled = Boolean($("imuSafetyToggle").checked);
      studio.imuSafetyTripped = false;
      renderImuTelemetry();
      if (studio.imuSafetyEnabled) {
        toast(
          "倾斜保护已开启",
          `Roll 或 Pitch 超过 ${studio.imuTiltLimit}° 时，将通过上位机请求急停；这不是独立硬件断电保护。`,
          "warning",
          6500,
        );
        evaluateImuSafety();
      }
    });
    $("imuTiltLimit")?.addEventListener("change", () => {
      studio.imuTiltLimit = clamp(integer($("imuTiltLimit").value, 35), 5, 85);
      $("imuTiltLimit").value = String(studio.imuTiltLimit);
      studio.imuSafetyTripped = false;
      evaluateImuSafety();
    });
  }

  function bindCalibrationControls() {
    $("calibrationSafetyConfirm")?.addEventListener("change", () => {
      const checked = Boolean($("calibrationSafetyConfirm").checked);
      const identity = calibrationIdentityCheck(studio.deviceCalibration.snapshot, { allowMissing: true });
      studio.deviceCalibration.safetyConfirmed = checked && identity.ok;
      if (checked && !identity.ok) {
        $("calibrationSafetyConfirm").checked = false;
        toast("安全确认尚不可用", identity.message, "warning");
      }
      if (!studio.deviceCalibration.safetyConfirmed) {
        cancelImuCalibration("安全确认已撤销。", { quiet: true });
      }
      renderCalibrationWizard();
    });

    [1, 2, 3].forEach((step) => {
      $( `calibrationStep${step}Tab` )?.addEventListener("click", () => setCalibrationStep(step, { focus: true }));
    });

    $("calibrationCenterAllBtn")?.addEventListener("click", centerAllForCalibration);
    $("calibrationOpenLimitsBtn")?.addEventListener("click", () => {
      const details = document.querySelector(".calibration-panel");
      if (!details) return;
      details.open = true;
      details.scrollIntoView({ behavior: "smooth", block: "center" });
      window.setTimeout(() => details.querySelector("input")?.focus(), 350);
    });
    $("calibrationCaptureHomeBtn")?.addEventListener("click", captureHomeCalibration);
    $("calibrationPreviewHomeBtn")?.addEventListener("click", () => {
      applyCalibratedHome({ allowPhysical: false, source: "calibration-home-preview" });
    });
    $("calibrationImuStartBtn")?.addEventListener("click", startImuCalibration);
    $("calibrationYawZeroBtn")?.addEventListener("click", zeroImuOrientation);
    $("calibrationBackBtn")?.addEventListener("click", () => {
      setCalibrationStep(studio.deviceCalibration.step - 1, { focus: true });
    });
    $("calibrationNextBtn")?.addEventListener("click", async () => {
      const step = studio.deviceCalibration.step;
      const completed = studio.deviceCalibration.snapshot?.completed_steps || {};
      if (step === 1) {
        if (completed.servo && servoCalibrationEqual(
          servoCalibrationFromUi(),
          studio.deviceCalibration.snapshot?.record?.servo_calibration,
        )) {
          setCalibrationStep(2, { focus: true });
        } else {
          await saveServoCalibrationStep();
        }
      } else if (step === 2) {
        if (completed.home) setCalibrationStep(3, { focus: true });
        else await captureHomeCalibration();
      } else {
        await startImuCalibration();
      }
    });
    $("calibrationExitBtn")?.addEventListener("click", () => {
      $("poseTitle")?.scrollIntoView({ behavior: "smooth", block: "start" });
      toast("校准向导仍可从页面顶部继续", "已保存的步骤不会丢失。", "info");
    });
  }

  function bindProjectControls() {
    $("projectName").addEventListener("input", () => {
      studio.project.name = safeText($("projectName").value, "四足动作工程", 64);
      markDirty("工程名称已修改");
    });
    $("importBtn").addEventListener("click", () => {
      if (studio.recording) {
        toast("录制进行中", "停止并保存后才能打开其他工程。", "warning");
        return;
      }
      $("projectImport").click();
    });
    $("projectImport").addEventListener("change", () => importProject($("projectImport").files?.[0]));
    $("exportBtn").addEventListener("click", exportProject);
    $("actionForm").addEventListener("submit", (event) => {
      event.preventDefault();
      if (event.submitter?.value === "cancel") {
        $("actionDialog").close();
        return;
      }
      commitActionDialog();
      $("actionDialog").close();
    });
  }

  function bindActionTemplateControls() {
    document.querySelectorAll("[data-action-template]").forEach((button) => {
      button.addEventListener("click", () => selectActionTemplate(button.dataset.actionTemplate));
    });
    ["gaitPattern", "gaitStroke", "gaitCadence", "gaitCycles", "gaitTurn"].forEach((id) => {
      $(id)?.addEventListener("input", renderGaitDialog);
      $(id)?.addEventListener("change", renderGaitDialog);
    });
  }

  function bindSafetyControls() {
    $("homeButton").addEventListener("click", sendHomePose);
    $("armButton").addEventListener("click", () => {
      if (studio.armed) setArmed(false);
      else {
        updatePowerInterlock();
        $("armDialog").showModal();
      }
    });
    $("estopButton").addEventListener("click", () => emergencyStop());
    const hold = $("holdToArm");
    hold.addEventListener("pointerdown", startHoldToArm);
    hold.addEventListener("pointerup", cancelHoldToArm);
    hold.addEventListener("pointercancel", cancelHoldToArm);
    hold.addEventListener("pointerleave", cancelHoldToArm);
    hold.addEventListener("keydown", (event) => {
      if (["Enter", " "].includes(event.key)) {
        event.preventDefault();
        startHoldToArm();
      }
    });
    hold.addEventListener("keyup", (event) => {
      if (["Enter", " "].includes(event.key)) cancelHoldToArm();
    });
    $("armDialog").addEventListener("close", cancelHoldToArm);
  }

  function bindGlobalControls() {
    $("shortcutBtn").addEventListener("click", () => $("shortcutDialog").showModal());
    document.addEventListener("pointermove", moveFrameDrag);
    document.addEventListener("pointerup", endFrameDrag);
    document.addEventListener("pointercancel", endFrameDrag);
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        emergencyStop();
        return;
      }
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
        event.preventDefault();
        saveProjectRemote();
        return;
      }
      // Do not reinterpret browser/OS shortcuts such as Ctrl+R as the
      // studio's single-key recording shortcut.
      if (event.ctrlKey || event.metaKey || event.altKey) return;
      if (isTypingTarget(event.target) || document.querySelector("dialog[open]")) return;
      if (event.code === "Space") {
        event.preventDefault();
        toggleOfflinePreview();
      } else if (event.key.toLowerCase() === "r") {
        event.preventDefault();
        toggleRecording();
      } else if (event.key === "Home") {
        event.preventDefault();
        applyCalibratedHome({ allowPhysical: true, source: "keyboard-home" });
      } else if (event.key === "Delete" || event.key === "Backspace") {
        if (studio.selectedFrameId) {
          event.preventDefault();
          deleteSelectedFrame();
        }
      }
    }, true);

    window.addEventListener("pagehide", () => {
      window.clearInterval(studio.imuPollTimer);
      window.clearTimeout(studio.deviceCalibration.refreshTimer);
      window.clearTimeout(studio.deviceCalibration.profileSyncTimer);
      window.clearTimeout(studio.ledCalibration.refreshTimer);
      cancelImuCalibration("", { quiet: true });
      stopOfflinePreview({ keepPosition: true });
      studio.dogViewerDisposed = true;
      studio.dogViewer?.dispose();
      studio.dogViewer = null;
      if (!studio.armed) return;
      const body = new Blob(["{}"], { type: "application/json" });
      navigator.sendBeacon?.(API.estop, body);
    });
  }

  async function initialize() {
    const hadLocal = loadLocalProject();
    studio.projectInitialized = hadLocal;
    if (!studio.project.actions.some((action) => action.id === studio.activeActionId)) {
      studio.activeActionId = studio.project.actions[0].id;
    }
    bindServoControls();
    bindSafePostureControls();
    bindMotionControls();
    bindLightControls();
    bindTimelineControls();
    bindImuControls();
    bindCalibrationControls();
    bindProjectControls();
    bindActionTemplateControls();
    bindSafetyControls();
    bindGlobalControls();
    renderProject();
    renderDeviceState();
    renderArmState();
    renderRecordingState();
    renderPlaybackState();
    renderPreviewState();
    renderImuTelemetry();
    renderCalibrationWizard();
    renderLedCalibration();
    initializeDogViewer();
    $("connectionBanner").classList.add("show");
    connectEvents();
    const online = await getState();
    if (online) {
      await loadRemoteProject();
      await refreshLedCalibration({ quiet: true });
      // A project file is portable and may contain another board's profile.
      // Keep conservative limits until the current connection epoch's record
      // has been fetched and proven applicable to this exact chip_id.
      studio.project.calibration = defaultCalibration();
      renderCalibration();
      renderPose();
    }
    if (online) {
      const calibration = await refreshDeviceCalibration({ quiet: true });
      if (calibration) applyCalibrationSnapshot(calibration, { syncUi: true });
    }
    studio.pollTimer = window.setInterval(getState, 1800);
    await pollImu();
    // GET /api/imu only reads the backend cache and never opens another ADB
    // shell. Match the 10 Hz device worker for responsive 3D motion.
    studio.imuPollTimer = window.setInterval(pollImu, 100);
  }

  initialize().catch((error) => {
    console.error(error);
    setServerOffline(error.message || String(error));
    toast("运动工作站启动失败", error.message || String(error), "error", 10000);
  });
})();
