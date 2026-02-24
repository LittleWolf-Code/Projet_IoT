
(function () {
  const defaultFloorPlans = [
    { z: 0, label: 'RDC (1st floor)', image: '/api/floor-plan/0' },
    { z: 1, label: 'R+1 (2nd floor)', image: '/api/floor-plan/1' }
  ];

  const state = {
    map: { width: 1000, height: 700, floorPlans: defaultFloorPlans },
    localization: { analysisEverySec: 30, windowSec: 60 },
    zones: [],
    anchors: [],
    positions: {},
    floorPlans: defaultFloorPlans,
    floorAspectByZ: {},
    selectedFloor: 0,
    selectedDevice: null,
    selectedHistory: null,
    showGrid: false,
    planRect: null,
    zoneDraft: null,
    zoneEditId: null,
    viewport: {
      zoom: 1,
      minZoom: 1,
      maxZoom: 4,
      x: 0,
      y: 0
    },
    interaction: {
      mode: 'none',
      pointerId: null,
      moved: false,
      startClientX: 0,
      startClientY: 0,
      startX: 0,
      startY: 0
    },
    anchorDrag: {
      mac: null,
      original: null,
      pending: null,
      saveInFlight: false
    },
    vertexDrag: {
      index: -1
    }
  };

  let viewBoxRafId = 0;
  let redrawRafId = 0;

  const floorFilter = document.getElementById('floor-filter');
  const analysisEvery = document.getElementById('analysis-every');
  const windowSec = document.getElementById('window-sec');
  const floorMap = document.getElementById('floor-map');
  const mapCoords = document.getElementById('map-coords');
  const deviceBody = document.getElementById('localized-devices-body');
  const zonesBody = document.getElementById('zones-table-body');
  const historyList = document.getElementById('history-list');
  const zoneFloor = document.getElementById('zone-floor');
  const zoneForm = document.getElementById('zone-form');
  const zoneNameInput = document.getElementById('zone-name');
  const zonePointsInput = document.getElementById('zone-points');
  const zoneColorInput = document.getElementById('zone-color');
  const zoneHelp = document.getElementById('zone-editor-help');
  const createZoneShapeBtn = document.getElementById('create-zone-shape-btn');
  const saveZoneBtn = document.getElementById('save-zone-btn');
  const cancelZoneEditBtn = document.getElementById('cancel-zone-edit-btn');
  const zoomInBtn = document.getElementById('zoom-in-btn');
  const zoomOutBtn = document.getElementById('zoom-out-btn');
  const resetViewBtn = document.getElementById('reset-view-btn');
  const toggleGridBtn = document.getElementById('toggle-grid-btn');

  if (!floorMap || !zoneForm) return;

  function cloneDefaultFloorPlans() {
    return defaultFloorPlans.map((plan) => ({ ...plan }));
  }

  function normalizeFloorPlans(plans) {
    const source = Array.isArray(plans) ? plans : [];
    const normalized = source
      .map((plan) => ({
        z: Number(plan.z),
        label: String(plan.label || `Etage ${plan.z}`),
        image: plan.image ? String(plan.image) : ''
      }))
      .filter((plan) => Number.isFinite(plan.z))
      .sort((a, b) => a.z - b.z);

    return normalized.length > 0 ? normalized : cloneDefaultFloorPlans();
  }

  function normalizeZone(zone) {
    if (!zone || typeof zone !== 'object') return null;

    let points = [];
    if (Array.isArray(zone.points) && zone.points.length >= 3) {
      points = zone.points
        .map((point) => ({ x: Number(point.x), y: Number(point.y) }))
        .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y));
    } else {
      const x = Number(zone.x);
      const y = Number(zone.y);
      const width = Number(zone.width);
      const height = Number(zone.height);
      if ([x, y, width, height].every(Number.isFinite)) {
        points = [
          { x, y },
          { x: x + width, y },
          { x: x + width, y: y + height },
          { x, y: y + height }
        ];
      }
    }

    if (points.length < 3) return null;

    const z = Number(zone.z);
    if (!Number.isFinite(z)) return null;

    const fallbackId = `zone-${z}-${String(zone.name || 'zone')
      .trim()
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '') || 'unnamed'}`;

    return {
      id: String(zone.id || fallbackId),
      name: String(zone.name || 'Zone'),
      z,
      color: String(zone.color || '#3b82f6'),
      points
    };
  }

  function getFloorMeta(z) {
    return state.floorPlans.find((plan) => Number(plan.z) === Number(z))
      || { z: Number(z), label: `Etage ${z}`, image: '' };
  }

  function syncFloorSelectors() {
    if (!Array.isArray(state.floorPlans) || state.floorPlans.length === 0) {
      state.floorPlans = cloneDefaultFloorPlans();
    }

    if (!state.floorPlans.some((plan) => Number(plan.z) === Number(state.selectedFloor))) {
      state.selectedFloor = Number(state.floorPlans[0].z);
    }

    const optionsHtml = state.floorPlans
      .map((plan) => `<option value="${plan.z}">${plan.label}</option>`)
      .join('');

    floorFilter.innerHTML = optionsHtml;
    zoneFloor.innerHTML = optionsHtml;
    floorFilter.value = String(state.selectedFloor);
    zoneFloor.value = String(state.selectedFloor);
  }

  function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
  }

  function fmtDuration(totalSec) {
    const sec = Math.max(0, Number(totalSec) || 0);
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    return h > 0 ? `${h}h ${m}min` : `${m}min`;
  }

  function fmtPoint(point) {
    return `(${Math.round(point.x)}, ${Math.round(point.y)})`;
  }

  function setCoordsMessage(message) {
    if (mapCoords && mapCoords.textContent !== message) {
      mapCoords.textContent = message;
    }
  }

  function defaultMapMessage() {
    if (state.zoneDraft) {
      return 'Deplacer les sommets bleus pour modeler la salle, puis cliquer Enregistrer zone.';
    }
    return 'Glisser un ESP32 pour le deplacer. Molette pour zoom, glisser le fond pour naviguer.';
  }

  function resetViewport() {
    state.viewport.zoom = 1;
    state.viewport.x = 0;
    state.viewport.y = 0;
  }

  function getCurrentViewBox() {
    const zoom = clamp(state.viewport.zoom, state.viewport.minZoom, state.viewport.maxZoom);
    state.viewport.zoom = zoom;

    const width = state.map.width / zoom;
    const height = state.map.height / zoom;
    const maxX = Math.max(0, state.map.width - width);
    const maxY = Math.max(0, state.map.height - height);
    state.viewport.x = clamp(state.viewport.x, 0, maxX);
    state.viewport.y = clamp(state.viewport.y, 0, maxY);

    return {
      x: state.viewport.x,
      y: state.viewport.y,
      width,
      height
    };
  }

  function applyViewBox() {
    const viewBox = getCurrentViewBox();
    floorMap.setAttribute('viewBox', `${viewBox.x} ${viewBox.y} ${viewBox.width} ${viewBox.height}`);
  }

  function applyViewBoxNextFrame() {
    if (viewBoxRafId) return;
    viewBoxRafId = requestAnimationFrame(() => {
      viewBoxRafId = 0;
      applyViewBox();
    });
  }

  function scheduleDrawMap() {
    if (redrawRafId) return;
    redrawRafId = requestAnimationFrame(() => {
      redrawRafId = 0;
      drawMap();
    });
  }

  function getPlanRect(planAspectRatio) {
    const mapAspect = state.map.width / state.map.height;
    if (planAspectRatio > mapAspect) {
      const width = state.map.width;
      const height = width / planAspectRatio;
      return {
        x: 0,
        y: (state.map.height - height) / 2,
        width,
        height
      };
    }

    const height = state.map.height;
    const width = height * planAspectRatio;
    return {
      x: (state.map.width - width) / 2,
      y: 0,
      width,
      height
    };
  }

  function mapToPlanPoint(point, planRect) {
    return {
      x: planRect.x + (Number(point.x) / state.map.width) * planRect.width,
      y: planRect.y + (Number(point.y) / state.map.height) * planRect.height
    };
  }

  function planToMapPoint(x, y, planRect) {
    if (!planRect || x < planRect.x || x > (planRect.x + planRect.width) || y < planRect.y || y > (planRect.y + planRect.height)) {
      return null;
    }

    return {
      x: clamp(((x - planRect.x) / planRect.width) * state.map.width, 0, state.map.width),
      y: clamp(((y - planRect.y) / planRect.height) * state.map.height, 0, state.map.height)
    };
  }

  function ensureFloorAspect(floorMeta) {
    const key = String(floorMeta.z);
    if (!floorMeta.image || state.floorAspectByZ[key]) return;

    const img = new Image();
    img.onload = () => {
      if (img.naturalWidth > 0 && img.naturalHeight > 0) {
        state.floorAspectByZ[key] = img.naturalWidth / img.naturalHeight;
        scheduleDrawMap();
      }
    };
    img.onerror = () => {
      state.floorAspectByZ[key] = state.map.width / state.map.height;
    };
    img.src = floorMeta.image;
  }

  function createSvgElement(tag, attributes = {}) {
    const element = document.createElementNS('http://www.w3.org/2000/svg', tag);
    Object.entries(attributes).forEach(([key, value]) => {
      element.setAttribute(key, value);
    });
    return element;
  }

  function appendMapText(x, y, text, options = {}) {
    const label = createSvgElement('text', {
      x,
      y,
      fill: options.fill || '#f8fafc',
      'font-size': options.size || 12,
      'font-weight': options.weight || 700,
      class: 'map-text-halo',
      'text-anchor': options.anchor || 'start',
      ...(options.attrs || {})
    });
    label.textContent = text;
    floorMap.appendChild(label);
  }

  function drawGrid(planRect) {
    const verticalSteps = 12;
    const horizontalSteps = 8;

    for (let i = 0; i <= verticalSteps; i += 1) {
      const x = planRect.x + (i / verticalSteps) * planRect.width;
      floorMap.appendChild(createSvgElement('line', {
        x1: x,
        y1: planRect.y,
        x2: x,
        y2: planRect.y + planRect.height,
        stroke: 'rgba(71, 85, 105, 0.25)',
        'stroke-width': 1
      }));
    }

    for (let i = 0; i <= horizontalSteps; i += 1) {
      const y = planRect.y + (i / horizontalSteps) * planRect.height;
      floorMap.appendChild(createSvgElement('line', {
        x1: planRect.x,
        y1: y,
        x2: planRect.x + planRect.width,
        y2: y,
        stroke: 'rgba(71, 85, 105, 0.25)',
        'stroke-width': 1
      }));
    }
  }

  function clientToSvgPoint(clientX, clientY) {
    const matrix = floorMap.getScreenCTM();
    if (!matrix) return null;

    const point = floorMap.createSVGPoint();
    point.x = clientX;
    point.y = clientY;
    return point.matrixTransform(matrix.inverse());
  }

  function eventToMapPoint(event) {
    const svgPoint = clientToSvgPoint(event.clientX, event.clientY);
    if (!svgPoint || !state.planRect) return null;
    return planToMapPoint(svgPoint.x, svgPoint.y, state.planRect);
  }

  function updateGridButton() {
    toggleGridBtn.textContent = state.showGrid ? 'Grille ON' : 'Grille OFF';
    toggleGridBtn.classList.toggle('active', state.showGrid);
  }

  function updateZoneEditorState() {
    const hasDraft = Boolean(state.zoneDraft);
    const isEdit = hasDraft && Boolean(state.zoneEditId);

    saveZoneBtn.disabled = !hasDraft;
    cancelZoneEditBtn.disabled = !hasDraft;
    saveZoneBtn.textContent = isEdit ? 'Mettre a jour zone' : 'Enregistrer zone';

    if (isEdit) {
      zoneHelp.textContent = 'Mode edition: deplacer les sommets puis enregistrer la zone.';
    } else if (hasDraft) {
      zoneHelp.textContent = 'Mode ajout: ajuster la forme sur la carte puis enregistrer.';
    } else {
      zoneHelp.textContent = 'Definir le nombre de points, cliquer "Creer polygone", puis deplacer les sommets sur la carte.';
    }
  }

  function updateStats() {
    const floor = state.selectedFloor;
    const positions = Object.values(state.positions).filter((p) => Number(p.z) === Number(floor));
    const zones = state.zones.filter((z) => Number(z.z) === Number(floor));
    const anchors = state.anchors.filter((a) => Number(a.z) === Number(floor));

    document.getElementById('stat-localized').textContent = positions.length;
    document.getElementById('stat-zones').textContent = zones.length;
    document.getElementById('stat-anchors').textContent = anchors.length;
    document.getElementById('map-badge').textContent = getFloorMeta(floor).label;
    document.getElementById('zone-count-badge').textContent = String(zones.length);
  }

  function findAnchorByMac(mac) {
    return state.anchors.find((anchor) => String(anchor.mac) === String(mac));
  }

  function updateLocalAnchor(mac, point, floor) {
    const idx = state.anchors.findIndex((anchor) => String(anchor.mac) === String(mac));
    if (idx < 0) return;
    state.anchors[idx] = {
      ...state.anchors[idx],
      x: Number(point.x),
      y: Number(point.y),
      z: Number(floor)
    };
  }

  function getAnchorSvgNodes(mac) {
    const nodes = Array.from(floorMap.querySelectorAll('[data-anchor-mac]'))
      .filter((node) => String(node.getAttribute('data-anchor-mac')) === String(mac));

    let marker = null;
    let label = null;
    nodes.forEach((node) => {
      const tag = node.tagName ? node.tagName.toLowerCase() : '';
      if (tag === 'rect') marker = node;
      if (tag === 'text') label = node;
    });

    return { marker, label };
  }

  function setAnchorDragVisual(mac, active) {
    if (!mac) return;
    const { marker } = getAnchorSvgNodes(mac);
    if (!marker) return;
    marker.classList.toggle('anchor-active', Boolean(active));
  }

  function setDraggedAnchorSvgPosition(mac, mapPoint) {
    if (!mac || !mapPoint || !state.planRect) return false;
    const { marker, label } = getAnchorSvgNodes(mac);
    if (!marker && !label) return false;

    const point = mapToPlanPoint(mapPoint, state.planRect);
    if (marker) {
      marker.setAttribute('x', String(point.x - 7));
      marker.setAttribute('y', String(point.y - 7));
    }
    if (label) {
      label.setAttribute('x', String(point.x + 11));
      label.setAttribute('y', String(point.y - 9));
    }
    return true;
  }

  function drawMap() {
    const floor = state.selectedFloor;
    const floorMeta = getFloorMeta(floor);
    ensureFloorAspect(floorMeta);
    floorMap.innerHTML = '';

    const ratio = state.floorAspectByZ[String(floor)] || (state.map.width / state.map.height);
    const planRect = getPlanRect(ratio);
    state.planRect = planRect;

    floorMap.appendChild(createSvgElement('rect', {
      x: planRect.x,
      y: planRect.y,
      width: planRect.width,
      height: planRect.height,
      fill: '#f8fafc'
    }));

    if (floorMeta.image) {
      floorMap.appendChild(createSvgElement('image', {
        href: floorMeta.image,
        x: planRect.x,
        y: planRect.y,
        width: planRect.width,
        height: planRect.height,
        preserveAspectRatio: 'none',
        class: 'floor-plan-image'
      }));
    }

    floorMap.appendChild(createSvgElement('rect', {
      x: planRect.x,
      y: planRect.y,
      width: planRect.width,
      height: planRect.height,
      fill: 'none',
      stroke: '#64748b',
      'stroke-width': 2
    }));

    if (state.showGrid) {
      drawGrid(planRect);
    }

    const floorZones = state.zones
      .filter((zone) => Number(zone.z) === Number(floor))
      .filter((zone) => !(state.zoneDraft && state.zoneEditId && zone.id === state.zoneEditId));

    floorZones.forEach((zone) => {
      const mapped = zone.points.map((point) => mapToPlanPoint(point, planRect));
      if (mapped.length < 3) return;

      floorMap.appendChild(createSvgElement('polygon', {
        points: mapped.map((point) => `${point.x},${point.y}`).join(' '),
        fill: `${zone.color || '#3b82f6'}26`,
        stroke: zone.color || '#3b82f6',
        'stroke-width': 2
      }));

      const cx = mapped.reduce((sum, point) => sum + point.x, 0) / mapped.length;
      const cy = mapped.reduce((sum, point) => sum + point.y, 0) / mapped.length;
      appendMapText(cx, cy, zone.name, { fill: '#0f172a', size: 13, weight: 700, anchor: 'middle' });
    });

    if (state.zoneDraft && Number(state.zoneDraft.z) === Number(floor) && Array.isArray(state.zoneDraft.points) && state.zoneDraft.points.length >= 3) {
      const mappedDraft = state.zoneDraft.points.map((point) => mapToPlanPoint(point, planRect));
      const color = state.zoneDraft.color || '#3b82f6';

      floorMap.appendChild(createSvgElement('polygon', {
        points: mappedDraft.map((point) => `${point.x},${point.y}`).join(' '),
        fill: `${color}33`,
        stroke: color,
        'stroke-width': 2.5,
        'stroke-dasharray': '8 4'
      }));

      mappedDraft.forEach((point, index) => {
        floorMap.appendChild(createSvgElement('circle', {
          cx: point.x,
          cy: point.y,
          r: 7,
          fill: '#0ea5e9',
          stroke: '#f8fafc',
          'stroke-width': 2,
          class: 'zone-vertex-handle',
          'data-zone-vertex': index
        }));

        appendMapText(point.x, point.y - 12, String(index + 1), {
          fill: '#0f172a',
          size: 10,
          weight: 700,
          anchor: 'middle',
          attrs: {
            class: 'map-text-halo zone-vertex-handle',
            'data-zone-vertex': index
          }
        });
      });
    }

    state.anchors
      .filter((anchor) => Number(anchor.z) === Number(floor))
      .forEach((anchor) => {
        const point = mapToPlanPoint(anchor, planRect);
        const active = state.interaction.mode === 'anchor' && state.anchorDrag.mac === anchor.mac;
        const markerClass = `anchor-marker anchor-draggable${active ? ' anchor-active' : ''}`;

        floorMap.appendChild(createSvgElement('rect', {
          x: point.x - 7,
          y: point.y - 7,
          width: 14,
          height: 14,
          rx: 2,
          ry: 2,
          fill: '#06b6d4',
          stroke: '#0f172a',
          'stroke-width': 1.5,
          class: markerClass,
          'data-anchor-mac': anchor.mac
        }));

        appendMapText(point.x + 11, point.y - 9, anchor.name, {
          fill: '#0f172a',
          size: 11,
          weight: 600,
          attrs: {
            class: 'map-text-halo anchor-draggable',
            'data-anchor-mac': anchor.mac
          }
        });
      });

    Object.values(state.positions)
      .filter((position) => Number(position.z) === Number(floor))
      .forEach((position) => {
        const point = mapToPlanPoint(position, planRect);
        const selected = state.selectedDevice === position.name;
        const radius = selected ? 9 : 7;

        floorMap.appendChild(createSvgElement('circle', {
          cx: point.x,
          cy: point.y,
          r: radius + 3,
          fill: selected ? 'rgba(16, 185, 129, 0.22)' : 'rgba(245, 158, 11, 0.2)'
        }));

        floorMap.appendChild(createSvgElement('circle', {
          cx: point.x,
          cy: point.y,
          r: radius,
          fill: selected ? '#10b981' : '#f59e0b',
          stroke: '#ffffff',
          'stroke-width': 2
        }));

        appendMapText(point.x + 12, point.y - 10, `${position.name}${position.room ? ` (${position.room})` : ''}`, {
          fill: '#0f172a',
          size: 11,
          weight: 700
        });
      });

    applyViewBox();
  }

  function renderDeviceTable() {
    const floor = state.selectedFloor;
    const positions = Object.values(state.positions)
      .filter((position) => Number(position.z) === Number(floor))
      .sort((a, b) => b.updatedAt - a.updatedAt);

    document.getElementById('device-count-badge').textContent = String(positions.length);

    if (positions.length === 0) {
      deviceBody.innerHTML = '<tr><td colspan="3"><div class="empty-state"><h3>Aucun device localise</h3><p>Verifier les coordonnees ESP32 et les scans BLE.</p></div></td></tr>';
      return;
    }

    deviceBody.innerHTML = positions.map((position) => {
      const activeClass = state.selectedDevice === position.name ? 'row-active' : '';
      const room = position.room || 'Hors zone';
      const ago = timeSince(position.updatedAt);
      return `<tr class="clickable-row ${activeClass}" data-device="${position.name}">
        <td><strong>${position.name}</strong><br><small>${fmtPoint(position)}</small></td>
        <td>${room}</td>
        <td>${ago}</td>
      </tr>`;
    }).join('');
  }

  function renderZonesTable() {
    if (state.zones.length === 0) {
      zonesBody.innerHTML = '<tr><td colspan="4"><div class="empty-state"><h3>Aucune zone</h3><p>Ajoutez des salles pour suivre la presence des devices.</p></div></td></tr>';
      return;
    }

    const sortedZones = state.zones
      .slice()
      .sort((a, b) => (Number(a.z) - Number(b.z)) || String(a.name).localeCompare(String(b.name), 'fr'));

    zonesBody.innerHTML = sortedZones.map((zone) => {
      const points = Array.isArray(zone.points) ? zone.points : [];
      const xs = points.map((p) => Number(p.x)).filter(Number.isFinite);
      const ys = points.map((p) => Number(p.y)).filter(Number.isFinite);
      const minX = xs.length ? Math.round(Math.min(...xs)) : 0;
      const maxX = xs.length ? Math.round(Math.max(...xs)) : 0;
      const minY = ys.length ? Math.round(Math.min(...ys)) : 0;
      const maxY = ys.length ? Math.round(Math.max(...ys)) : 0;
      const footprint = `${points.length} pts | x:${minX}-${maxX}, y:${minY}-${maxY}`;
      const floor = getFloorMeta(zone.z);

      return `<tr>
        <td><strong>${zone.name}</strong></td>
        <td>${floor.label}</td>
        <td>${footprint}</td>
        <td class="zone-actions">
          <button class="btn btn-neutral btn-sm edit-zone-btn" data-zone-id="${zone.id}">Editer</button>
          <button class="btn btn-danger btn-sm delete-zone-btn" data-zone-id="${zone.id}">Supprimer</button>
        </td>
      </tr>`;
    }).join('');
  }

  async function loadHistory(deviceName) {
    try {
      const response = await fetch(`/api/device-history/${encodeURIComponent(deviceName)}`);
      const data = await response.json();
      state.selectedHistory = data;
      renderHistory();
    } catch (err) {
      showToast('Erreur chargement historique', 'error');
    }
  }

  function renderHistory() {
    const selected = state.selectedHistory;
    const label = document.getElementById('history-device-label');

    if (!state.selectedDevice || !selected) {
      label.textContent = 'Aucun';
      historyList.innerHTML = '<div class="empty-state"><h3>Selectionnez un device</h3><p>Cliquez sur une ligne pour afficher les durees par salle.</p></div>';
      return;
    }

    label.textContent = state.selectedDevice;
    const stays = selected.stays || [];

    if (stays.length === 0) {
      historyList.innerHTML = '<div class="empty-state"><h3>Aucun historique</h3></div>';
      return;
    }

    historyList.innerHTML = stays
      .slice()
      .reverse()
      .map((stay) => {
        const start = new Date(stay.start).toLocaleTimeString('fr-FR');
        const end = new Date(stay.end).toLocaleTimeString('fr-FR');
        return `<div class="history-item">
          <div><strong>${stay.room || 'Hors zone'}</strong> - ${fmtDuration(stay.durationSec)}</div>
          <small>${start} -> ${end}${stay.ongoing ? ' (en cours)' : ''}</small>
        </div>`;
      }).join('');
  }

  function setSelectedFloor(floor, resetView) {
    state.selectedFloor = Number(floor);
    floorFilter.value = String(state.selectedFloor);
    zoneFloor.value = String(state.selectedFloor);
    if (resetView) resetViewport();
    updateStats();
    scheduleDrawMap();
    renderDeviceTable();
  }

  function createZoneDraft(pointCount) {
    const count = clamp(Math.round(Number(pointCount) || 4), 3, 12);
    const floor = Number(zoneFloor.value);

    if (Number(state.selectedFloor) !== floor) {
      setSelectedFloor(floor, true);
    }

    const center = { x: state.map.width / 2, y: state.map.height / 2 };
    const radius = Math.min(state.map.width, state.map.height) * 0.12;
    const points = [];

    for (let i = 0; i < count; i += 1) {
      const angle = (-Math.PI / 2) + ((Math.PI * 2 * i) / count);
      points.push({
        x: clamp(center.x + (radius * Math.cos(angle)), 0, state.map.width),
        y: clamp(center.y + (radius * Math.sin(angle)), 0, state.map.height)
      });
    }

    state.zoneDraft = {
      z: floor,
      color: zoneColorInput.value || '#3b82f6',
      points
    };

    if (!state.zoneEditId) {
      zoneNameInput.focus();
    }

    updateZoneEditorState();
    setCoordsMessage(defaultMapMessage());
    scheduleDrawMap();
  }

  function startZoneEdit(zone) {
    state.zoneEditId = zone.id;
    state.zoneDraft = {
      z: Number(zone.z),
      color: zone.color || '#3b82f6',
      points: (zone.points || [])
        .map((point) => ({ x: Number(point.x), y: Number(point.y) }))
        .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y))
    };

    if (state.zoneDraft.points.length < 3) {
      showToast('Zone invalide: au moins 3 points requis', 'error');
      state.zoneDraft = null;
      state.zoneEditId = null;
      updateZoneEditorState();
      return;
    }

    zoneNameInput.value = zone.name || '';
    zoneFloor.value = String(zone.z);
    zoneColorInput.value = zone.color || '#3b82f6';
    zonePointsInput.value = String(clamp(state.zoneDraft.points.length, 3, 12));

    setSelectedFloor(Number(zone.z), true);
    updateZoneEditorState();
    setCoordsMessage(defaultMapMessage());
    scheduleDrawMap();
  }

  function cancelZoneDraft(resetFields) {
    state.zoneDraft = null;
    state.zoneEditId = null;

    if (resetFields) {
      zoneForm.reset();
      zonePointsInput.value = '4';
      zoneColorInput.value = '#3b82f6';
    }

    zoneFloor.value = String(state.selectedFloor);
    updateZoneEditorState();
    setCoordsMessage(defaultMapMessage());
    scheduleDrawMap();
  }

  async function persistAnchorPosition() {
    const mac = state.anchorDrag.mac;
    const pending = state.anchorDrag.pending;

    if (!mac || !pending || state.anchorDrag.saveInFlight) return;

    state.anchorDrag.saveInFlight = true;
    try {
      const payload = {
        x: Number(pending.x.toFixed(2)),
        y: Number(pending.y.toFixed(2)),
        z: Number(state.selectedFloor)
      };

      const response = await fetch(`/api/devices/${encodeURIComponent(mac)}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });

      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.error || 'Sauvegarde impossible');
      }

      updateLocalAnchor(mac, payload, payload.z);
      showToast('Position ESP32 mise a jour', 'success');
    } catch (err) {
      if (state.anchorDrag.original) {
        updateLocalAnchor(mac, state.anchorDrag.original, state.anchorDrag.original.z);
        scheduleDrawMap();
      }
      showToast('Erreur deplacement ESP32', 'error');
    } finally {
      state.anchorDrag.saveInFlight = false;
      state.anchorDrag.mac = null;
      state.anchorDrag.original = null;
      state.anchorDrag.pending = null;
      setCoordsMessage(defaultMapMessage());
    }
  }

  function beginInteraction(mode, event) {
    state.interaction.mode = mode;
    state.interaction.pointerId = event.pointerId;
    state.interaction.moved = false;
    state.interaction.startClientX = event.clientX;
    state.interaction.startClientY = event.clientY;
    state.interaction.startX = state.viewport.x;
    state.interaction.startY = state.viewport.y;
    floorMap.classList.add('dragging');
    floorMap.setPointerCapture(event.pointerId);
  }

  function stopInteraction(event) {
    const previousMode = state.interaction.mode;
    const previousAnchorMac = state.anchorDrag.mac;

    if (state.interaction.pointerId !== null && event) {
      try {
        floorMap.releasePointerCapture(state.interaction.pointerId);
      } catch (err) {
        // ignore release issues
      }
    }

    floorMap.classList.remove('dragging');
    state.interaction.mode = 'none';
    state.interaction.pointerId = null;
    state.interaction.moved = false;
    state.vertexDrag.index = -1;

    if (previousMode === 'anchor' && previousAnchorMac) {
      setAnchorDragVisual(previousAnchorMac, false);
    }
  }

  async function loadMapState() {
    try {
      const response = await fetch('/api/map-state');
      const data = await response.json();

      state.map = { ...state.map, ...(data.map || {}) };
      state.map.width = Math.max(100, Number(state.map.width) || 1000);
      state.map.height = Math.max(100, Number(state.map.height) || 700);

      state.floorPlans = normalizeFloorPlans(state.map.floorPlans);
      state.map.floorPlans = state.floorPlans;
      syncFloorSelectors();

      state.localization = data.localization || state.localization;
      state.zones = (Array.isArray(data.zones) ? data.zones : [])
        .map(normalizeZone)
        .filter(Boolean);
      state.anchors = Array.isArray(data.anchors) ? data.anchors : [];
      state.positions = data.positions || {};

      analysisEvery.value = String(state.localization.analysisEverySec || 30);
      windowSec.value = String(state.localization.windowSec || 60);

      updateStats();
      scheduleDrawMap();
      renderDeviceTable();
      renderZonesTable();
    } catch (err) {
      showToast('Erreur chargement carte', 'error');
    }
  }

  function zoomAt(clientX, clientY, factor) {
    const previousZoom = state.viewport.zoom;
    const nextZoom = clamp(previousZoom * factor, state.viewport.minZoom, state.viewport.maxZoom);
    if (nextZoom === previousZoom) return;

    const oldWidth = state.map.width / previousZoom;
    const oldHeight = state.map.height / previousZoom;
    const focus = clientToSvgPoint(clientX, clientY) || {
      x: state.viewport.x + (oldWidth / 2),
      y: state.viewport.y + (oldHeight / 2)
    };

    const relX = (focus.x - state.viewport.x) / oldWidth;
    const relY = (focus.y - state.viewport.y) / oldHeight;
    const newWidth = state.map.width / nextZoom;
    const newHeight = state.map.height / nextZoom;

    state.viewport.zoom = nextZoom;
    state.viewport.x = focus.x - (relX * newWidth);
    state.viewport.y = focus.y - (relY * newHeight);

    applyViewBox();
  }

  function zoomCentered(factor) {
    const rect = floorMap.getBoundingClientRect();
    const centerX = rect.left + (rect.width / 2);
    const centerY = rect.top + (rect.height / 2);
    zoomAt(centerX, centerY, factor);
  }

  floorFilter.addEventListener('change', () => {
    state.selectedDevice = null;
    state.selectedHistory = null;
    renderHistory();
    cancelZoneDraft(false);
    setSelectedFloor(Number(floorFilter.value), true);
  });

  zoneFloor.addEventListener('change', () => {
    if (state.zoneDraft) {
      const targetFloor = Number(zoneFloor.value);
      state.zoneDraft.z = targetFloor;
      setSelectedFloor(targetFloor, true);
    }
  });

  zoneColorInput.addEventListener('input', () => {
    if (state.zoneDraft) {
      state.zoneDraft.color = zoneColorInput.value;
      scheduleDrawMap();
    }
  });

  createZoneShapeBtn.addEventListener('click', () => {
    createZoneDraft(zonePointsInput.value);
  });

  cancelZoneEditBtn.addEventListener('click', () => {
    cancelZoneDraft(true);
  });

  zoomInBtn.addEventListener('click', () => zoomCentered(1.2));
  zoomOutBtn.addEventListener('click', () => zoomCentered(0.84));
  resetViewBtn.addEventListener('click', () => {
    resetViewport();
    applyViewBox();
  });

  toggleGridBtn.addEventListener('click', () => {
    state.showGrid = !state.showGrid;
    updateGridButton();
    scheduleDrawMap();
  });

  floorMap.addEventListener('wheel', (event) => {
    event.preventDefault();
    const factor = event.deltaY < 0 ? 1.14 : 0.88;
    zoomAt(event.clientX, event.clientY, factor);
  }, { passive: false });

  floorMap.addEventListener('pointerdown', (event) => {
    if (event.button !== 0) return;

    const target = event.target;
    const vertexIndex = target && target.getAttribute ? target.getAttribute('data-zone-vertex') : null;
    const anchorMac = target && target.getAttribute ? target.getAttribute('data-anchor-mac') : null;

    if (vertexIndex !== null && state.zoneDraft && Number(state.zoneDraft.z) === Number(state.selectedFloor)) {
      state.vertexDrag.index = Number(vertexIndex);
      beginInteraction('vertex', event);
      return;
    }

    if (anchorMac) {
      const anchor = findAnchorByMac(anchorMac);
      if (anchor) {
        state.anchorDrag.mac = anchorMac;
        state.anchorDrag.original = { x: Number(anchor.x), y: Number(anchor.y), z: Number(anchor.z) };
        state.anchorDrag.pending = { x: Number(anchor.x), y: Number(anchor.y), z: Number(anchor.z) };
        beginInteraction('anchor', event);
        setAnchorDragVisual(anchorMac, true);
        setCoordsMessage(`Deplacement ESP32 ${anchor.name}: relacher pour sauvegarder`);
        return;
      }
    }

    beginInteraction('pan', event);
  });

  floorMap.addEventListener('pointermove', (event) => {
    if (state.interaction.mode === 'none') return;

    const dxPx = event.clientX - state.interaction.startClientX;
    const dyPx = event.clientY - state.interaction.startClientY;
    if (Math.abs(dxPx) + Math.abs(dyPx) > 4) {
      state.interaction.moved = true;
    }

    if (state.interaction.mode === 'pan') {
      const zoom = clamp(state.viewport.zoom, state.viewport.minZoom, state.viewport.maxZoom);
      const viewWidth = state.map.width / zoom;
      const viewHeight = state.map.height / zoom;
      const dxMap = (dxPx / Math.max(1, floorMap.clientWidth)) * viewWidth;
      const dyMap = (dyPx / Math.max(1, floorMap.clientHeight)) * viewHeight;

      state.viewport.x = state.interaction.startX - dxMap;
      state.viewport.y = state.interaction.startY - dyMap;
      applyViewBoxNextFrame();
      return;
    }

    if (state.interaction.mode === 'anchor') {
      const mapPoint = eventToMapPoint(event);
      if (!mapPoint || !state.anchorDrag.mac) return;

      state.anchorDrag.pending = {
        x: Number(mapPoint.x),
        y: Number(mapPoint.y),
        z: Number(state.selectedFloor)
      };

      updateLocalAnchor(state.anchorDrag.mac, mapPoint, state.selectedFloor);
      if (!setDraggedAnchorSvgPosition(state.anchorDrag.mac, mapPoint)) {
        scheduleDrawMap();
      }
      setCoordsMessage(`ESP32: x=${Math.round(mapPoint.x)} y=${Math.round(mapPoint.y)} (relacher pour sauvegarder)`);
      return;
    }

    if (state.interaction.mode === 'vertex') {
      const mapPoint = eventToMapPoint(event);
      if (!mapPoint || !state.zoneDraft) return;

      const index = state.vertexDrag.index;
      if (index < 0 || index >= state.zoneDraft.points.length) return;

      state.zoneDraft.points[index] = { x: Number(mapPoint.x), y: Number(mapPoint.y) };
      setCoordsMessage(`Sommet ${index + 1}: x=${Math.round(mapPoint.x)} y=${Math.round(mapPoint.y)}`);
      scheduleDrawMap();
    }
  });

  async function finishPointerInteraction(event) {
    const mode = state.interaction.mode;
    const moved = state.interaction.moved;
    stopInteraction(event);

    if (mode === 'anchor') {
      if (moved) {
        await persistAnchorPosition();
      } else {
        state.anchorDrag.mac = null;
        state.anchorDrag.original = null;
        state.anchorDrag.pending = null;
        setCoordsMessage(defaultMapMessage());
      }
      return;
    }

    if (mode === 'vertex') {
      setCoordsMessage(defaultMapMessage());
      return;
    }

    if (mode === 'pan') {
      setCoordsMessage(defaultMapMessage());
    }
  }

  floorMap.addEventListener('pointerup', (event) => {
    finishPointerInteraction(event);
  });

  floorMap.addEventListener('pointercancel', (event) => {
    finishPointerInteraction(event);
  });

  floorMap.addEventListener('dblclick', () => {
    resetViewport();
    applyViewBox();
  });

  document.getElementById('save-localization-btn').addEventListener('click', async () => {
    try {
      const payload = {
        analysisEverySec: Number(analysisEvery.value),
        windowSec: Number(windowSec.value)
      };

      const response = await fetch('/api/localization/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      const data = await response.json();

      if (!response.ok) {
        showToast(data.error || 'Erreur sauvegarde', 'error');
        return;
      }

      state.localization = data.localization;
      showToast('Parametres de localisation enregistres', 'success');
    } catch (err) {
      showToast('Erreur reseau', 'error');
    }
  });

  zoneForm.addEventListener('submit', async (event) => {
    event.preventDefault();

    if (!state.zoneDraft || !Array.isArray(state.zoneDraft.points) || state.zoneDraft.points.length < 3) {
      showToast('Creer et ajuster un polygone avant sauvegarde', 'error');
      return;
    }

    const payload = {
      id: state.zoneEditId || undefined,
      name: zoneNameInput.value.trim(),
      z: Number(zoneFloor.value),
      color: zoneColorInput.value,
      points: state.zoneDraft.points.map((point) => ({
        x: Number(point.x.toFixed(2)),
        y: Number(point.y.toFixed(2))
      }))
    };

    if (!payload.name) {
      showToast('Nom de salle requis', 'error');
      return;
    }

    try {
      const response = await fetch('/api/zones', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });

      const data = await response.json();
      if (!response.ok) {
        showToast(data.error || 'Erreur creation zone', 'error');
        return;
      }

      showToast(state.zoneEditId ? 'Zone mise a jour' : 'Zone ajoutee', 'success');
      cancelZoneDraft(true);
    } catch (err) {
      showToast('Erreur reseau', 'error');
    }
  });

  zonesBody.addEventListener('click', async (event) => {
    const editBtn = event.target.closest('.edit-zone-btn');
    if (editBtn) {
      const zoneId = editBtn.getAttribute('data-zone-id');
      const zone = state.zones.find((item) => item.id === zoneId);
      if (!zone) {
        showToast('Zone introuvable', 'error');
        return;
      }
      startZoneEdit(zone);
      return;
    }

    const deleteBtn = event.target.closest('.delete-zone-btn');
    if (!deleteBtn) return;

    const zoneId = deleteBtn.getAttribute('data-zone-id');
    if (!zoneId) return;
    if (!confirm('Supprimer cette zone ?')) return;

    try {
      const response = await fetch(`/api/zones/${encodeURIComponent(zoneId)}`, { method: 'DELETE' });
      if (!response.ok) {
        showToast('Suppression impossible', 'error');
        return;
      }
      showToast('Zone supprimee', 'success');
    } catch (err) {
      showToast('Erreur reseau', 'error');
    }
  });

  deviceBody.addEventListener('click', (event) => {
    const row = event.target.closest('tr[data-device]');
    if (!row) return;

    const deviceName = row.getAttribute('data-device');
    if (!deviceName) return;

    state.selectedDevice = deviceName;
    renderDeviceTable();
    scheduleDrawMap();
    loadHistory(deviceName);
  });

  socket.on('position:update', (positions) => {
    state.positions = positions || {};
    updateStats();
    scheduleDrawMap();
    renderDeviceTable();
    if (state.selectedDevice) {
      loadHistory(state.selectedDevice);
    }
  });

  socket.on('zones:update', (zones) => {
    state.zones = (Array.isArray(zones) ? zones : [])
      .map(normalizeZone)
      .filter(Boolean);
    if (state.zoneEditId && !state.zones.some((zone) => zone.id === state.zoneEditId)) {
      cancelZoneDraft(false);
    }
    updateStats();
    scheduleDrawMap();
    renderZonesTable();
  });

  socket.on('devices:update', () => {
    loadMapState();
  });

  syncFloorSelectors();
  updateGridButton();
  updateZoneEditorState();
  setCoordsMessage(defaultMapMessage());
  loadMapState();
  renderHistory();
})();
