import * as THREE from "./vendor/three/three.module.min.js";
import { GLTFLoader } from "./vendor/three/addons/loaders/GLTFLoader.js";

const DEFAULT_MODEL_URL = new URL("./models/unitree-go1-animated.glb", import.meta.url).href;
// The bundled GLB's geometric head points opposite Motion Studio's +X body
// forward convention. Correct the imported bind pose once; live IMU yaw stays
// on bodyPoseRoot and must not carry this fixed model-only adjustment.
const MODEL_HEADING_CORRECTION_RAD = Math.PI;

export const DOG_VIEWER_READY_EVENT = "dog-viewer:ready";
export const DOG_VIEWER_FALLBACK_EVENT = "dog-viewer:fallback";

const FRAME_LINKS = [
  ["BodyBone", "HipFLBone"], ["HipFLBone", "ThighFLBone"], ["ThighFLBone", "CalfFLBone"],
  ["BodyBone", "HipFRBone"], ["HipFRBone", "ThighFRBone"], ["ThighFRBone", "CalfFRBone"],
  ["BodyBone", "HipBLBone"], ["HipBLBone", "ThighBLBone"], ["ThighBLBone", "CalfBLBone"],
  ["BodyBone", "HipBRBone"], ["HipBRBone", "ThighBRBone"], ["ThighBRBone", "CalfBRBone"],
  ["CalfFLBone", "FootFLBone"], ["CalfFRBone", "FootFRBone"],
  ["CalfBLBone", "FootBLBone"], ["CalfBRBone", "FootBRBone"],
];
const FOOT_NODES = ["FootFLBone", "FootFRBone", "FootBLBone", "FootBRBone"];
const SECONDARY_JOINTS = [
  "ThighFLBone", "CalfFLBone", "ThighFRBone", "CalfFRBone",
  "ThighBLBone", "CalfBLBone", "ThighBRBone", "CalfBRBone",
];

function buildHolographicFrame(model, poseRoot, groundParent = poseRoot.parent || poseRoot) {
  const modelMeshes = [];
  model.traverse((object) => {
    if (object.isMesh) modelMeshes.push(object);
  });

  // The GLB is still the exact animated Go1 geometry, but none of its product
  // textures are rendered. Shared edge geometry keeps the four repeated legs
  // light enough for embedded-browser/GPU use.
  const edgeGeometryCache = new Map();
  const surfaceMaterials = [];
  const edgeMaterials = [];
  modelMeshes.forEach((object, index) => {
    const isBody = /trunk|body/i.test(object.name);
    const isRear = /(?:BL|BR)$/i.test(object.name);
    const surface = new THREE.MeshBasicMaterial({
      color: isBody ? 0x073238 : isRear ? 0x15142d : 0x071a2a,
      transparent: true,
      opacity: isBody ? 0.18 : 0.12,
      depthWrite: false,
      side: THREE.DoubleSide,
    });
    surface.userData.baseOpacity = surface.opacity;
    surface.userData.phase = index * 0.47;
    object.material = surface;
    object.castShadow = false;
    object.receiveShadow = false;
    object.renderOrder = 1;
    surfaceMaterials.push(surface);

    let edgeGeometry = edgeGeometryCache.get(object.geometry);
    if (!edgeGeometry) {
      edgeGeometry = new THREE.EdgesGeometry(object.geometry, 28);
      edgeGeometryCache.set(object.geometry, edgeGeometry);
    }
    const edgeMaterial = new THREE.LineBasicMaterial({
      color: isBody ? 0x42e1d4 : isRear ? 0x776ee8 : 0x448fd8,
      transparent: true,
      opacity: isBody ? 0.72 : 0.54,
      blending: THREE.NormalBlending,
      depthWrite: false,
      toneMapped: false,
    });
    edgeMaterial.userData.baseOpacity = edgeMaterial.opacity;
    edgeMaterial.userData.phase = index * 0.31;
    const edges = new THREE.LineSegments(edgeGeometry, edgeMaterial);
    edges.name = `${object.name || "Go1Part"}TechnicalEdges`;
    edges.renderOrder = 3;
    object.add(edges);
    edgeMaterials.push(edgeMaterial);
  });

  const resolvedLinks = FRAME_LINKS.map(([startName, endName]) => [
    model.getObjectByName(startName),
    model.getObjectByName(endName),
  ]).filter(([startNode, endNode]) => startNode && endNode);

  const jointNames = new Set(resolvedLinks.flatMap(([startNode, endNode]) => [startNode.name, endNode.name]));
  const jointGeometry = new THREE.SphereGeometry(0.021, 10, 8);
  const jointMaterial = new THREE.MeshBasicMaterial({
    color: 0x72fff3,
    transparent: true,
    opacity: 0.86,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const jointData = Array.from(jointNames).map((jointName, instanceIndex) => ({
    node: model.getObjectByName(jointName),
    scale: jointName === "BodyBone" ? 1.55 : jointName.startsWith("Hip") ? 1.25 : jointName.startsWith("Foot") ? 0.72 : 1,
    instanceIndex,
  })).filter(({ node }) => node);
  const jointBeacons = new THREE.InstancedMesh(jointGeometry, jointMaterial, jointData.length);
  jointBeacons.name = "Go1JointBeacons";
  jointBeacons.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  jointBeacons.frustumCulled = false;
  jointBeacons.renderOrder = 5;
  poseRoot.add(jointBeacons);

  const ringGeometry = new THREE.TorusGeometry(0.043, 0.0038, 6, 24);
  const ringMaterial = new THREE.MeshBasicMaterial({
    color: 0x56c9ff,
    transparent: true,
    opacity: 0.62,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const primaryRings = ["BodyBone", "HipFLBone", "HipFRBone", "HipBLBone", "HipBRBone"]
    .map((jointName, instanceIndex) => ({
      node: model.getObjectByName(jointName),
      baseScale: jointName === "BodyBone" ? 1.7 : 1,
      phase: instanceIndex * 0.8,
      instanceIndex,
    })).filter(({ node }) => node);
  const primaryRingInstances = new THREE.InstancedMesh(ringGeometry, ringMaterial, primaryRings.length);
  primaryRingInstances.name = "Go1PrimaryJointRings";
  primaryRingInstances.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  primaryRingInstances.frustumCulled = false;
  primaryRingInstances.renderOrder = 4;
  poseRoot.add(primaryRingInstances);

  // A small number of authored primitives adds a readable "power system"
  // without particle clouds or post-processing passes. Everything is parented
  // to BodyBone, so the core and scan shell follow the real idle animation.
  const bodyNode = model.getObjectByName("BodyBone");
  const coreGroup = new THREE.Group();
  coreGroup.name = "Go1EnergyCore";
  coreGroup.position.y = 0.018;
  bodyNode.add(coreGroup);

  const coreWireMaterial = new THREE.MeshBasicMaterial({
    color: 0x78eaff,
    wireframe: true,
    transparent: true,
    opacity: 0.72,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const coreWire = new THREE.Mesh(new THREE.OctahedronGeometry(0.048, 1), coreWireMaterial);
  coreWire.renderOrder = 7;
  coreGroup.add(coreWire);

  const coreInnerMaterial = new THREE.MeshBasicMaterial({
    color: 0x9d78ff,
    transparent: true,
    opacity: 0.82,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const coreInner = new THREE.Mesh(new THREE.SphereGeometry(0.022, 10, 8), coreInnerMaterial);
  coreInner.renderOrder = 8;
  coreGroup.add(coreInner);

  const coreRingGeometry = new THREE.TorusGeometry(0.066, 0.0028, 5, 28);
  const coreRingCyanMaterial = new THREE.MeshBasicMaterial({
    color: 0x55fff0,
    transparent: true,
    opacity: 0.68,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const coreRingVioletMaterial = coreRingCyanMaterial.clone();
  coreRingVioletMaterial.color.setHex(0x8b6dff);
  coreRingVioletMaterial.opacity = 0.5;
  const coreRingCyan = new THREE.Mesh(coreRingGeometry, coreRingCyanMaterial);
  const coreRingViolet = new THREE.Mesh(coreRingGeometry, coreRingVioletMaterial);
  coreRingCyan.renderOrder = 7;
  coreRingViolet.renderOrder = 7;
  coreGroup.add(coreRingCyan, coreRingViolet);

  const spinePoints = [];
  const spineColors = [];
  const spineCyan = new THREE.Color(0x61fff2);
  const spineViolet = new THREE.Color(0x8a6bff);
  const spineSegments = 9;
  const spineHeight = (ratio) => 0.068 + (1 - Math.pow(ratio * 2 - 1, 2)) * 0.022;
  for (let index = 0; index < spineSegments; index += 1) {
    const ratio = index / (spineSegments - 1);
    spinePoints.push(new THREE.Vector3(0, spineHeight(ratio), -0.17 + ratio * 0.34));
    const color = spineViolet.clone().lerp(spineCyan, ratio);
    spineColors.push(color.r, color.g, color.b);
  }
  const spineGeometry = new THREE.BufferGeometry().setFromPoints(spinePoints);
  spineGeometry.setAttribute("color", new THREE.Float32BufferAttribute(spineColors, 3));
  const spineMaterial = new THREE.LineBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.76,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const spine = new THREE.Line(spineGeometry, spineMaterial);
  spine.name = "Go1EnergySpine";
  spine.renderOrder = 6;
  bodyNode.add(spine);

  const tetherGeometry = new THREE.BufferGeometry().setFromPoints([
    new THREE.Vector3(0, 0.018, 0),
    new THREE.Vector3(0, spineHeight(0.5), 0),
  ]);
  const coreTether = new THREE.Line(tetherGeometry, spineMaterial);
  coreTether.renderOrder = 6;
  bodyNode.add(coreTether);

  const vertebraMaterial = new THREE.MeshBasicMaterial({
    color: 0xffffff,
    transparent: true,
    opacity: 0.74,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const vertebrae = new THREE.InstancedMesh(new THREE.SphereGeometry(0.01, 7, 5), vertebraMaterial, spineSegments);
  const vertebraMatrix = new THREE.Matrix4();
  for (let index = 0; index < spineSegments; index += 1) {
    const ratio = index / (spineSegments - 1);
    vertebraMatrix.makeTranslation(0, spineHeight(ratio), -0.17 + ratio * 0.34);
    vertebrae.setMatrixAt(index, vertebraMatrix);
    vertebrae.setColorAt(index, spineViolet.clone().lerp(spineCyan, ratio));
  }
  vertebrae.instanceMatrix.needsUpdate = true;
  vertebrae.renderOrder = 7;
  bodyNode.add(vertebrae);

  const spinePulseMaterial = new THREE.MeshBasicMaterial({
    color: 0xb3fff8,
    transparent: true,
    opacity: 0.9,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const spinePulse = new THREE.Mesh(new THREE.SphereGeometry(0.017, 8, 6), spinePulseMaterial);
  spinePulse.position.y = spineHeight(0.5);
  spinePulse.renderOrder = 8;
  bodyNode.add(spinePulse);

  const hullPositions = [];
  const hullColors = [];
  const hullLevels = 5;
  const hullSegments = 40;
  for (let level = 0; level < hullLevels; level += 1) {
    const levelRatio = level / (hullLevels - 1);
    const y = -0.054 + levelRatio * 0.126;
    const taper = 0.74 + (1 - Math.abs(levelRatio * 2 - 1)) * 0.26;
    const color = spineViolet.clone().lerp(spineCyan, levelRatio);
    for (let segment = 0; segment < hullSegments; segment += 1) {
      const startAngle = segment / hullSegments * Math.PI * 2;
      const endAngle = (segment + 1) / hullSegments * Math.PI * 2;
      hullPositions.push(
        Math.cos(startAngle) * 0.135 * taper, y, Math.sin(startAngle) * 0.255 * taper,
        Math.cos(endAngle) * 0.135 * taper, y, Math.sin(endAngle) * 0.255 * taper
      );
      hullColors.push(color.r, color.g, color.b, color.r, color.g, color.b);
    }
  }
  const hullGeometry = new THREE.BufferGeometry();
  hullGeometry.setAttribute("position", new THREE.Float32BufferAttribute(hullPositions, 3));
  hullGeometry.setAttribute("color", new THREE.Float32BufferAttribute(hullColors, 3));
  const hullMaterial = new THREE.LineBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.24,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const hullSlices = new THREE.LineSegments(hullGeometry, hullMaterial);
  hullSlices.name = "Go1LayeredHologramShell";
  hullSlices.renderOrder = 2;
  bodyNode.add(hullSlices);

  const scanPoints = [];
  for (let segment = 0; segment < 48; segment += 1) {
    const angle = segment / 48 * Math.PI * 2;
    scanPoints.push(new THREE.Vector3(Math.cos(angle) * 0.135, 0, Math.sin(angle) * 0.255));
  }
  const scanMaterial = new THREE.LineBasicMaterial({
    color: 0x75eaff,
    transparent: true,
    opacity: 0.52,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const scanSlice = new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints(scanPoints), scanMaterial);
  scanSlice.name = "Go1ActiveScanSlice";
  scanSlice.renderOrder = 6;
  bodyNode.add(scanSlice);

  const secondaryRingGeometry = new THREE.TorusGeometry(0.035, 0.0026, 5, 20);
  const secondaryCyanMaterial = ringMaterial.clone();
  secondaryCyanMaterial.color.setHex(0x42cfff);
  secondaryCyanMaterial.opacity = 0.42;
  const secondaryVioletMaterial = ringMaterial.clone();
  secondaryVioletMaterial.color.setHex(0x876cff);
  secondaryVioletMaterial.opacity = 0.46;
  const secondaryRings = SECONDARY_JOINTS.map((jointName, index) => ({
    node: model.getObjectByName(jointName),
    isRear: /B/.test(jointName),
    baseScale: jointName.startsWith("Calf") ? 0.64 : 0.82,
    phase: index * 0.65,
    instanceIndex: 0,
    instances: null,
  })).filter(({ node }) => node);
  const makeRingInstances = (entries, material) => {
    const instances = new THREE.InstancedMesh(secondaryRingGeometry, material, entries.length);
    instances.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    instances.frustumCulled = false;
    instances.renderOrder = 5;
    entries.forEach((entry, instanceIndex) => {
      entry.instances = instances;
      entry.instanceIndex = instanceIndex;
    });
    poseRoot.add(instances);
    return instances;
  };
  const secondaryCyanInstances = makeRingInstances(secondaryRings.filter(({ isRear }) => !isRear), secondaryCyanMaterial);
  const secondaryVioletInstances = makeRingInstances(secondaryRings.filter(({ isRear }) => isRear), secondaryVioletMaterial);

  const resolvedFeet = FOOT_NODES.map((name, index) => ({ name, index, node: model.getObjectByName(name) }))
    .filter(({ node }) => node);
  const footColors = [0x56fff0, 0x4cb8ff, 0x9a6cff, 0x716eff].map((color) => new THREE.Color(color));
  const trailLength = 10;
  const trailMaterial = new THREE.LineBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.58,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const footTrails = resolvedFeet.map(({ node }) => ({
    node,
    history: new Float32Array(trailLength * 3),
    ready: false,
  }));
  const trailPositions = new Float32Array(resolvedFeet.length * (trailLength - 1) * 6);
  const trailColors = new Float32Array(trailPositions.length);
  resolvedFeet.forEach(({ index }, footIndex) => {
    for (let segment = 0; segment < trailLength - 1; segment += 1) {
      const offset = (footIndex * (trailLength - 1) + segment) * 6;
      const startIntensity = 0.05 + Math.pow(segment / (trailLength - 1), 1.4) * 0.8;
      const endIntensity = 0.05 + Math.pow((segment + 1) / (trailLength - 1), 1.4) * 0.8;
      trailColors.set([
        footColors[index].r * startIntensity, footColors[index].g * startIntensity, footColors[index].b * startIntensity,
        footColors[index].r * endIntensity, footColors[index].g * endIntensity, footColors[index].b * endIntensity,
      ], offset);
    }
  });
  const trailGeometry = new THREE.BufferGeometry();
  trailGeometry.setAttribute("position", new THREE.BufferAttribute(trailPositions, 3));
  trailGeometry.setAttribute("color", new THREE.BufferAttribute(trailColors, 3));
  const trailLines = new THREE.LineSegments(trailGeometry, trailMaterial);
  trailLines.frustumCulled = false;
  trailLines.renderOrder = 3;
  poseRoot.add(trailLines);

  const groundY = -0.715;
  const groundLayer = new THREE.Group();
  groundLayer.name = "Go1GroundFeedback";
  groundParent.add(groundLayer);
  const footprintGeometry = new THREE.RingGeometry(0.064, 0.094, 28);
  footprintGeometry.rotateX(-Math.PI / 2);
  const footprintMaterial = new THREE.MeshBasicMaterial({
    color: 0xffffff,
    transparent: true,
    opacity: 0.5,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    side: THREE.DoubleSide,
    toneMapped: false,
  });
  const footprints = new THREE.InstancedMesh(footprintGeometry, footprintMaterial, resolvedFeet.length);
  footprints.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  footprints.frustumCulled = false;
  footprints.renderOrder = 2;
  groundLayer.add(footprints);

  const contactPositions = new Float32Array(resolvedFeet.length * 6);
  const contactColors = new Float32Array(resolvedFeet.length * 6);
  resolvedFeet.forEach(({ index }, footIndex) => {
    const color = footColors[index];
    contactColors.set([color.r * 0.15, color.g * 0.15, color.b * 0.15, color.r, color.g, color.b], footIndex * 6);
  });
  const contactGeometry = new THREE.BufferGeometry();
  contactGeometry.setAttribute("position", new THREE.BufferAttribute(contactPositions, 3));
  contactGeometry.setAttribute("color", new THREE.BufferAttribute(contactColors, 3));
  const contactMaterial = new THREE.LineBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.18,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const contactBeams = new THREE.LineSegments(contactGeometry, contactMaterial);
  contactBeams.frustumCulled = false;
  contactBeams.renderOrder = 1;
  groundLayer.add(contactBeams);

  const groundAuraMaterial = new THREE.MeshBasicMaterial({
    color: 0x7868ff,
    transparent: true,
    opacity: 0.09,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    side: THREE.DoubleSide,
    toneMapped: false,
  });
  const groundAura = new THREE.Mesh(new THREE.RingGeometry(0.49, 0.505, 64), groundAuraMaterial);
  groundAura.rotation.x = -Math.PI / 2;
  groundAura.position.y = groundY - 0.003;
  groundAura.scale.z = 0.72;
  groundAura.renderOrder = 1;
  groundLayer.add(groundAura);

  const skeletonPositions = new Float32Array(resolvedLinks.length * 6);
  const skeletonGeometry = new THREE.BufferGeometry();
  skeletonGeometry.setAttribute("position", new THREE.BufferAttribute(skeletonPositions, 3));
  const skeletonMaterial = new THREE.LineBasicMaterial({
    color: 0x43dff8,
    transparent: true,
    opacity: 0.46,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  const skeleton = new THREE.LineSegments(skeletonGeometry, skeletonMaterial);
  skeleton.name = "Go1LiveKinematicFrame";
  skeleton.frustumCulled = false;
  skeleton.renderOrder = 4;
  poseRoot.add(skeleton);

  const inversePoseMatrix = new THREE.Matrix4();
  const point = new THREE.Vector3();
  const worldPoint = new THREE.Vector3();
  const projectionPosition = new THREE.Vector3();
  const projectionScale = new THREE.Vector3();
  const projectionQuaternion = new THREE.Quaternion();
  const projectionMatrix = new THREE.Matrix4();
  const projectionColor = new THREE.Color();
  const ringPosition = new THREE.Vector3();
  const ringScale = new THREE.Vector3();
  const ringEuler = new THREE.Euler();
  const ringQuaternion = new THREE.Quaternion();
  const ringLocalMatrix = new THREE.Matrix4();
  const ringInstanceMatrix = new THREE.Matrix4();
  let lastTrailSample = Number.NEGATIVE_INFINITY;
  function updateDynamicFrame(elapsed) {
    poseRoot.updateMatrixWorld(true);
    inversePoseMatrix.copy(poseRoot.matrixWorld).invert();
    ringQuaternion.identity();
    jointData.forEach((joint) => {
      ringScale.setScalar(joint.scale);
      ringLocalMatrix.compose(ringPosition, ringQuaternion, ringScale);
      ringInstanceMatrix.multiplyMatrices(inversePoseMatrix, joint.node.matrixWorld).multiply(ringLocalMatrix);
      jointBeacons.setMatrixAt(joint.instanceIndex, ringInstanceMatrix);
    });
    jointBeacons.instanceMatrix.needsUpdate = true;
    primaryRings.forEach((ring, index) => {
      const scale = ring.baseScale * (1 + Math.sin(elapsed * 2 + ring.phase) * 0.08);
      ringScale.setScalar(scale);
      ringEuler.set(Math.PI / 2, 0, elapsed * (index % 2 ? -0.32 : 0.32));
      ringQuaternion.setFromEuler(ringEuler);
      ringLocalMatrix.compose(ringPosition, ringQuaternion, ringScale);
      ringInstanceMatrix.multiplyMatrices(inversePoseMatrix, ring.node.matrixWorld).multiply(ringLocalMatrix);
      primaryRingInstances.setMatrixAt(ring.instanceIndex, ringInstanceMatrix);
    });
    primaryRingInstances.instanceMatrix.needsUpdate = true;
    secondaryRings.forEach((ring) => {
      const scale = ring.baseScale * (1 + Math.sin(elapsed * 2.4 + ring.phase) * 0.1);
      ringScale.setScalar(scale);
      ringEuler.set(Math.PI / 2, 0, elapsed * 0.28 + ring.phase);
      ringQuaternion.setFromEuler(ringEuler);
      ringLocalMatrix.compose(ringPosition, ringQuaternion, ringScale);
      ringInstanceMatrix.multiplyMatrices(inversePoseMatrix, ring.node.matrixWorld).multiply(ringLocalMatrix);
      ring.instances.setMatrixAt(ring.instanceIndex, ringInstanceMatrix);
    });
    secondaryCyanInstances.instanceMatrix.needsUpdate = true;
    secondaryVioletInstances.instanceMatrix.needsUpdate = true;
    resolvedLinks.forEach(([startNode, endNode], linkIndex) => {
      point.setFromMatrixPosition(startNode.matrixWorld).applyMatrix4(inversePoseMatrix);
      skeletonPositions.set([point.x, point.y, point.z], linkIndex * 6);
      point.setFromMatrixPosition(endNode.matrixWorld).applyMatrix4(inversePoseMatrix);
      skeletonPositions.set([point.x, point.y, point.z], linkIndex * 6 + 3);
    });
    skeletonGeometry.attributes.position.needsUpdate = true;

    const sampleTrail = elapsed - lastTrailSample >= 0.065;
    let trailChanged = false;
    footTrails.forEach((trail) => {
      point.setFromMatrixPosition(trail.node.matrixWorld).applyMatrix4(inversePoseMatrix);
      if (!trail.ready) {
        for (let sample = 0; sample < trailLength; sample += 1) {
          const offset = sample * 3;
          trail.history[offset] = point.x;
          trail.history[offset + 1] = point.y;
          trail.history[offset + 2] = point.z;
        }
        trail.ready = true;
        trailChanged = true;
      } else if (sampleTrail) {
        trail.history.copyWithin(0, 3);
        trail.history[trail.history.length - 3] = point.x;
        trail.history[trail.history.length - 2] = point.y;
        trail.history[trail.history.length - 1] = point.z;
        trailChanged = true;
      }
    });
    if (trailChanged) {
      footTrails.forEach((trail, footIndex) => {
        for (let segment = 0; segment < trailLength - 1; segment += 1) {
          const sourceOffset = segment * 3;
          const targetOffset = (footIndex * (trailLength - 1) + segment) * 6;
          trailPositions[targetOffset] = trail.history[sourceOffset];
          trailPositions[targetOffset + 1] = trail.history[sourceOffset + 1];
          trailPositions[targetOffset + 2] = trail.history[sourceOffset + 2];
          trailPositions[targetOffset + 3] = trail.history[sourceOffset + 3];
          trailPositions[targetOffset + 4] = trail.history[sourceOffset + 4];
          trailPositions[targetOffset + 5] = trail.history[sourceOffset + 5];
        }
      });
      trailGeometry.attributes.position.needsUpdate = true;
    }
    if (sampleTrail) lastTrailSample = elapsed;

    resolvedFeet.forEach(({ index, node }, footIndex) => {
      worldPoint.setFromMatrixPosition(node.matrixWorld);
      const contact = Math.max(0, 1 - Math.abs(worldPoint.y - groundY) / 0.2);
      const pulse = 0.86 + contact * 0.28 + Math.sin(elapsed * 2.2 + footIndex * 1.3) * 0.06;
      projectionPosition.set(worldPoint.x, groundY, worldPoint.z);
      projectionScale.setScalar(pulse);
      projectionMatrix.compose(projectionPosition, projectionQuaternion, projectionScale);
      footprints.setMatrixAt(footIndex, projectionMatrix);
      projectionColor.copy(footColors[index]).multiplyScalar(0.18 + contact * 0.82);
      footprints.setColorAt(footIndex, projectionColor);
      contactPositions.set([
        worldPoint.x, groundY, worldPoint.z,
        worldPoint.x, worldPoint.y, worldPoint.z,
      ], footIndex * 6);
    });
    footprints.instanceMatrix.needsUpdate = true;
    if (footprints.instanceColor) footprints.instanceColor.needsUpdate = true;
    contactGeometry.attributes.position.needsUpdate = true;
  }

  function animate(elapsed) {
    for (const material of surfaceMaterials) {
      material.opacity = material.userData.baseOpacity * (0.94 + Math.sin(elapsed * 1.45 + material.userData.phase) * 0.06);
    }
    for (const material of edgeMaterials) {
      material.opacity = material.userData.baseOpacity * (0.92 + Math.sin(elapsed * 1.8 + material.userData.phase) * 0.08);
    }
    jointMaterial.opacity = 0.76 + Math.sin(elapsed * 2.6) * 0.1;
    ringMaterial.opacity = 0.54 + Math.sin(elapsed * 1.7) * 0.08;
    skeletonMaterial.opacity = 0.4 + Math.sin(elapsed * 1.35) * 0.07;
    coreWire.rotation.x = elapsed * 0.42;
    coreWire.rotation.y = elapsed * 0.68;
    coreWire.rotation.z = elapsed * 0.24;
    const corePulse = 1 + Math.sin(elapsed * 2.4) * 0.12;
    coreInner.scale.setScalar(corePulse);
    coreWireMaterial.opacity = 0.64 + Math.sin(elapsed * 2.1) * 0.12;
    coreInnerMaterial.opacity = 0.72 + Math.sin(elapsed * 2.7 + 0.8) * 0.14;
    coreRingCyan.rotation.set(elapsed * 0.46, 0.34, elapsed * 0.31);
    coreRingViolet.rotation.set(Math.PI / 2 + elapsed * 0.22, -elapsed * 0.38, 0.72);
    coreRingCyanMaterial.opacity = 0.58 + Math.sin(elapsed * 1.9) * 0.12;
    coreRingVioletMaterial.opacity = 0.42 + Math.sin(elapsed * 1.6 + 1.2) * 0.1;
    spineMaterial.opacity = 0.67 + Math.sin(elapsed * 1.55) * 0.11;
    vertebraMaterial.opacity = 0.64 + Math.sin(elapsed * 2.0 + 0.5) * 0.12;
    const spinePulseRatio = (Math.sin(elapsed * 0.95) + 1) * 0.5;
    spinePulse.position.z = -0.17 + spinePulseRatio * 0.34;
    spinePulse.position.y = spineHeight(spinePulseRatio);
    spinePulseMaterial.opacity = 0.72 + Math.sin(elapsed * 3.1) * 0.18;
    const scanRatio = (Math.sin(elapsed * 0.78) + 1) * 0.5;
    const scanTaper = 0.74 + (1 - Math.abs(scanRatio * 2 - 1)) * 0.26;
    scanSlice.position.y = -0.054 + scanRatio * 0.126;
    scanSlice.scale.set(scanTaper, 1, scanTaper);
    scanMaterial.color.setHSL(0.49 + scanRatio * 0.16, 0.9, 0.69);
    scanMaterial.opacity = 0.42 + Math.sin(elapsed * 2.2) * 0.1;
    hullMaterial.opacity = 0.2 + Math.sin(elapsed * 1.1 + 0.6) * 0.05;
    secondaryCyanMaterial.opacity = 0.34 + Math.sin(elapsed * 1.8) * 0.08;
    secondaryVioletMaterial.opacity = 0.38 + Math.sin(elapsed * 1.6 + 1) * 0.08;
    trailMaterial.opacity = 0.48 + Math.sin(elapsed * 1.3) * 0.08;
    contactMaterial.opacity = 0.14 + Math.sin(elapsed * 1.45 + 0.4) * 0.045;
    footprintMaterial.opacity = 0.42 + Math.sin(elapsed * 1.7) * 0.09;
    const auraPulse = 1 + Math.sin(elapsed * 0.9) * 0.035;
    groundAura.scale.set(auraPulse, 0.72 * auraPulse, 1);
    groundAuraMaterial.opacity = 0.075 + Math.sin(elapsed * 1.1 + 0.7) * 0.022;
    updateDynamicFrame(elapsed);
  }

  return { animate };
}

const LEG_CHANNELS = Object.freeze([
  Object.freeze({ code: "FL", bone: "ThighFLBone", direction: 1 }),
  Object.freeze({ code: "FR", bone: "ThighFRBone", direction: -1 }),
  Object.freeze({ code: "RR", bone: "ThighBRBone", direction: -1 }),
  Object.freeze({ code: "RL", bone: "ThighBLBone", direction: 1 }),
]);

const ACTIVE_PLAYBACK_STATES = new Set([
  "play",
  "playing",
  "preview",
  "previewing",
  "record",
  "recording",
  "running",
]);

function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

function finiteNumber(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function finiteOrientationNumber(value) {
  if (value === null || typeof value === "boolean") return null;
  if (typeof value === "string" && value.trim() === "") return null;
  try {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  } catch {
    return null;
  }
}

function wrapDegrees(value) {
  const wrapped = ((value + 180) % 360 + 360) % 360 - 180;
  return wrapped === -180 && value > 0 ? 180 : wrapped;
}

function normalizeHexColor(value, fallback = "#35A7FF") {
  if (Array.isArray(value) && value.length >= 3) {
    const channels = value.slice(0, 3).map((channel) => clamp(Math.round(finiteNumber(channel, 0)), 0, 255));
    return `#${channels.map((channel) => channel.toString(16).padStart(2, "0")).join("").toUpperCase()}`;
  }
  const text = String(value ?? "").trim();
  const short = /^#?([0-9a-f]{3})$/i.exec(text);
  if (short) {
    const expanded = [...short[1]].map((digit) => `${digit}${digit}`).join("");
    return `#${expanded.toUpperCase()}`;
  }
  const full = /^#?([0-9a-f]{6})$/i.exec(text);
  return full ? `#${full[1].toUpperCase()}` : fallback;
}

function normalizePlaybackState(value) {
  return String(value ?? "idle").trim().toLowerCase() || "idle";
}

function isPlaybackActive(value) {
  return ACTIVE_PLAYBACK_STATES.has(normalizePlaybackState(value));
}

function disposeObjectTree(root) {
  if (!root) return;
  const geometries = new Set();
  const materials = new Set();
  const textures = new Set();
  root.traverse((object) => {
    if (object.geometry) geometries.add(object.geometry);
    const objectMaterials = Array.isArray(object.material) ? object.material : [object.material];
    objectMaterials.filter(Boolean).forEach((material) => {
      materials.add(material);
      for (const value of Object.values(material)) {
        if (value?.isTexture) textures.add(value);
      }
    });
  });
  textures.forEach((texture) => texture.dispose());
  materials.forEach((material) => material.dispose());
  geometries.forEach((geometry) => geometry.dispose());
}

function createViewerSurface(mount) {
  const root = document.createElement("div");
  root.className = "dog-viewer";
  root.dataset.loading = "true";
  root.style.position = "relative";
  root.style.width = "100%";
  root.style.height = "100%";
  root.style.minHeight = "240px";
  root.style.overflow = "hidden";

  const canvas = document.createElement("canvas");
  canvas.className = "dog-viewer__canvas";
  canvas.setAttribute("aria-label", "四路 PWM 姿态驱动的开源机器狗三维预览");
  canvas.style.position = "absolute";
  canvas.style.inset = "0";
  canvas.style.display = "block";
  canvas.style.width = "100%";
  canvas.style.height = "100%";
  canvas.style.touchAction = "none";

  const grid = document.createElement("div");
  grid.className = "dog-viewer__grid";
  grid.setAttribute("aria-hidden", "true");

  const status = document.createElement("div");
  status.className = "dog-viewer__status";
  status.setAttribute("role", "status");
  status.setAttribute("aria-live", "polite");
  status.innerHTML = '<i aria-hidden="true"></i><span>LOADING 3D MODEL</span>';

  const channels = document.createElement("div");
  channels.className = "dog-viewer__channels";
  channels.setAttribute("aria-hidden", "true");
  channels.innerHTML = LEG_CHANNELS.map((leg, index) => (
    `<span data-dog-leg="${index}"><b>${leg.code}</b><small>1500</small></span>`
  )).join("");

  const credit = document.createElement("div");
  credit.className = "dog-viewer__credit";
  credit.innerHTML = '3D <strong>C. Yamahata</strong> · CC BY 4.0';

  const fallback = document.createElement("div");
  fallback.className = "dog-viewer__fallback";
  fallback.hidden = true;
  fallback.innerHTML = `
    <svg viewBox="0 0 260 150" aria-hidden="true">
      <g fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
        <path d="M68 52h108l22 18-10 25H72L53 75z"/><path d="M176 55l30-17 16 8-15 31"/>
        <path d="M82 93l-13 37M105 94l-5 36M165 94l7 36M187 92l23 36"/>
        <circle cx="69" cy="131" r="5"/><circle cx="99" cy="131" r="5"/>
        <circle cx="173" cy="131" r="5"/><circle cx="211" cy="129" r="5"/>
        <path d="M54 72 35 59"/><circle cx="200" cy="51" r="2" fill="currentColor"/>
      </g>
    </svg>
    <strong>3D PREVIEW UNAVAILABLE</strong>
    <span>舵机与动作控制不受影响</span>`;

  root.append(grid, canvas, status, channels, credit, fallback);
  mount.append(root);
  return { root, canvas, status, statusText: status.querySelector("span"), channels, fallback };
}

/**
 * Mount the local Unitree Go1 reference model into a Motion Studio element.
 *
 * The model is a visual preview only. It deliberately does not attempt to be a
 * kinematics solver for the connected four-servo mechanism.
 *
 * @param {Element|string} container Element (or selector) that receives the viewer.
 * @param {object} [options]
 * @param {string} [options.modelUrl] Override the bundled GLB URL.
 * @param {number[]} [options.centerUs] Servo centre pulses; defaults to 1500 us.
 * @param {number} [options.minUs=500] Visual input clamp.
 * @param {number} [options.maxUs=2500] Visual input clamp.
 * @param {number} [options.maxJointDegrees=52] Maximum rendered joint offset.
 * @param {number} [options.orientationStaleMs=1500] Time without a fresh IMU sample before data is stale; 0 disables the timeout.
 * @param {number} [options.orientationResponse=10] Body-orientation smoothing response in 1/s.
 * @param {boolean} [options.autoRotate=true] Enable restrained idle camera drift.
 * @param {object} [options.initialState] Initial value accepted by update().
 * @param {Function} [options.onReady] Called after the GLB is available.
 * @param {Function} [options.onFallback] Called if WebGL or the model fails.
 * @returns {{update: Function, setActualOrientation: Function, resetActualOrientation: Function, resize: Function, dispose: Function}}
 */
export function mountDogViewer(container, options = {}) {
  const mount = typeof container === "string" ? document.querySelector(container) : container;
  if (!(mount instanceof Element)) {
    throw new TypeError("mountDogViewer requires a valid container element");
  }

  const modelUrl = String(options.modelUrl || DEFAULT_MODEL_URL);
  const minUs = clamp(finiteNumber(options.minUs, 500), 100, 2499);
  const maxUs = clamp(finiteNumber(options.maxUs, 2500), minUs + 1, 4000);
  const defaultCenter = (minUs + maxUs) / 2;
  const centerUs = LEG_CHANNELS.map((_, index) => clamp(
    finiteNumber(options.centerUs?.[index], defaultCenter),
    minUs,
    maxUs,
  ));
  const maxJointRadians = clamp(finiteNumber(options.maxJointDegrees, 52), 5, 80) * Math.PI / 180;
  const orientationStaleMs = clamp(finiteNumber(options.orientationStaleMs, 1500), 0, 60000);
  const orientationResponse = clamp(finiteNumber(options.orientationResponse, 10), 0.5, 60);
  const reducedMotion = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches === true;
  const autoRotate = options.autoRotate !== false && !reducedMotion;
  const surface = createViewerSurface(mount);
  const state = {
    servosUs: [...centerUs],
    playbackState: "idle",
    armed: false,
    ledColor: "#35A7FF",
    ledEnabled: false,
    bodyOrientation: {
      rollDeg: 0,
      pitchDeg: 0,
      yawDeg: 0,
      status: "reset",
      receivedAt: 0,
      observed: false,
    },
  };
  surface.root.dataset.bodyOrientation = state.bodyOrientation.status;
  surface.root.dataset.bodyOrientationFresh = "false";

  let disposed = false;
  let fallbackShown = false;
  let frameRequest = 0;
  let renderer = null;
  let scene = null;
  let camera = null;
  let poseRoot = null;
  let bodyPoseRoot = null;
  let loadedModel = null;
  let mixer = null;
  let holographicFrame = null;
  let resizeObserver = null;
  let orientationStaleTimer = 0;
  let ledBeacon = null;
  let ledBeaconMaterial = null;
  let ledHalo = null;
  let ledHaloMaterial = null;
  let legBones = [];
  let legBaseQuaternions = [];
  let elapsed = 0;
  let cameraYaw = 0.705;
  let cameraYawTarget = cameraYaw;
  let cameraHeight = 1.84;
  let cameraHeightTarget = cameraHeight;
  let pointerId = null;
  let pointerX = 0;
  let pointerY = 0;
  let lastInteractionAt = performance.now();
  const clock = new THREE.Clock();
  const visualLegAngles = [0, 0, 0, 0];
  const targetLegAngles = [0, 0, 0, 0];
  const jointAxis = new THREE.Vector3(0, 0, 1);
  const jointOffset = new THREE.Quaternion();
  const bodyForwardAxis = new THREE.Vector3(1, 0, 0);
  const bodyUpAxis = new THREE.Vector3(0, 1, 0);
  const bodyLateralAxis = new THREE.Vector3(0, 0, 1);
  const bodyRollQuaternion = new THREE.Quaternion();
  const bodyPitchQuaternion = new THREE.Quaternion();
  const bodyYawQuaternion = new THREE.Quaternion();
  const targetBodyQuaternion = new THREE.Quaternion();
  const lookAt = new THREE.Vector3(0, -0.03, 0);
  const activeLedColor = new THREE.Color(state.ledColor);

  function emit(eventName, detail, callback) {
    surface.root.dispatchEvent(new CustomEvent(eventName, { bubbles: true, detail }));
    if (typeof callback === "function") {
      try {
        callback(detail);
      } catch (error) {
        console.error(`Dog viewer ${eventName} callback failed`, error);
      }
    }
  }

  function showFallback(reason, error) {
    if (disposed || fallbackShown) return;
    fallbackShown = true;
    console.error(`Dog viewer fallback: ${reason}`, error || "");
    surface.root.dataset.fallback = "true";
    delete surface.root.dataset.loading;
    surface.status.classList.add("error");
    surface.statusText.textContent = reason.replaceAll("-", " ").toUpperCase();
    surface.fallback.hidden = false;
    const detail = Object.freeze({ reason, error: error || null, modelUrl });
    emit(DOG_VIEWER_FALLBACK_EVENT, detail, options.onFallback);
  }

  function refreshLegTargets() {
    LEG_CHANNELS.forEach((leg, index) => {
      const halfRange = Math.max(1, state.servosUs[index] >= centerUs[index]
        ? maxUs - centerUs[index]
        : centerUs[index] - minUs);
      const normalized = clamp((state.servosUs[index] - centerUs[index]) / halfRange, -1, 1);
      targetLegAngles[index] = normalized * maxJointRadians * leg.direction;
      const value = surface.channels.querySelector(`[data-dog-leg="${index}"] small`);
      if (value) value.textContent = String(Math.round(state.servosUs[index]));
    });
  }

  function setBodyOrientationStatus(status) {
    state.bodyOrientation.status = status;
    surface.root.dataset.bodyOrientation = status;
    surface.root.dataset.bodyOrientationFresh = String(status === "live");
    if (status !== "live" && orientationStaleTimer) {
      window.clearTimeout(orientationStaleTimer);
      orientationStaleTimer = 0;
    }
  }

  function scheduleOrientationStale() {
    if (orientationStaleTimer) window.clearTimeout(orientationStaleTimer);
    orientationStaleTimer = 0;
    if (orientationStaleMs <= 0) return;
    orientationStaleTimer = window.setTimeout(() => {
      orientationStaleTimer = 0;
      if (!disposed && state.bodyOrientation.status === "live") {
        setBodyOrientationStatus("stale");
      }
    }, orientationStaleMs);
  }

  function stopDecorativeBodyMotion() {
    if (state.bodyOrientation.observed) return;
    state.bodyOrientation.observed = true;
    // Once an IMU channel is attached, the bundled idle clip and procedural
    // bob must not be mistaken for measured robot motion.
    mixer?.setTime(0);
  }

  function readOrientationAngle(orientation, names) {
    for (const name of names) {
      if (!Object.prototype.hasOwnProperty.call(orientation, name)) continue;
      return finiteOrientationNumber(orientation[name]);
    }
    return null;
  }

  function applyBodyOrientationTarget(rollDeg, pitchDeg, yawDeg) {
    state.bodyOrientation.rollDeg = wrapDegrees(rollDeg);
    state.bodyOrientation.pitchDeg = wrapDegrees(pitchDeg);
    state.bodyOrientation.yawDeg = wrapDegrees(yawDeg);

    // The model points along +X, with +Y up and +Z lateral. Its imported bind
    // pose uses the opposite visual signs for body Roll/Pitch; Yaw retains the
    // separately validated heading mapping. Sensor mounting has already been
    // handled before data reaches the viewer.
    bodyYawQuaternion.setFromAxisAngle(bodyUpAxis, THREE.MathUtils.degToRad(-state.bodyOrientation.yawDeg));
    bodyPitchQuaternion.setFromAxisAngle(bodyLateralAxis, THREE.MathUtils.degToRad(state.bodyOrientation.pitchDeg));
    bodyRollQuaternion.setFromAxisAngle(bodyForwardAxis, THREE.MathUtils.degToRad(-state.bodyOrientation.rollDeg));
    targetBodyQuaternion.copy(bodyYawQuaternion)
      .multiply(bodyPitchQuaternion)
      .multiply(bodyRollQuaternion)
      .normalize();
  }

  /**
   * Apply one IMU sample in degrees. Accepted axes are roll_deg/pitch_deg/yaw_deg,
   * rollDeg/pitchDeg/yawDeg, or roll/pitch/yaw. Returns true only for a usable
   * live sample; invalid input never throws.
   */
  function setActualOrientation(orientation) {
    if (disposed) return false;
    stopDecorativeBodyMotion();

    if (orientation === null || orientation?.reset === true) {
      resetActualOrientation();
      return false;
    }
    if (!orientation || typeof orientation !== "object" || orientation.valid === false) {
      targetBodyQuaternion.identity();
      setBodyOrientationStatus("invalid");
      return false;
    }

    const explicitlyStale = orientation.fresh === false || orientation.stale === true;
    const rollDeg = readOrientationAngle(orientation, ["roll_deg", "rollDeg", "roll"]);
    const pitchDeg = readOrientationAngle(orientation, ["pitch_deg", "pitchDeg", "pitch"]);
    const yawDeg = readOrientationAngle(orientation, ["yaw_deg", "yawDeg", "yaw"]);
    const hasCompletePose = rollDeg !== null && pitchDeg !== null && yawDeg !== null;

    if (explicitlyStale) {
      if (hasCompletePose) applyBodyOrientationTarget(rollDeg, pitchDeg, yawDeg);
      setBodyOrientationStatus("stale");
      return false;
    }
    if (!hasCompletePose) {
      targetBodyQuaternion.identity();
      setBodyOrientationStatus("invalid");
      return false;
    }

    applyBodyOrientationTarget(rollDeg, pitchDeg, yawDeg);
    state.bodyOrientation.receivedAt = performance.now();
    setBodyOrientationStatus("live");
    scheduleOrientationStale();
    return true;
  }

  /** Reset the measured body pose to neutral. The rendered pose eases to zero. */
  function resetActualOrientation() {
    if (disposed) return;
    stopDecorativeBodyMotion();
    state.bodyOrientation.rollDeg = 0;
    state.bodyOrientation.pitchDeg = 0;
    state.bodyOrientation.yawDeg = 0;
    state.bodyOrientation.receivedAt = 0;
    targetBodyQuaternion.identity();
    setBodyOrientationStatus("reset");
  }

  function update(next = {}) {
    if (!next || typeof next !== "object") return;
    if (Array.isArray(next.servos_us)) {
      state.servosUs = LEG_CHANNELS.map((_, index) => clamp(
        finiteNumber(next.servos_us[index], state.servosUs[index]),
        minUs,
        maxUs,
      ));
    }
    if (Object.prototype.hasOwnProperty.call(next, "playback_state")) {
      state.playbackState = normalizePlaybackState(next.playback_state);
    }
    if (Object.prototype.hasOwnProperty.call(next, "armed")) state.armed = Boolean(next.armed);
    if (Object.prototype.hasOwnProperty.call(next, "led_color")) {
      state.ledColor = normalizeHexColor(next.led_color, state.ledColor);
    }
    if (Object.prototype.hasOwnProperty.call(next, "led_enabled")) {
      state.ledEnabled = Boolean(next.led_enabled);
    }
    if (Object.prototype.hasOwnProperty.call(next, "body_orientation")) {
      setActualOrientation(next.body_orientation);
    } else if (Object.prototype.hasOwnProperty.call(next, "bodyOrientation")) {
      setActualOrientation(next.bodyOrientation);
    }
    surface.root.dataset.armed = String(state.armed);
    surface.root.dataset.playback = state.playbackState;
    surface.root.dataset.led = String(state.ledEnabled);
    activeLedColor.set(state.ledColor);
    refreshLegTargets();
  }

  function resize() {
    if (disposed || !renderer || !camera) return;
    const bounds = surface.root.getBoundingClientRect();
    const width = Math.max(1, Math.round(bounds.width || mount.clientWidth || 1));
    const height = Math.max(1, Math.round(bounds.height || mount.clientHeight || 320));
    const ratioLimit = width < 460 ? 1.4 : 1.8;
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, ratioLimit));
    renderer.setSize(width, height, false);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
  }

  function pointerDown(event) {
    if (disposed || pointerId !== null) return;
    pointerId = event.pointerId;
    pointerX = event.clientX;
    pointerY = event.clientY;
    lastInteractionAt = performance.now();
    surface.canvas.setPointerCapture?.(event.pointerId);
    surface.root.dataset.dragging = "true";
  }

  function pointerMove(event) {
    if (disposed || event.pointerId !== pointerId) return;
    const deltaX = event.clientX - pointerX;
    const deltaY = event.clientY - pointerY;
    pointerX = event.clientX;
    pointerY = event.clientY;
    cameraYawTarget += deltaX * 0.008;
    cameraHeightTarget = clamp(cameraHeightTarget + deltaY * 0.008, 0.45, 3.1);
    lastInteractionAt = performance.now();
  }

  function pointerUp(event) {
    if (event.pointerId !== pointerId) return;
    surface.canvas.releasePointerCapture?.(event.pointerId);
    pointerId = null;
    lastInteractionAt = performance.now();
    delete surface.root.dataset.dragging;
  }

  function dispose() {
    if (disposed) return;
    disposed = true;
    window.cancelAnimationFrame(frameRequest);
    if (orientationStaleTimer) window.clearTimeout(orientationStaleTimer);
    orientationStaleTimer = 0;
    resizeObserver?.disconnect();
    window.removeEventListener("resize", resize);
    surface.canvas.removeEventListener("pointerdown", pointerDown);
    surface.canvas.removeEventListener("pointermove", pointerMove);
    surface.canvas.removeEventListener("pointerup", pointerUp);
    surface.canvas.removeEventListener("pointercancel", pointerUp);
    disposeObjectTree(scene);
    renderer?.dispose();
    renderer?.forceContextLoss();
    surface.root.remove();
    renderer = null;
    scene = null;
    camera = null;
    poseRoot = null;
    bodyPoseRoot = null;
    loadedModel = null;
  }

  surface.canvas.addEventListener("pointerdown", pointerDown);
  surface.canvas.addEventListener("pointermove", pointerMove);
  surface.canvas.addEventListener("pointerup", pointerUp);
  surface.canvas.addEventListener("pointercancel", pointerUp);
  window.addEventListener("resize", resize, { passive: true });
  surface.canvas.addEventListener("webglcontextlost", (event) => {
    event.preventDefault();
    showFallback("webgl-context-lost");
  });
  update(options.initialState || {});

  try {
    renderer = new THREE.WebGLRenderer({
      canvas: surface.canvas,
      alpha: true,
      antialias: true,
      powerPreference: "high-performance",
    });
  } catch (error) {
    showFallback("webgl-unavailable", error);
    return Object.freeze({ update, setActualOrientation, resetActualOrientation, resize, dispose });
  }

  renderer.setClearColor(0x000000, 0);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1;

  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(29, 1, 0.01, 50);
  poseRoot = new THREE.Group();
  poseRoot.name = "MotionStudioDogPose";
  bodyPoseRoot = new THREE.Group();
  bodyPoseRoot.name = "MotionStudioActualBodyOrientation";
  poseRoot.add(bodyPoseRoot);
  scene.add(poseRoot);

  scene.add(new THREE.HemisphereLight(0xe7feff, 0x05070d, 1.4));
  const keyLight = new THREE.DirectionalLight(0xc9fffb, 2.15);
  keyLight.position.set(3.5, 4.4, 4.5);
  scene.add(keyLight);
  const rimLight = new THREE.DirectionalLight(0x756bff, 2.5);
  rimLight.position.set(-4, 1.8, -3.8);
  scene.add(rimLight);

  ledBeaconMaterial = new THREE.MeshBasicMaterial({
    color: activeLedColor,
    transparent: true,
    opacity: 0.12,
    blending: THREE.AdditiveBlending,
    depthTest: false,
    depthWrite: false,
    toneMapped: false,
  });
  ledBeacon = new THREE.Mesh(new THREE.SphereGeometry(0.038, 12, 8), ledBeaconMaterial);
  ledBeacon.renderOrder = 10;

  ledHaloMaterial = new THREE.MeshBasicMaterial({
    color: activeLedColor,
    transparent: true,
    opacity: 0.025,
    blending: THREE.AdditiveBlending,
    depthWrite: false,
    toneMapped: false,
  });
  ledHalo = new THREE.Mesh(new THREE.CircleGeometry(1.06, 48), ledHaloMaterial);
  ledHalo.rotation.x = -Math.PI / 2;
  ledHalo.position.y = -0.705;
  ledHalo.renderOrder = 0;
  poseRoot.add(ledHalo);

  new GLTFLoader().load(
    modelUrl,
    (gltf) => {
      if (disposed) {
        disposeObjectTree(gltf.scene);
        return;
      }
      loadedModel = gltf.scene;
      loadedModel.rotation.y = MODEL_HEADING_CORRECTION_RAD;
      loadedModel.updateMatrixWorld(true);
      const bounds = new THREE.Box3().setFromObject(loadedModel);
      const dimensions = bounds.getSize(new THREE.Vector3());
      const scale = 2.72 / Math.max(dimensions.x, dimensions.z, dimensions.y * 1.15);
      loadedModel.scale.setScalar(scale);
      loadedModel.updateMatrixWorld(true);
      const center = new THREE.Box3().setFromObject(loadedModel).getCenter(new THREE.Vector3());
      loadedModel.position.sub(center);
      bodyPoseRoot.add(loadedModel);
      holographicFrame = buildHolographicFrame(loadedModel, bodyPoseRoot, scene);

      const body = loadedModel.getObjectByName("BodyBone");
      if (body) {
        ledBeacon.position.set(0, 0.075, 0);
        body.add(ledBeacon);
      } else {
        ledBeacon.position.set(0, 0.12, 0);
        bodyPoseRoot.add(ledBeacon);
      }

      legBones = LEG_CHANNELS.map((leg) => loadedModel.getObjectByName(leg.bone));
      // The servo offset is always composed from this stable bind-pose
      // quaternion.  Repeated update() calls therefore cannot accumulate a
      // rotation when a GLB has no animation track for one of the four bones.
      legBaseQuaternions = legBones.map((bone) => bone?.quaternion.clone() || null);
      const idleClip = THREE.AnimationClip.findByName(gltf.animations, "0Idle")
        || THREE.AnimationClip.findByName(gltf.animations, "1Idle");
      if (idleClip) {
        mixer = new THREE.AnimationMixer(loadedModel);
        const action = mixer.clipAction(idleClip);
        action.setLoop(THREE.LoopRepeat, Infinity);
        action.play();
        if (state.bodyOrientation.observed) mixer.setTime(0);
      }

      delete surface.root.dataset.loading;
      surface.root.dataset.ready = "true";
      surface.status.classList.add("ready");
      const resolvedCount = legBones.filter(Boolean).length;
      surface.statusText.textContent = `3D READY · ${resolvedCount}/4 JOINTS`;
      emit(DOG_VIEWER_READY_EVENT, Object.freeze({
        modelUrl,
        resolvedLegs: LEG_CHANNELS.filter((_, index) => Boolean(legBones[index])).map((leg) => leg.code),
      }), options.onReady);
    },
    (progress) => {
      if (disposed || fallbackShown || !progress.total) return;
      const percent = clamp(Math.round(progress.loaded / progress.total * 100), 0, 99);
      surface.statusText.textContent = `LOADING 3D MODEL · ${percent}%`;
    },
    (error) => showFallback("model-load-failed", error),
  );

  function render() {
    if (disposed) return;
    const delta = Math.min(clock.getDelta(), 0.05);
    elapsed += delta;
    const playbackActive = isPlaybackActive(state.playbackState);
    const actualOrientationAttached = state.bodyOrientation.observed;
    if (mixer && !actualOrientationAttached) {
      mixer.timeScale = playbackActive ? 0.54 : state.armed ? 0.34 : 0.18;
      mixer.update(delta);
    }

    if (
      state.bodyOrientation.status === "live"
      && orientationStaleMs > 0
      && performance.now() - state.bodyOrientation.receivedAt > orientationStaleMs
    ) {
      setBodyOrientationStatus("stale");
    }
    if (bodyPoseRoot) {
      const orientationDamping = 1 - Math.exp(-delta * orientationResponse);
      bodyPoseRoot.quaternion.slerp(targetBodyQuaternion, orientationDamping);
    }

    const damping = 1 - Math.exp(-delta * (playbackActive ? 18 : 12));
    LEG_CHANNELS.forEach((_, index) => {
      visualLegAngles[index] += (targetLegAngles[index] - visualLegAngles[index]) * damping;
      const bone = legBones[index];
      const baseQuaternion = legBaseQuaternions[index];
      if (!bone || !baseQuaternion) return;
      jointOffset.setFromAxisAngle(jointAxis, visualLegAngles[index]);
      bone.quaternion.copy(baseQuaternion).multiply(jointOffset);
    });

    const bobAmplitude = reducedMotion || actualOrientationAttached
      ? 0
      : playbackActive ? 0.035 : state.armed ? 0.012 : 0.006;
    poseRoot.position.y = Math.sin(elapsed * (playbackActive ? 4.1 : 1.55)) * bobAmplitude;
    if (holographicFrame) holographicFrame.animate(elapsed);

    const ledIntensity = state.ledEnabled ? (state.armed ? 1 : 0.7) : 0.08;
    ledBeaconMaterial.color.lerp(activeLedColor, 0.22);
    ledHaloMaterial.color.lerp(activeLedColor, 0.16);
    ledBeaconMaterial.opacity += ((0.12 + ledIntensity * 0.76) - ledBeaconMaterial.opacity) * 0.12;
    ledHaloMaterial.opacity += ((0.018 + ledIntensity * 0.15) - ledHaloMaterial.opacity) * 0.1;
    const beaconPulse = 1 + (state.ledEnabled ? Math.sin(elapsed * 4.6) * 0.22 : 0);
    ledBeacon.scale.setScalar(beaconPulse);
    ledHalo.scale.setScalar(0.92 + ledIntensity * 0.2 + Math.sin(elapsed * 1.7) * 0.025);

    if (autoRotate && !actualOrientationAttached && pointerId === null && performance.now() - lastInteractionAt > 2600) {
      cameraYawTarget += delta * (playbackActive ? 0.052 : 0.018);
    }
    const cameraDamping = 1 - Math.exp(-delta * 8);
    cameraYaw += (cameraYawTarget - cameraYaw) * cameraDamping;
    cameraHeight += (cameraHeightTarget - cameraHeight) * cameraDamping;
    const radius = 5.03;
    camera.position.set(Math.sin(cameraYaw) * radius, cameraHeight, Math.cos(cameraYaw) * radius);
    camera.lookAt(lookAt);
    renderer.render(scene, camera);
    frameRequest = window.requestAnimationFrame(render);
  }

  if (typeof ResizeObserver === "function") {
    resizeObserver = new ResizeObserver(resize);
    resizeObserver.observe(surface.root);
  }
  resize();
  frameRequest = window.requestAnimationFrame(render);
  return Object.freeze({ update, setActualOrientation, resetActualOrientation, resize, dispose });
}
