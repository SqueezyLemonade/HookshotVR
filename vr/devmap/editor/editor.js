// The dev map editor (vr/devmap/editor): places the game's actors in a map of our own, VrTestStage.
// The layout it saves is ours (actor names, places, settings); the game's data comes from the local server, from your files.
import * as THREE from 'three';
import { TransformControls } from 'three/addons/controls/TransformControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';

const COLORS = { Prefabs: 0xff9f43, Blocks: 0x5fd0e0, Floors: 0x8fbf6a, Walls: 0xc9a46a, Doors: 0xd36a5a, Lighting: 0xf2d65c, Foliage: 0x4caf6a,
  Landscape: 0x9a8f80, NPCs: 0x6ab3e6, Enemies: 0xe65a7a, Items: 0xe6a03a, Logic: 0xb08ae6, Objects: 0x7ac7c2, Other: 0x9aa3b3 };
const $ = (id) => document.getElementById(id);
const api = async (path, opts) => {
  const r = await fetch(path, opts);
  const t = await r.text();
  let j; try { j = JSON.parse(t); } catch { j = { error: t }; }
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
};
const status = (msg) => { $('status').textContent = msg; };

// ---- state ------------------------------------------------------------------------------------------------------------
let catalog = [], byName = new Map(), category = 'Blocks', layoutName = 'default';
let layout = { format: 1, base: 'DarkLinkStage', objects: [] };
let base = null, selected = null, dirty = false, framedOnce = false;
const undoStack = [], redoStack = [];
const snapshot = () => JSON.stringify(layout);
function remember() { undoStack.push(snapshot()); if (undoStack.length > 200) undoStack.shift(); redoStack.length = 0; dirty = true; }

// ---- scene ------------------------------------------------------------------------------------------------------------
const canvas = $('canvas');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
renderer.setPixelRatio(window.devicePixelRatio);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x101216);
const camera = new THREE.PerspectiveCamera(60, 1, 0.05, 2000);
const sky = new THREE.HemisphereLight(0xdde6ff, 0x30281f, 1.1); scene.add(sky);
const sun = new THREE.DirectionalLight(0xffffff, 1.4); sun.position.set(30, 60, 20); scene.add(sun);
const grid = new THREE.GridHelper(100, 100, 0x3a4150, 0x262b35); scene.add(grid);
const gizmo = new TransformControls(camera, canvas);
gizmo.addEventListener('dragging-changed', (e) => {
  if (e.value) remember(); else { syncFromObject(selected); renderProps(); }
});
gizmo.addEventListener('objectChange', () => { syncFromObject(selected); renderPropsValues(); });
scene.add(gizmo);
const baseGroup = new THREE.Group(); scene.add(baseGroup);
const lightsGroup = new THREE.Group(); scene.add(lightsGroup);
const placedGroup = new THREE.Group(); scene.add(placedGroup);
const loader = new GLTFLoader();
const modelCache = new Map();   // (model name -> Promise of a THREE.Object3D, or null when there is none)

function resize() {
  const r = canvas.parentElement.getBoundingClientRect();
  renderer.setSize(r.width, r.height, false);
  camera.aspect = r.width / Math.max(1, r.height); camera.updateProjectionMatrix();
}
window.addEventListener('resize', resize);

// ---- the camera: a free fly camera (its speed in m/s, never tied to a target, so it gets as close as you like) ---------
// Hold the right button: look round, WASD fly, Q/E down/up, Shift faster, the wheel sets the speed. The wheel alone moves
// toward the cursor by a fixed step, left drag on empty space orbits round the point pressed, middle drag pans.
const fly = { yaw: 0, pitch: -0.5, speed: 8, keys: new Set(), drag: null, looked: false };
camera.rotation.order = 'YXZ';
function aim() { camera.rotation.set(fly.pitch, fly.yaw, 0); }
function lookAt(target) {
  const d = target.clone().sub(camera.position);
  fly.yaw = Math.atan2(-d.x, -d.z); fly.pitch = Math.atan2(d.y, Math.hypot(d.x, d.z)); aim();
}
function forward() { return new THREE.Vector3(0, 0, -1).applyQuaternion(camera.quaternion); }
function inFront(m) { return camera.position.clone().add(forward().multiplyScalar(m)); }
function showSpeed() { $('camSpeed').textContent = 'camera ' + (fly.speed < 1 ? fly.speed.toFixed(2) : fly.speed.toFixed(1)) + ' m/s'; }
function hitUnder(ev) {   // (the first thing under the cursor, as a point; else null)
  rayFrom(ev);
  const hits = ray.intersectObjects([...baseGroup.children, ...placedGroup.children], true)
    .filter((h) => h.object.isMesh && visibleChain(h.object));
  if (hits.length) return hits[0].point.clone();
  const p = new THREE.Vector3();
  return ray.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 1, 0), -grid.position.y), p) ? p : null;
}
canvas.addEventListener('pointerdown', (ev) => {
  canvas.focus({ preventScroll: true });   // (out of the search box or a menu: keys are for the view now)
  if (gizmo.dragging || (ev.button === 0 && gizmo.axis)) return;
  const kind = ev.button === 2 ? 'look' : ev.button === 1 ? 'pan' : ev.button === 0 ? 'orbit' : null;
  if (!kind) return;
  if (ev.button === 1) ev.preventDefault();
  let pivot = null;
  if (kind !== 'look') pivot = hitUnder(ev) || inFront(10);
  fly.drag = { kind, pivot, x: ev.clientX, y: ev.clientY, moved: 0, dist: pivot ? pivot.distanceTo(camera.position) : 0 };
  canvas.setPointerCapture(ev.pointerId);
  if (kind === 'look') {   // (the cursor locked and hidden while looking: browsers grant the lock only on a press, so here;
    // it comes back where it was on release. A plain right click (no move) still opens the menu)
    fly.looked = false;
    canvas.style.cursor = 'none';
    if (canvas.requestPointerLock && !document.pointerLockElement) {
      try {
        const p = canvas.requestPointerLock({ unadjustedMovement: true });
        if (p && p.catch) p.catch(() => { try { const q = canvas.requestPointerLock(); if (q && q.catch) q.catch(lockRefused); } catch (e) { lockRefused(); } });
      } catch (e) { lockRefused(); }
    }
  }
});
canvas.addEventListener('pointermove', (ev) => {
  const d = fly.drag;
  if (!d || gizmo.dragging) return;
  const dx = ev.movementX, dy = ev.movementY;
  d.moved += Math.abs(dx) + Math.abs(dy);
  if (d.moved < 4) return;
  if (d.kind === 'look') {
    fly.looked = true;
    fly.yaw -= dx * 0.004; fly.pitch = Math.max(-1.55, Math.min(1.55, fly.pitch - dy * 0.004)); aim();
  } else if (d.kind === 'orbit') {
    const off = camera.position.clone().sub(d.pivot);
    off.applyAxisAngle(new THREE.Vector3(0, 1, 0), -dx * 0.006);
    const right = new THREE.Vector3(1, 0, 0).applyQuaternion(camera.quaternion);
    const tilted = off.clone().applyAxisAngle(right, -dy * 0.006);
    if (Math.abs(tilted.clone().normalize().y) < 0.995) off.copy(tilted);
    camera.position.copy(d.pivot).add(off); lookAt(d.pivot);
  } else {   // (pan: the point pressed stays under the cursor)
    const m = 2 * Math.tan(THREE.MathUtils.degToRad(camera.fov / 2)) * Math.max(d.dist, 0.5) / canvas.clientHeight;
    const right = new THREE.Vector3(1, 0, 0).applyQuaternion(camera.quaternion);
    const up = new THREE.Vector3(0, 1, 0).applyQuaternion(camera.quaternion);
    camera.position.add(right.multiplyScalar(-dx * m)).add(up.multiplyScalar(dy * m));
  }
});
let lockWarned = false;
function lockRefused() {   // (some browsers and embedded views do not allow it: the cursor is still hidden while looking)
  if (!lockWarned) { lockWarned = true; status('This browser did not lock the mouse: looking round works, the cursor is hidden'); }
}
document.addEventListener('pointerlockerror', lockRefused);
function endDrag() {
  fly.drag = null;
  canvas.style.cursor = '';
  if (document.pointerLockElement === canvas) document.exitPointerLock();
}
document.addEventListener('pointerlockchange', () => {   // (Esc ends the lock: the look ends with it)
  if (!document.pointerLockElement && fly.drag && fly.drag.kind === 'look' && fly.looked) { fly.drag = null; canvas.style.cursor = ''; }
});
canvas.addEventListener('pointerup', (ev) => {   // (a right click that did not look round opens the menu: on release, as
  // Linux browsers send contextmenu on the press)
  const d = fly.drag;
  endDrag();
  if (ev.button === 2 && d && d.kind === 'look' && !fly.looked) openMenu(ev);
});
canvas.addEventListener('pointercancel', endDrag);
canvas.addEventListener('wheel', (ev) => {
  ev.preventDefault();
  if (fly.drag && fly.drag.kind === 'look') {   // (while looking: the speed)
    fly.speed = Math.max(0.1, Math.min(200, fly.speed * (ev.deltaY < 0 ? 1.25 : 0.8))); showSpeed(); fly.looked = true;
    return;
  }
  rayFrom(ev);   // (a fixed step toward the cursor, an eighth of a second at the speed: no slowing down up close)
  const step = fly.speed * 0.125 * (ev.shiftKey ? 4 : 1) * Math.min(3, Math.abs(ev.deltaY) / 100 || 1);
  camera.position.add(ray.ray.direction.clone().multiplyScalar(ev.deltaY < 0 ? step : -step));
}, { passive: false });
canvas.addEventListener('auxclick', (ev) => { if (ev.button === 1) ev.preventDefault(); });
const FLY_KEYS = { w: [0, 0, -1], s: [0, 0, 1], a: [-1, 0, 0], d: [1, 0, 0], q: [0, -1, 0], e: [0, 1, 0] };
const ARROW_KEYS = { arrowup: [0, 0, -1], arrowdown: [0, 0, 1], arrowleft: [-1, 0, 0], arrowright: [1, 0, 0],
  pageup: [0, 1, 0], pagedown: [0, -1, 0] };   // (these fly without holding the right button)
window.addEventListener('keyup', (ev) => fly.keys.delete(ev.key.toLowerCase()));
window.addEventListener('blur', () => { fly.keys.clear(); fly.drag = null; });
let lastFrame = performance.now();
(function loop() {
  requestAnimationFrame(loop);
  const now = performance.now(), dt = Math.min(0.1, (now - lastFrame) / 1000); lastFrame = now;
  const looking = fly.drag && fly.drag.kind === 'look';
  if (fly.keys.size) {
    const v = new THREE.Vector3();
    for (const k of fly.keys) {
      const dir = ARROW_KEYS[k] || (looking && FLY_KEYS[k]);
      if (dir) v.add(new THREE.Vector3(...dir));
    }
    if (v.lengthSq()) {
      if (looking) fly.looked = true;
      const up = v.y; v.y = 0;
      v.applyQuaternion(camera.quaternion);
      v.y += up;
      camera.position.add(v.normalize().multiplyScalar(fly.speed * dt * (fly.keys.has('shift') ? 3 : 1)));
    }
  }
  renderer.render(scene, camera);
})();

// ---- shapes -----------------------------------------------------------------------------------------------------------
function boxFor(aabb, color, opacity, wire) {
  const lo = aabb ? aabb[0] : [-0.5, 0, -0.5], hi = aabb ? aabb[1] : [0.5, 1, 0.5];
  const size = [0, 1, 2].map((k) => Math.max(0.05, hi[k] - lo[k]));
  const geo = new THREE.BoxGeometry(...size);
  geo.translate((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2);
  const g = new THREE.Group();
  const mesh = new THREE.Mesh(geo, new THREE.MeshStandardMaterial({ color, transparent: opacity < 1, opacity, roughness: 0.8 }));
  g.add(mesh);
  if (wire) g.add(new THREE.LineSegments(new THREE.EdgesGeometry(geo), new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.9 })));
  return g;
}
function placeShape(obj3d, entry, color, opacity = 1) {   // (the actor's own model when the server can give one: else its box stays)
  const model = entry && entry.model;
  if (!model) return;
  if (!modelCache.has(model)) {
    modelCache.set(model, new Promise((resolve) => {
      loader.load('/api/model/' + encodeURIComponent(model), (gltf) => resolve(gltf.scene), undefined, () => resolve(null));
    }));
  }
  modelCache.get(model).then((scn) => {
    if (!scn || !obj3d.parent) return;
    const copy = scn.clone(true);
    const plain = new THREE.MeshStandardMaterial({ color, roughness: 0.85, transparent: opacity < 1, opacity, depthWrite: opacity >= 1 });
    const textured = new Map();   // (the game's own colour texture where it has one: kept, made see-through like the rest)
    copy.traverse((m) => {
      if (!m.isMesh) return;
      const map = m.material && m.material.map;
      const kind = m.material && m.material.userData && m.material.userData.kind;
      if (kind) {   // (water, lava: see-through, tinted, the game's texture under it)
        m.material = new THREE.MeshStandardMaterial({ map: texturesOn() ? map : null, color: kind === 'lava' ? 0xff6a20 : 0x3a8fd8,
          emissive: kind === 'lava' ? 0x6a1a00 : 0x000000, transparent: true, opacity: Math.min(opacity, 0.55), depthWrite: false,
          side: THREE.DoubleSide, roughness: 0.2 });
        m.material.userData.fluid = kind;
      } else if (map && texturesOn()) {
        if (!textured.has(map)) textured.set(map, new THREE.MeshStandardMaterial({ map, roughness: 0.85, side: THREE.DoubleSide,
          transparent: opacity < 1, opacity, depthWrite: opacity >= 1 }));
        m.material = textured.get(map);
      } else m.material = plain;
      m.userData.pick = obj3d;
    });
    if (entry.bake) applyBake(copy, entry.bake);
    const box = obj3d.getObjectByName('box');
    if (box) box.visible = false;
    for (const c of obj3d.children) if (c.isLineSegments) c.visible = opacity >= 1 && obj3d.userData.id === selected;
    copy.name = 'model';
    obj3d.add(copy);
  });
}
// (the base's baked lighting: each material's place in its atlas (server.py bake()); the atlas's red, ambient occlusion,
// darkens corners, under ledges and between stones as the game's bake does)
const atlases = new Map();
function atlasTexture(name) {
  if (!atlases.has(name)) {
    const t = new THREE.TextureLoader().load('/api/atlas/' + encodeURIComponent(name));
    t.flipY = false; t.colorSpace = THREE.NoColorSpace; t.channel = 1;
    atlases.set(name, t);
  }
  return atlases.get(name);
}
function lightBalance() {   // (baked light on: mostly the soft light the bake shades, a weak sun; off: as before)
  const on = $('baked').checked;
  sky.intensity = on ? 2.2 : 1.1; sun.intensity = on ? 0.5 : 1.4;
}
function applyBake(model, mats) {
  if (!$('baked').checked) return;
  model.traverse((m) => {
    if (!m.isMesh || !m.geometry.attributes.uv1 || !m.material || m.material.userData.fluid) return;
    const mat = (m.geometry.userData && m.geometry.userData.mat) ?? (m.userData && m.userData.mat);
    const r = mats[mat];
    if (!r) return;
    const t = atlasTexture(r[4]).clone();   // (one picture, each its own place in it)
    t.repeat.set(r[0], r[1]); t.offset.set(r[2], r[3]); t.channel = 1; t.needsUpdate = true;
    m.material = m.material.clone();
    m.material.aoMap = t; m.material.aoMapIntensity = 1;
    m.material.lightMap = t; m.material.lightMapIntensity = 0;   // (kept for a light pass later)
    m.material.needsUpdate = true;
  });
}
const toRad = (d) => d * Math.PI / 180, toDeg = (r) => r * 180 / Math.PI;
function applyTransform(o3, p, r, s) {
  o3.position.set(p[0], p[1], p[2]);
  o3.rotation.set(toRad(r[0] || 0), toRad(r[1] || 0), toRad(r[2] || 0), 'ZYX');   // (the game's: sead builds Rz·Ry·Rx, as the model bones do)
  o3.scale.set(s[0] ?? 1, s[1] ?? 1, s[2] ?? 1);
}

// ---- base map ---------------------------------------------------------------------------------------------------------
const baseById = new Map();   // (a base actor's hash -> its group)
async function loadBase(name) {
  status('Loading base ' + name + '…');
  base = await api('/api/base/' + encodeURIComponent(name));
  const baked = await api('/api/bake/' + encodeURIComponent(name)).catch(() => ({}));
  baseGroup.clear(); baseById.clear();
  for (const a of base.actors) {
    if (a.left_out || !a.aabb) continue;
    const g = boxFor(a.aabb, COLORS[a.category] || 0x888888, 0.18, true);
    g.name = 'base';
    g.children[0].name = 'box';
    applyTransform(g, a.position, a.rotation, a.scale);
    g.userData.base = a;
    g.userData.id = 'base:' + a.hash;
    g.traverse((c) => (c.userData.pick = g));
    baseById.set(a.hash, g);
    baseGroup.add(g);
    placeShape(g, { model: a.model, bake: baked[a.hash] }, 0x8a8f99, baseOpacity());
  }
  grid.position.y = base.start[1];
  lightsGroup.clear();   // (the base's own lights: a glowing marker each, and a light the editor's view shows them by)
  const lights = await api('/api/lights/' + encodeURIComponent(name)).catch(() => []);
  for (const l of lights) {
    const color = l.kind === 'spot' ? 0xfff2c0 : l.kind === 'amplifier' ? 0xffc870 : 0xffe0a0;
    const bulb = new THREE.Mesh(new THREE.SphereGeometry(0.18, 12, 8), new THREE.MeshBasicMaterial({ color }));
    bulb.position.set(...l.position);
    if (lights.length <= 48) { const pl = new THREE.PointLight(color, 3, 10, 2); bulb.add(pl); }
    bulb.userData.light = l;
    lightsGroup.add(bulb);
  }
  status(`Base ${name}: ${base.actors.length} actors`);
}

// ---- placed objects ---------------------------------------------------------------------------------------------------
const objects3d = new Map();   // (layout object id -> THREE.Group)
let startMarker = null;
function newId() { let k = layout.objects.length + 1; while (layout.objects.some((o) => o.id === 'o' + k)) k++; return 'o' + k; }
const prefabCache = new Map();   // (prefab name/from -> Promise of its members)
function prefabMembers(o) {
  const key = o.prefab + '/' + (o.from || '') + '/' + (o.group || '');
  if (!prefabCache.has(key)) prefabCache.set(key, api('/api/prefab/' + encodeURIComponent(o.prefab) + '/' + encodeURIComponent(o.from || '') +
    '/' + encodeURIComponent(o.group || '')).catch(() => null));
  return prefabCache.get(key);
}
function makeObject3d(o) {
  if (o.prefab) {   // (one of the game's sets: its members where they stand, the first at the set's origin)
    const g = new THREE.Group();
    const marker = boxFor([[-0.3, 0, -0.3], [0.3, 0.6, 0.3]], COLORS.Prefabs, 0.9, true);
    marker.children[0].name = 'box';
    g.add(marker);
    g.userData.id = o.id;
    marker.children.forEach((c) => (c.userData.pick = g));
    applyTransform(g, o.position, o.rotation || [0, 0, 0], [1, 1, 1]);
    prefabMembers(o).then((m) => {
      if (!m || !g.parent) return;
      m.members.forEach((x, k) => {   // (each member its own: selectable, movable, swappable: o.members[k])
        const e = (o.members || {})[k] || {};
        const swapped = e.actor ? byName.get(e.actor) : null;
        const aabb = swapped ? swapped.aabb : x.aabb, model = swapped ? swapped.model : x.model, cat = swapped ? swapped.category : x.category;
        const sub = boxFor(aabb, COLORS[cat] || 0x999999, 0.85, true);
        sub.children[0].name = 'box';
        applyTransform(sub, e.position || x.position, e.rotation || x.rotation, [1, 1, 1]);
        sub.userData.id = o.id + '#' + k;
        sub.userData.member = { ...x, actor: e.actor || x.actor, original: x };
        sub.traverse((c) => (c.userData.pick = sub));
        g.add(sub);
        placeShape(sub, { model }, COLORS[cat] || 0x999999, 1);
        if (selected === sub.userData.id) gizmo.attach(sub);
      });
    });
    return g;
  }
  const e = byName.get(o.actor);
  const color = COLORS[e ? e.category : 'Other'] || 0x999999;
  const g = boxFor(e && e.aabb, color, e && e.invisible ? 0.25 : e && e.pre_actor ? 0.35 : 0.85, true);
  g.children[0].name = 'box';
  g.userData.id = o.id;
  for (const c of g.children) c.userData.pick = g;
  applyTransform(g, o.position, o.rotation || [0, 0, 0], o.scale || [1, 1, 1]);
  placeShape(g, e, color, e && e.pre_actor ? 0.6 : 1);
  return g;
}
function applyBaseEdits() {   // (the base's own actors as the layout changes them: base_edits, by hash)
  const edits = layout.base_edits || {};
  for (const [hash, g] of baseById) {
    const a = g.userData.base, e = edits[hash] || {};
    applyTransform(g, e.position || a.position, e.rotation || a.rotation, e.scale || a.scale);
    g.visible = !e.removed;
  }
}
function rebuildObjects() {
  applyBaseEdits();
  placedGroup.clear(); objects3d.clear();
  for (const o of layout.objects) { const g = makeObject3d(o); placedGroup.add(g); objects3d.set(o.id, g); }
  // the start position: a green arrow
  const s = layout.start || {};
  const pos = s.position || (base ? base.start : [0, 0, 0]);
  const yaw = s.yaw ?? (base ? base.start_rot[1] : 0);
  startMarker = new THREE.Group();
  const cone = new THREE.Mesh(new THREE.ConeGeometry(0.35, 0.9, 16), new THREE.MeshStandardMaterial({ color: 0x40d070 }));
  cone.rotation.x = Math.PI / 2; cone.position.set(0, 0.5, 0.45);
  const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.06, 0.06, 1.8), new THREE.MeshStandardMaterial({ color: 0x40d070 }));
  pole.position.y = 0.9;
  startMarker.add(cone, pole);
  startMarker.children.forEach((c) => (c.userData.pick = startMarker));
  startMarker.userData.id = '__start';
  startMarker.position.set(...pos); startMarker.rotation.set(0, toRad(yaw), 0);
  placedGroup.add(startMarker);
  if (selected) select(selected === '__start' || selected.startsWith('base:') || layout.objects.some((o) => o.id === selected.split('#')[0]) ? selected : null);
  renderOutliner();
}
function objById(id) { return layout.objects.find((o) => o.id === id); }
function syncFromObject(id) {
  if (!id) return;
  const g = findObject3d(id);
  if (!g) return;
  const r3 = (v) => Math.round(v * 1000) / 1000;
  if (id.includes('#')) {   // (a prefab's member: from the set's origin, in its axes)
    const [pid, k] = id.split('#');
    const o = objById(pid);
    o.members = o.members || {};
    o.members[k] = { ...(o.members[k] || {}), position: g.position.toArray().map(r3),
      rotation: [g.rotation.x, g.rotation.y, g.rotation.z].map((v) => r3(toDeg(v))) };
    dirty = true;
    return;
  }
  if (id.startsWith('base:')) {
    layout.base_edits = layout.base_edits || {};
    const e = layout.base_edits[id.slice(5)] = layout.base_edits[id.slice(5)] || {};
    e.position = g.position.toArray().map(r3);
    e.rotation = [g.rotation.x, g.rotation.y, g.rotation.z].map((v) => r3(toDeg(v)));
    e.scale = g.scale.toArray().map(r3);
    dirty = true;
    return;
  }
  if (id === '__start') {
    layout.start = { position: g.position.toArray().map(r3), yaw: r3(toDeg(g.rotation.y)) };
  } else {
    const o = objById(id);
    o.position = g.position.toArray().map(r3);
    o.rotation = [g.rotation.x, g.rotation.y, g.rotation.z].map((v) => r3(toDeg(v)));
    o.scale = g.scale.toArray().map(r3);
  }
  dirty = true;
}
function findObject3d(id) {
  if (!id) return null;
  if (id === '__start') return startMarker;
  if (id.startsWith('base:')) return baseById.get(id.slice(5));
  if (id.includes('#')) { const pg = objects3d.get(id.split('#')[0]); return pg ? pg.children.find((c) => c.userData.id === id) : null; }
  return objects3d.get(id);
}
function select(id) {
  selected = id;
  const g = findObject3d(id);
  if (g) gizmo.attach(g); else gizmo.detach();
  if (id === '__start') gizmo.setMode(gizmo.mode === 'scale' ? 'translate' : gizmo.mode);
  for (const [oid, grp] of objects3d) grp.children.forEach((c) => {
    if (!c.isLineSegments) return;
    const hasModel = !!grp.getObjectByName('model');
    c.visible = !hasModel || oid === id;
    const ob = objById(oid);
    c.material.color.set(oid === id ? 0xffffff : (ob && ob.prefab ? COLORS.Prefabs : (COLORS[(byName.get(ob && ob.actor) || {}).category] || 0x999999)));
  });
  renderOutliner(); renderProps();
}
function addObject(actorName, position, extra = {}) {
  remember();
  const o = { id: newId(), actor: actorName, position: position.map((v) => Math.round(v * 1000) / 1000), rotation: [0, 0, 0], scale: [1, 1, 1], ...extra };
  layout.objects.push(o);
  const g = makeObject3d(o); placedGroup.add(g); objects3d.set(o.id, g);
  select(o.id);
  status('Placed ' + actorName);
}
function placeFromPalette(name, at) {   // (an actor, or a prefab: palette names 'prefab:<module>')
  if (name.startsWith('prefab:')) {
    const pf = byName.get(name);
    remember();
    const o = { id: newId(), prefab: pf.module, from: pf.from[0], position: at.map((v) => Math.round(v * 1000) / 1000), rotation: [0, 0, 0] };
    if (pf.group) o.group = pf.group, o.label = pf.label;
    layout.objects.push(o);
    const g = makeObject3d(o); placedGroup.add(g); objects3d.set(o.id, g);
    select(o.id); status('Placed the prefab ' + o.prefab);
    return;
  }
  addObject(name, at);
}
function deleteSelected() {
  if (!selected || selected === '__start') return;
  if (selected.includes('#')) selected = selected.split('#')[0];   // (a member: the whole set goes)
  if (selected.startsWith('base:')) {   // (a base piece: left out of our map, kept in the layout as removed)
    remember();
    layout.base_edits = layout.base_edits || {};
    layout.base_edits[selected.slice(5)] = { removed: true };
    selected = null; gizmo.detach(); rebuildObjects(); renderProps();
    return;
  }
  remember();
  layout.objects = layout.objects.filter((o) => o.id !== selected);
  selected = null; gizmo.detach(); rebuildObjects(); renderProps();
}
function duplicateSelected() {
  const o = objById(selected);
  if (!o) return;
  remember();
  const c = JSON.parse(JSON.stringify(o));
  c.id = newId();
  c.position = [o.position[0] + 1, o.position[1], o.position[2]];
  layout.objects.push(c);
  rebuildObjects(); select(c.id);
}
function undo() { if (!undoStack.length) return; redoStack.push(snapshot()); layout = JSON.parse(undoStack.pop()); rebuildObjects(); renderProps(); dirty = true; }
function redo() { if (!redoStack.length) return; undoStack.push(snapshot()); layout = JSON.parse(redoStack.pop()); rebuildObjects(); renderProps(); dirty = true; }

// ---- marks (places marked in the game: Menu + X) ---------------------------------------------------------------------
const marksGroup = new THREE.Group(); scene.add(marksGroup);
async function loadMarks() {
  const marks = (await api('/api/marks').catch(() => [])).filter((m) => m.map === 'VrTestStage');
  marksGroup.clear();
  for (const m of marks) {
    const pin = new THREE.Mesh(new THREE.CylinderGeometry(0.12, 0.02, 1.2, 10), new THREE.MeshStandardMaterial({ color: 0xffd23f, emissive: 0x332600 }));
    pin.position.set(m.position[0], m.position[1] + 0.6, m.position[2]);
    pin.userData.mark = m;
    marksGroup.add(pin);
  }
  $('marks').textContent = marks.length ? `${marks.length} marked in the game: drop an actor on a pin to place it there, facing as you did.` :
    'None yet. In the VR test map, hold Menu and press X where you want something, then Load.';
}
$('refreshMarks').onclick = loadMarks;
$('clearMarks').onclick = async () => { await api('/api/marks/clear', { method: 'POST' }); loadMarks(); };

// ---- picking and dropping ---------------------------------------------------------------------------------------------
const ray = new THREE.Raycaster(), mouse = new THREE.Vector2();
function rayFrom(ev) {
  const r = canvas.getBoundingClientRect();
  mouse.set(((ev.clientX - r.left) / r.width) * 2 - 1, -((ev.clientY - r.top) / r.height) * 2 + 1);
  ray.setFromCamera(mouse, camera);
}
function visibleChain(o) { for (; o; o = o.parent) if (!o.visible) return false; return true; }
function pickPlaced(ev) {   // (what was clicked: something placed first, else a piece of the base map)
  rayFrom(ev);
  for (const group of [placedGroup, baseGroup]) {
    if (!group.visible) continue;
    const hits = ray.intersectObjects(group.children, true).filter((h) => h.object.isMesh && visibleChain(h.object));
    for (const h of hits) { const g = h.object.userData.pick; if (g && g.userData.id) return g.userData.id; }
  }
  return null;
}
function surfacePoint(ev) {   // (where a dropped actor goes: on what the ray meets first, else on the start's floor level)
  rayFrom(ev);
  const targets = [...baseGroup.children, ...placedGroup.children];
  const hits = ray.intersectObjects(targets, true).filter((h) => h.object.isMesh && h.object.visible && h.face);
  if (hits.length) {
    const h = hits[0];
    const n = h.face.normal.clone().transformDirection(h.object.matrixWorld);
    const p = h.point.clone();
    if (n.y > 0.5) return p.toArray();
    return p.add(n.multiplyScalar(0.05)).toArray();
  }
  const plane = new THREE.Plane(new THREE.Vector3(0, 1, 0), -grid.position.y);
  const p = new THREE.Vector3();
  return ray.ray.intersectPlane(plane, p) && p.distanceTo(camera.position) < 40 ? p.toArray() : inFront(6).toArray();
}
let downAt = null;
canvas.addEventListener('pointerdown', (ev) => { downAt = [ev.clientX, ev.clientY]; });
canvas.addEventListener('pointerup', (ev) => {
  if (!downAt || gizmo.dragging) return;
  const moved = Math.hypot(ev.clientX - downAt[0], ev.clientY - downAt[1]);
  downAt = null;
  if (moved > 4 || ev.button !== 0) return;
  select(pickPlaced(ev));
});
const vp = $('viewport');
vp.addEventListener('dragover', (ev) => { ev.preventDefault(); vp.classList.add('drop'); });
vp.addEventListener('dragleave', () => vp.classList.remove('drop'));
vp.addEventListener('drop', (ev) => {
  ev.preventDefault(); vp.classList.remove('drop');
  const name = ev.dataTransfer.getData('text/actor');
  if (!name) return;
  rayFrom(ev);
  const pin = ray.intersectObjects(marksGroup.children, false)[0];
  if (pin) {   // (on a mark: exactly there, turned as the view was)
    const m = pin.object.userData.mark;
    placeFromPalette(name, m.position.slice());
    const o = objById(selected);
    if (o) { o.rotation = [0, Math.round(m.yaw || 0), 0]; rebuildObjects(); select(o.id); }
    return;
  }
  placeFromPalette(name, snapValue(surfacePoint(ev)));
});
function snapValue(p) { return $('snap').checked ? p.map((v, k) => (k === 1 ? Math.round(v * 100) / 100 : Math.round(v * 2) / 2)) : p; }
function applySnap() {
  const on = $('snap').checked;
  gizmo.setTranslationSnap(on ? 0.5 : null);
  gizmo.setRotationSnap(on ? toRad(15) : null);
  gizmo.setScaleSnap(on ? 0.25 : null);
}

// ---- thumbnails: each palette row's actor drawn small, from its model (else its box), only when the row is seen ---------
const thumbRenderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, preserveDrawingBuffer: true });
thumbRenderer.setSize(96, 96);
const thumbScene = new THREE.Scene();
thumbScene.add(new THREE.HemisphereLight(0xffffff, 0x404050, 1.6));
const thumbSun = new THREE.DirectionalLight(0xffffff, 1.6); thumbSun.position.set(2, 3, 4); thumbScene.add(thumbSun);
const thumbCamera = new THREE.PerspectiveCamera(30, 1, 0.01, 1000);
const thumbs = new Map();   // (actor name -> data URL)
const thumbQueue = [];
let thumbBusy = false;
function thumbFor(a, img) {
  if (thumbs.has(a.name)) { img.src = thumbs.get(a.name); return; }
  thumbQueue.push([a, img]);
  pumpThumbs();
}
async function pumpThumbs() {
  if (thumbBusy) return;
  thumbBusy = true;
  while (thumbQueue.length) {
    const [a, img] = thumbQueue.shift();
    if (!img.isConnected) continue;
    if (thumbs.has(a.name)) { img.src = thumbs.get(a.name); continue; }
    let obj = null;
    if (a.model && !a.prefab) {
      if (!modelCache.has(a.model)) modelCache.set(a.model, new Promise((resolve) =>
        loader.load('/api/model/' + encodeURIComponent(a.model), (gltf) => resolve(gltf.scene), undefined, () => resolve(null))));
      const scn = await modelCache.get(a.model);
      if (scn) {
        obj = scn.clone(true);
        if (!texturesOn()) obj.traverse((m) => { if (m.isMesh) m.material = new THREE.MeshStandardMaterial({ color: COLORS[a.category] || 0x999999 }); });
      }
    }
    if (!obj) obj = boxFor(a.aabb, COLORS[a.category] || 0x999999, 1, true);
    thumbScene.add(obj);
    const box = new THREE.Box3().setFromObject(obj);
    const size = box.getSize(new THREE.Vector3()), mid = box.getCenter(new THREE.Vector3());
    const r = Math.max(size.length() / 2, 0.05);
    thumbCamera.position.copy(mid).add(new THREE.Vector3(1, 0.7, 1.3).normalize().multiplyScalar(r / Math.sin(THREE.MathUtils.degToRad(15))));
    thumbCamera.near = r / 20; thumbCamera.far = r * 20; thumbCamera.updateProjectionMatrix();
    thumbCamera.lookAt(mid);
    thumbRenderer.render(thumbScene, thumbCamera);
    thumbScene.remove(obj);
    const url = thumbRenderer.domElement.toDataURL('image/png');
    thumbs.set(a.name, url);
    img.src = url;
    await new Promise((r) => setTimeout(r, 0));
  }
  thumbBusy = false;
}
const thumbWatch = new IntersectionObserver((entries) => {
  for (const e of entries) if (e.isIntersecting) { thumbWatch.unobserve(e.target); thumbFor(byName.get(e.target.dataset.actor), e.target); }
});

// ---- palette ----------------------------------------------------------------------------------------------------------
function renderCategories() {
  const counts = {};
  for (const a of catalog) counts[a.category] = (counts[a.category] || 0) + 1;
  const order = ['Blocks', 'Prefabs', 'Floors', 'Walls', 'Objects', 'Doors', 'Items', 'NPCs', 'Enemies', 'Lighting', 'Foliage', 'Landscape', 'Logic', 'Other'];
  $('categories').innerHTML = '';
  for (const c of order) {
    const b = document.createElement('button');
    b.textContent = `${c} ${counts[c] || 0}`;
    b.className = c === category ? 'on' : '';
    b.onclick = () => { category = c; renderCategories(); renderPalette(); };
    $('categories').appendChild(b);
  }
  $('legend').innerHTML = order.map((c) => `<span style="--c:#${(COLORS[c] || 0).toString(16).padStart(6, '0')}">${c}</span>`).join('');
}
function renderPalette() {
  const q = $('search').value.trim().toLowerCase();
  const list = catalog.filter((a) => (q ? a.name.toLowerCase().includes(q) : a.category === category) && a.pack).slice(0, 600);
  const el = $('paletteList');
  el.innerHTML = '';
  if (category === 'Blocks' && !q) el.insertAdjacentHTML('beforeend', '<p class="hint" style="padding:6px">Solid blocks: floors, walls and steps Link collides with. Scale them to any size (2.6 × 2.5 × 2.6 m at scale 1, its bottom at its origin). Land pieces only show, so stand them on or in blocks; VrBlockInvisible is solid but not drawn: put it in a land piece to make the piece solid.</p>');
  for (const a of list) {
    const d = document.createElement('div');
    d.className = 'item' + (a.pre_actor ? ' pre' : '');
    d.draggable = true;
    d.title = `${a.name}\nPlaced ${a.count}× in the game (${a.maps.join(', ')})` +
      (a.pre_actor ? '\n⚠ A land piece: it shows in the game, but does not collide: put a block (Blocks) under or in it' : '') + (a.own ? (a.invisible ? '\nOne of ours: solid but not drawn in the game (an invisible wall or floor), sized by its scale' : '\nOne of ours: solid, sized by its scale') : '');
    d.innerHTML = `<img class="t" alt=""><span class="n"></span><span class="c">${a.count}</span>`;
    d.querySelector('.n').textContent = a.label || a.name;
    const img = d.querySelector('.t');
    img.dataset.actor = a.name;
    if (!a.prefab) thumbWatch.observe(img); else img.style.display = 'none';
    d.addEventListener('dragstart', (ev) => { ev.dataTransfer.setData('text/actor', a.name); ev.dataTransfer.effectAllowed = 'copy'; });
    d.addEventListener('dblclick', () => placeFromPalette(a.name, snapValue(placeInFront())));
    el.appendChild(d);
  }
  if (!list.length) el.innerHTML = '<p class="hint" style="padding:6px">No actors match.</p>';
}

// ---- outliner and properties ------------------------------------------------------------------------------------------
function renderOutliner() {
  const el = $('outliner');
  el.innerHTML = '';
  const baseRows = Object.entries(layout.base_edits || {}).map(([h, e]) => {
    const g = baseById.get(h);
    return { id: 'base:' + h, label: (g ? g.userData.base.actor : h) + (e.removed ? ' (removed)' : ' (moved)'), kind: 'base' };
  });
  const rows = [{ id: '__start', label: 'Start position', kind: 'start' }, ...layout.objects.map((o) => ({ id: o.id, label: o.name || o.actor || ('Prefab: ' + (o.label || o.prefab)), kind: o.id })), ...baseRows];
  for (const r of rows) {
    const d = document.createElement('div');
    d.className = 'row' + (r.id === selected ? ' sel' : '');
    d.innerHTML = '<span class="l"></span><span class="k"></span>';
    d.querySelector('.l').textContent = r.label;
    d.querySelector('.k').textContent = r.kind;
    d.onclick = () => select(r.id);
    d.ondblclick = () => frameSelected();
    el.appendChild(d);
  }
  $('count').textContent = `(${layout.objects.length})`;
}
const actorDetails = new Map();
async function details(name) {
  if (!actorDetails.has(name)) actorDetails.set(name, api('/api/actor/' + encodeURIComponent(name)).catch(() => null));
  return actorDetails.get(name);
}
function v3(label, key, values, step) {
  return `<label>${label}</label><div class="v3">${values.map((v, k) =>
    `<input type="number" step="${step}" data-key="${key}" data-k="${k}" value="${v}">`).join('')}</div>`;
}
function renderProps() {
  const el = $('props');
  if (!selected) { el.innerHTML = '<p class="hint">Select something to see its properties.</p>'; return; }
  if (selected === '__start') {
    const s = layout.start || { position: base ? base.start : [0, 0, 0], yaw: base ? base.start_rot[1] : 0 };
    el.innerHTML = `<p>Where Link arrives (the teleport's start position).</p>` + v3('Position (m)', 'position', s.position, 0.1) +
      `<label>Facing (degrees)</label><input type="number" step="15" data-key="yaw" value="${s.yaw ?? 0}">`;
    bindInputs(); return;
  }
  if (selected.startsWith('base:')) {
    const hash = selected.slice(5), g = baseById.get(hash);
    const a = g ? g.userData.base : { actor: hash };
    const e = (layout.base_edits || {})[hash] || {};
    const p = e.position || a.position, r = e.rotation || a.rotation, sc = e.scale || a.scale;
    el.innerHTML = `<label>Base map piece (the base's own; edits go in the layout)</label><input type="text" readonly id="pActor">` +
      (a.baked ? `<p class="warn">⚠ Baked into the base: it draws where you put it, but its collision is baked where it was. Moving or removing it may leave an invisible wall or floor behind.</p>` : '') +
      v3('Position (m)', 'position', p, 0.1) + v3('Rotation (degrees)', 'rotation', r, 15) + v3('Scale', 'scale', sc, 0.25) +
      `<div class="buttons"><button id="pDel">Remove</button><button id="pRestore">Restore</button></div>`;
    $('pActor').value = a.actor + (a.name ? ' (' + a.name + ')' : '');
    $('pDel').onclick = deleteSelected;
    $('pRestore').onclick = () => { remember(); delete layout.base_edits[hash]; rebuildObjects(); select(selected); };
    for (const inp of el.querySelectorAll('input[data-key]')) inp.onchange = () => {
      remember();
      layout.base_edits = layout.base_edits || {};
      const ed = layout.base_edits[hash] = layout.base_edits[hash] || {};
      ed.position = ed.position || [...p]; ed.rotation = ed.rotation || [...r]; ed.scale = ed.scale || [...sc];
      ed[inp.dataset.key][+inp.dataset.k] = parseFloat(inp.value) || 0;
      rebuildObjects();
    };
    return;
  }
  if (selected.includes('#')) {   // (a prefab member)
    const [pid, k] = selected.split('#');
    const o = objById(pid), sub = findObject3d(selected);
    if (!o || !sub) { el.innerHTML = ''; return; }
    const mem = sub.userData.member, e = (o.members || {})[k] || {};
    const alts = swapChoices(mem.original.actor);
    el.innerHTML = `<label>Part of the set "${o.label || o.prefab}"</label><select id="pSwap">${alts.map((n) => `<option ${n === mem.actor ? 'selected' : ''}>${n}</option>`).join('')}</select>` +
      `<p class="hint">Move it on its own: the set's logic and links come along (a switch still opens what it opened).</p>` +
      v3('Position from the set (m)', 'position', sub.position.toArray().map((v) => Math.round(v * 1000) / 1000), 0.1) +
      v3('Rotation (degrees)', 'rotation', [sub.rotation.x, sub.rotation.y, sub.rotation.z].map((v) => Math.round(toDeg(v) * 10) / 10), 15) +
      `<div class="buttons"><button id="pReset">Put back</button><button id="pWhole">Select the whole set</button></div>`;
    $('pSwap').onchange = () => { remember(); o.members = o.members || {}; o.members[k] = { ...(o.members[k] || {}), actor: $('pSwap').value === mem.original.actor ? undefined : $('pSwap').value }; rebuildObjects(); };
    $('pReset').onclick = () => { remember(); if (o.members) delete o.members[k]; rebuildObjects(); };
    $('pWhole').onclick = () => select(pid);
    for (const inp of el.querySelectorAll('input[data-key]')) inp.onchange = () => {
      remember(); o.members = o.members || {};
      const cur = o.members[k] = { ...(o.members[k] || {}) };
      cur.position = cur.position || sub.position.toArray(); cur.rotation = cur.rotation || [0, 0, 0].map((_, i) => toDeg([sub.rotation.x, sub.rotation.y, sub.rotation.z][i]));
      cur[inp.dataset.key][+inp.dataset.k] = parseFloat(inp.value) || 0;
      rebuildObjects();
    };
    return;
  }
  const o = objById(selected);
  if (o.prefab) {
    const pf = byName.get('prefab:' + o.prefab) || { from: [o.from] };
    el.innerHTML = `<label>Prefab (one of the game's own sets: its logic and actors)</label><input type="text" readonly id="pActor">` +
      `<label>Copied from (the game map whose set it is)</label><select id="pFrom">${pf.from.map((f) => `<option ${f === o.from ? 'selected' : ''}>${f}</option>`).join('')}</select>` +
      v3('Position (m)', 'position', o.position, 0.1) + v3('Rotation (degrees)', 'rotation', o.rotation || [0, 0, 0], 15) +
      `<div class="buttons"><button id="pDup">Duplicate</button><button id="pDel">Delete</button></div>`;
    $('pActor').value = o.prefab;
    $('pFrom').onchange = () => { remember(); o.from = $('pFrom').value; rebuildObjects(); };
    $('pDup').onclick = duplicateSelected; $('pDel').onclick = deleteSelected;
    bindInputs(); return;
  }
  const e = byName.get(o.actor) || {};
  el.innerHTML = `<label>Actor</label><input type="text" value="" id="pActor" readonly>` +
    (e.pre_actor ? `<p class="warn">⚠ A land piece: it shows in the game, but does not collide. For something to stand on or bump into, put a block (Blocks) under or in it.</p>` : '') +
    `<label>Name (optional: what links and events find it by)</label><input type="text" id="pName" value="">` +
    v3('Position (m)', 'position', o.position, 0.1) + v3('Rotation (degrees)', 'rotation', o.rotation || [0, 0, 0], 15) +
    v3('Scale', 'scale', o.scale || [1, 1, 1], 0.25) +
    (e.own ? v3('Size (m)', 'size', (o.scale || [1, 1, 1]).map((v, k) => Math.round(v * e.size[k] * 1000) / 1000), 0.5) : '') +
    `<div id="pChoices"></div>` +
    `<label>Settings (the actor's Dynamic fields you change, JSON)</label><textarea id="pParams" spellcheck="false"></textarea>` +
    `<div class="buttons"><button id="pDefaults">Copy the game's settings in</button><button id="pDup">Duplicate</button><button id="pDel">Delete</button></div>` +
    `<label>The game's settings for it (from ${e.maps ? e.maps[0] : 'its first placement'})</label><pre id="pExample">…</pre>`;
  $('pActor').value = o.actor;
  $('pName').value = o.name || '';
  $('pParams').value = JSON.stringify(o.params || {}, null, 1);
  $('pName').onchange = () => { remember(); o.name = $('pName').value.trim() || undefined; renderOutliner(); };
  $('pParams').onchange = () => {
    try { const v = JSON.parse($('pParams').value || '{}'); remember(); o.params = v; $('pParams').classList.remove('bad'); }
    catch { $('pParams').classList.add('bad'); }
  };
  $('pDup').onclick = duplicateSelected;
  $('pDel').onclick = deleteSelected;
  details(o.actor).then((d) => {
    const ex = d && d.example ? d.example : null;
    const box = $('pChoices');   // (settings whose values the game's maps show: a menu each, Drop__DropActor first)
    if (box && d && d.choices) {
      const keys = Object.keys(d.choices).sort((a, b) => (b === 'Drop__DropActor') - (a === 'Drop__DropActor'));
      for (const key of keys) {
        const label = key === 'Drop__DropActor' ? 'Contents' : key;
        const cur = (o.params || {})[key] ?? (ex && ex.Dynamic ? ex.Dynamic[key] : '') ?? '';
        const sel = document.createElement('select');
        sel.innerHTML = [...new Set([cur, ...d.choices[key]])].filter((v) => v !== undefined)
          .map((v) => `<option ${v === cur ? 'selected' : ''}>${v}</option>`).join('');
        sel.onchange = () => { remember(); o.params = { ...(o.params || {}), [key]: sel.value }; $('pParams').value = JSON.stringify(o.params, null, 1); };
        const l = document.createElement('label'); l.textContent = label;
        box.append(l, sel);
      }
    }
    if ($('pExample')) $('pExample').textContent = ex ? JSON.stringify(ex.Dynamic || {}, null, 1) + (ex.Name ? `\nName: ${ex.Name}` : '') : '(none)';
    $('pDefaults').onclick = () => {
      if (!ex || !ex.Dynamic) return;
      remember(); o.params = { ...ex.Dynamic, ...(o.params || {}) };
      $('pParams').value = JSON.stringify(o.params, null, 1);
    };
  });
  bindInputs();
}
function renderPropsValues() {   // (while the gizmo drags: only the numbers)
  const el = $('props');
  const src = selected === '__start' ? (layout.start || {}) : objById(selected);
  if (!src) return;
  for (const inp of el.querySelectorAll('input[data-key]')) {
    const key = inp.dataset.key;
    const v = key === 'yaw' ? src.yaw : (src[key] || [])[+inp.dataset.k];
    if (v !== undefined && document.activeElement !== inp) inp.value = v;
  }
}
function bindInputs() {
  for (const inp of $('props').querySelectorAll('input[data-key]')) {
    inp.onchange = () => {
      remember();
      const v = parseFloat(inp.value) || 0;
      if (selected === '__start') {
        layout.start = layout.start || { position: [...startMarker.position.toArray()], yaw: toDeg(startMarker.rotation.y) };
        if (inp.dataset.key === 'yaw') layout.start.yaw = v; else layout.start.position[+inp.dataset.k] = v;
      } else {
        const o = objById(selected);
        if (inp.dataset.key === 'size') {   // (a block's size in metres: its scale times the block's own size)
          const e = byName.get(o.actor);
          o.scale = o.scale || [1, 1, 1];
          o.scale[+inp.dataset.k] = Math.max(0.01, v / e.size[+inp.dataset.k]);
          rebuildObjects(); renderProps(); return;
        }
        o[inp.dataset.key] = o[inp.dataset.key] || (inp.dataset.key === 'scale' ? [1, 1, 1] : [0, 0, 0]);
        o[inp.dataset.key][+inp.dataset.k] = v;
      }
      rebuildObjects();
    };
  }
}
function frameSelected() {
  const g = selected === '__start' ? startMarker : objects3d.get(selected);
  const target = g ? new THREE.Box3().setFromObject(g).getCenter(new THREE.Vector3()) : new THREE.Vector3(...(base ? base.start : [0, 0, 0]));
  if (g && !isFinite(target.x)) target.copy(g.position);
  let off = forward().negate();
  if (!framedOnce) off = new THREE.Vector3(7, 9, 7);   // (first: above, looking down at an angle)
  framedOnce = true;
  off.setLength(g ? 6 : 14);
  camera.position.copy(target.clone().add(off)); lookAt(target);
}
function placeInFront() {   // (where a double-clicked actor goes: what the middle of the view meets, near enough; else 6 m ahead)
  const r = canvas.getBoundingClientRect();
  return surfacePoint({ clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 });
}

// (what a set's member may be swapped for: the game's actors of its own kind)
function swapChoices(actorName) {
  const kinds = [/SwitchShutter/, /Door_/, /TBox/, /StepSwitch/, /EyeSwitch/, /SwitchHit/, /HammerSwitch/, /SolarSwitch/];
  const kind = kinds.find((r) => r.test(actorName));
  if (!kind) return [actorName];
  return [actorName, ...catalog.filter((a) => !a.prefab && !a.own && a.pack && kind.test(a.name) && a.name !== actorName).map((a) => a.name)];
}

// ---- right click: quick actions on what is under the pointer ------------------------------------------------------
const menu = document.createElement('div');
menu.id = 'ctx';
document.body.appendChild(menu);
function closeMenu() { menu.style.display = 'none'; }
window.addEventListener('pointerdown', (ev) => { if (!menu.contains(ev.target)) closeMenu(); });
canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());   // (opened on release instead: the camera's pointerup)
async function openMenu(ev) {
  const id = pickPlaced(ev);
  if (!id) return closeMenu();
  select(id);
  const items = [];
  if (id.includes('#')) {
    const sub = findObject3d(id), [pid, k] = id.split('#'), o = objById(pid);
    for (const n of swapChoices(sub.userData.member.original.actor).slice(0, 18))
      items.push([`Make it ${n}`, () => { remember(); o.members = o.members || {}; o.members[k] = { ...(o.members[k] || {}), actor: n === sub.userData.member.original.actor ? undefined : n }; rebuildObjects(); }]);
    items.push(['Select the whole set', () => select(pid)]);
  } else if (!id.startsWith('base:') && id !== '__start') {
    const o = objById(id);
    const d = o.actor ? await details(o.actor) : null;
    const contents = d && d.choices && d.choices.Drop__DropActor;
    if (contents) for (const n of contents) items.push([`Holds ${n}`, () => { remember(); o.params = { ...(o.params || {}), Drop__DropActor: n }; renderProps(); }]);
    items.push(['Duplicate', duplicateSelected]);
  }
  items.push([id.startsWith('base:') ? 'Remove from our map' : 'Delete', deleteSelected]);
  menu.innerHTML = '';
  for (const [label, fn] of items) {
    const b = document.createElement('button');
    b.textContent = label;
    b.onclick = () => { closeMenu(); fn(); };
    menu.appendChild(b);
  }
  menu.style.left = Math.min(ev.clientX, innerWidth - 260) + 'px';
  menu.style.top = Math.min(ev.clientY, innerHeight - 40) + 'px';
  menu.style.display = 'block';
}

// ---- layouts, saving, building ----------------------------------------------------------------------------------------
async function listLayouts(selectName) {
  const names = await api('/api/layouts');
  if (!names.includes(selectName)) names.push(selectName);
  $('layoutSelect').innerHTML = names.map((n) => `<option ${n === selectName ? 'selected' : ''}>${n}</option>`).join('');
}
async function openLayout(name) {
  try { layout = await api('/api/layout/' + encodeURIComponent(name)); }
  catch { layout = { format: 1, base: 'DarkLinkStage', objects: [] }; }
  layout.objects = layout.objects || [];
  layout.objects.forEach((o) => { if (!o.id) o.id = newId(); });
  layoutName = name; undoStack.length = 0; redoStack.length = 0; dirty = false;
  $('baseSelect').value = layout.base;
  await loadBase(layout.base);
  rebuildObjects(); select(null); frameSelected();
  status(`Layout ${name}: ${layout.objects.length} placed on ${layout.base}`);
}
async function save() {
  await api('/api/layout/' + encodeURIComponent(layoutName), { method: 'POST', body: JSON.stringify(layout) });
  dirty = false; status('Saved ' + layoutName);
}
async function build() {
  await save();
  status('Building the dev map…');
  try {
    const r = await api('/api/build/' + encodeURIComponent(layoutName), { method: 'POST' });
    status((r.log.trim().split('\n').pop() || 'Built') + ' (restart the game, then Teleport > VR test map: the game keeps a map it has loaded)');
  } catch (e) { status('Build failed: ' + e.message); }
}

// ---- wiring -----------------------------------------------------------------------------------------------------------
$('search').addEventListener('input', renderPalette);
$('saveLayout').onclick = () => save().catch((e) => status('Save failed: ' + e.message));
$('buildMap').onclick = build;
$('newLayout').onclick = async () => {
  const n = prompt('Name for the new layout (letters, digits, - and _):', 'layout' + Date.now() % 1000);
  if (!n || !/^[A-Za-z0-9_-]+$/.test(n)) return;
  layout = { format: 1, base: $('baseSelect').value || 'DarkLinkStage', objects: [] };
  layoutName = n; await save(); await listLayouts(n); await openLayout(n);
};
$('layoutSelect').onchange = () => openLayout($('layoutSelect').value);
$('baseSelect').onchange = async () => { remember(); layout.base = $('baseSelect').value; delete layout.start; await loadBase(layout.base); rebuildObjects(); frameSelected(); };
for (const b of $('modeButtons').children) b.onclick = () => setMode(b.dataset.mode);
function setMode(m) {
  if (selected === '__start' && m === 'scale') return;
  gizmo.setMode(m);
  for (const b of $('modeButtons').children) b.classList.toggle('on', b.dataset.mode === m);
}
$('snap').onchange = applySnap;
$('textures').onchange = () => { loadBase(layout.base).then(() => { rebuildObjects(); }); };
$('baked').onchange = () => { lightBalance(); loadBase(layout.base); };
lightBalance();
$('showBase').onchange = () => {   // (the base map's pieces: solid, ghost (see through them) or hidden)
  baseGroup.visible = $('showBase').value !== 'hidden';
  const o = baseOpacity();
  for (const g of baseGroup.children) {   // (every mesh of each piece's model, however deep in it; water and lava stay see-through)
    const model = g.getObjectByName('model');
    if (!model) continue;
    const done = new Set();
    model.traverse((m) => {
      if (!m.isMesh || !m.material || done.has(m.material)) return;
      done.add(m.material);
      const fluid = !!m.material.userData.fluid;
      m.material.opacity = fluid ? Math.min(o, 0.55) : o; m.material.transparent = fluid || o < 1;
      m.material.depthWrite = !fluid && o >= 1; m.material.needsUpdate = true;
    });
  }
};
function texturesOn() { return $('textures').checked; }
function baseOpacity() { return $('showBase').value === 'solid' ? 1 : 0.3; }
$('undo').onclick = undo; $('redo').onclick = redo;
window.addEventListener('keydown', (ev) => {
  if (ev.target && ev.target.matches && ev.target.matches('input, textarea, select')) return;
  const k = ev.key.toLowerCase();
  if (ARROW_KEYS[k]) { fly.keys.add(k); ev.preventDefault(); return; }
  if (fly.drag && fly.drag.kind === 'look') { fly.keys.add(k); if (FLY_KEYS[k] || k === 'shift') { ev.preventDefault(); return; } }
  if (k === 'shift') fly.keys.add(k);
  if (ev.ctrlKey && k === 's') { ev.preventDefault(); save(); }
  else if (ev.ctrlKey && (k === 'y' || (k === 'z' && ev.shiftKey))) { ev.preventDefault(); redo(); }
  else if (ev.ctrlKey && k === 'z') { ev.preventDefault(); undo(); }
  else if (ev.ctrlKey && k === 'd') { ev.preventDefault(); duplicateSelected(); }
  else if (k === 'delete' || k === 'backspace') deleteSelected();
  else if (k === 'w') setMode('translate');
  else if (k === 'e') setMode('rotate');
  else if (k === 'r') setMode('scale');
  else if (k === 'f') frameSelected();
  else if (k === 'escape') select(null);
});
window.addEventListener('beforeunload', (ev) => { if (dirty) { ev.preventDefault(); ev.returnValue = ''; } });

// (for checks from a script: a frame drawn on request, as a PNG data URL)
window.devmapEditor = { snapshot() { resize(); renderer.render(scene, camera); return canvas.toDataURL('image/png'); },
  layout: () => layout, select, frame: frameSelected,
  look: (from, to) => { camera.position.set(...from); lookAt(new THREE.Vector3(...to)); } };

(async function start() {
  resize(); applySnap();
  status('Loading the catalog…');
  catalog = await api('/api/catalog');
  const prefabs = await api('/api/prefabs').catch(() => []);
  catalog = prefabs.map((p) => ({ name: 'prefab:' + p.name + (p.group ? '#' + p.group : ''), label: p.label || p.name, category: 'Prefabs',
    count: p.from.length, maps: p.from.slice(0, 3), from: p.from, group: p.group, module: p.name, pack: true, prefab: true })).concat(catalog);
  byName = new Map(catalog.map((a) => [a.name, a]));
  renderCategories(); renderPalette();
  const bases = await api('/api/bases');
  $('baseSelect').innerHTML = bases.map((b) => `<option>${b}</option>`).join('');
  await listLayouts('default');
  await openLayout('default');
  loadMarks();
})().catch((e) => status('Error: ' + e.message));
