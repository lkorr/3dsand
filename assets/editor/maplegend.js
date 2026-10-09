/**
 * THE MAP LEGEND: what every colour, line and badge on the World map page
 * means, as a sidebar panel of swatches. Each swatch is drawn with the same
 * colours and dashes the painters use (map.js paintFrame / paintSites /
 * paintLines / paintSculpt, mapvector.js paint, maplocale.js paintWorld /
 * paintLocale) -- change a painter's colour and change its row here too.
 *
 * Two sections: the WHOLE MAP (the world view) and INSIDE A LOCALE (the page a
 * locale opens to on double-click), shown by `sync(inLocale)`. Roads, regions,
 * markers and sites paint in both, so the locale section lists only what is
 * the locale page's own and says so.
 */
const SW = 36, SH = 18;

function swatch(el, draw) {
  const cv = el('canvas', {width: SW, height: SH, class: 'maplegend-sw'});
  const g = cv.getContext('2d');
  g.imageSmoothingEnabled = false;
  g.fillStyle = '#0e1219'; g.fillRect(0, 0, SW, SH);
  draw(g);
  return cv;
}
const rect = (g, col, w, dash, x = 6, y = 4, ww = SW - 12, hh = SH - 8) => {
  g.strokeStyle = col; g.lineWidth = w; g.setLineDash(dash || []);
  g.strokeRect(x + 0.5, y + 0.5, ww, hh); g.setLineDash([]);
};
const fill = (g, col, a, x = 6, y = 4, ww = SW - 12, hh = SH - 8) => {
  g.globalAlpha = a; g.fillStyle = col; g.fillRect(x, y, ww, hh); g.globalAlpha = 1;
};
const line = (g, col, w, dash, pts) => {
  g.strokeStyle = col; g.lineWidth = w; g.setLineDash(dash || []);
  g.beginPath(); pts.forEach(([x, y], k) => (k ? g.lineTo(x, y) : g.moveTo(x, y))); g.stroke(); g.setLineDash([]);
};
/** A colour ramp left to right (the climate overlays' stops). */
const grad = (g, stops) => {
  for (let x = 0; x < SW; x++) {
    const t = x / (SW - 1) * (stops.length - 1), i = Math.min(stops.length - 2, Math.floor(t)), u = t - i;
    const c = stops[i].map((v, q) => Math.round(v + (stops[i + 1][q] - v) * u));
    g.fillStyle = `rgb(${c[0]},${c[1]},${c[2]})`; g.fillRect(x, 0, 1, SH);
  }
};
/** A road as mapvector.js paints it: shoulder band, surface band, centreline. */
const road = (g, surface, centre, dash) => {
  g.lineCap = 'butt';
  line(g, surface, 14, null, [[2, 9], [SW - 2, 9]]); fill(g, '#0e1219', 0.82, 0, 0, SW, 2); fill(g, '#0e1219', 0.82, 0, 16, SW, 2);
  g.globalAlpha = 0.8; line(g, surface, 6, null, [[2, 9], [SW - 2, 9]]); g.globalAlpha = 1;
  line(g, centre, 1, dash, [[2, 9.5], [SW - 2, 9.5]]);
};

/**
 * `el` is the page's element helper; `iconCanvas(name)` the 12 x 12 marker
 * icon; `lut` = {land, sea} the height ramp's colour tables (Uint8 r,g,b x 256).
 */
export function buildLegend(el, iconCanvas, lut) {
  const badge = (ring, icon) => g => {
    g.fillStyle = ring; g.fillRect(10, 1, 16, 16);
    g.fillStyle = '#0e1219'; g.fillRect(11, 2, 14, 14);
    g.drawImage(iconCanvas(icon), 12, 3, 12, 12);
  };
  const ramp = g => {
    for (let x = 0; x < SW; x++) {
      const sea = x < 8, t = sea ? (8 - x) / 8 : (x - 8) / (SW - 9), i = Math.min(255, (t * 255) | 0) * 3, L = sea ? lut.sea : lut.land;
      g.fillStyle = `rgb(${L[i]},${L[i + 1]},${L[i + 2]})`; g.fillRect(x, 0, 1, SH);
    }
    line(g, 'rgba(0,0,0,0.45)', 1, null, [[8.5, 0], [8.5, SH]]);
    line(g, 'rgba(0,0,0,0.3)', 1, null, [[22.5, 0], [22.5, SH]]);
  };

  // [swatch, name, what it is]
  const WORLD = [
    ['Ground', null],
    [ramp, 'height colours', 'The ground’s height: blues are under the sea, greens to browns to pale are low to high land. Shaded as if lit from the north-west.'],
    [g => { fill(g, '#5a7a4a', 1, 0, 0, SW, SH); line(g, 'rgba(0,0,0,0.38)', 1, null, [[0, 6.5], [SW, 6.5]]); line(g, 'rgba(0,0,0,0.68)', 1, null, [[0, 12.5], [SW, 12.5]]); },
     'contour lines', 'Thin darker lines join points of equal height (the interval is in the toolbar; auto follows the zoom). Every 5th is bold; the 0 m line is the coast.'],
    [g => { fill(g, '#5a7a4a', 1, 0, 0, SW, SH); fill(g, '#d2b46a', 0.35, 0, 0, SW / 2, SH); fill(g, '#3a6a3a', 0.35, SW / 2, 0, SW / 2, SH); },
     'biome tint', 'The coloured wash over the ground is the biome plane (what grows where, 25.6 m cells). Opacity is the “biomes” slider.'],
    [g => { fill(g, 'rgb(255,150,60)', 0.55, 3, 3, 14, 12); fill(g, 'rgb(80,160,255)', 0.55, 19, 3, 14, 12); },
     'sculpt layer', 'Fine hand-sculpted edits on top of the height plane: orange = raised, blue = lowered. The “sculpt” checkbox hides it.'],
    ['Map frame', null],
    [g => { rect(g, '#ffd866', 1); rect(g, 'rgba(255,216,102,0.45)', 1, [4, 4], 2, 1, SW - 4, SH - 3); },
     'yellow box', 'Solid: the map’s edge (map.json extent). Dashed: the edge fade, the band outside it over which the authored heights ease off to the plane’s default.'],
    [g => line(g, '#ffd866', 1, null, [[SW / 2 - 6, 9.5], [SW / 2 + 6, 9.5], [SW / 2 + 0.5, 9.5], [SW / 2 + 0.5, 3], [SW / 2 + 0.5, 16]]),
     'yellow cross', 'World coordinate (0, 0).'],
    ['Places', null],
    [g => { fill(g, '#7fd4ff', 0.12); rect(g, 'rgba(127,212,255,0.6)', 1, [5, 3]); },
     'cyan dashed box', 'A LOCALE’s bounds (a hand-built place such as Harrowby). Its buildings, villagers and paths are stored with it. Double-click it to open it.'],
    [badge('#7fd4ff', 'village'), 'cyan badge', 'The locale’s anchor and name; the icon is its kind (village, castle, camp, …).'],
    [badge('#c8b48a', 'shrine'), 'tan badge', 'A MARKER: a labelled pin for a point of interest. Annotation only — the game builds nothing from it.'],
    [g => { fill(g, '#c9a227', 0.16); rect(g, '#c9a227', 1, [6, 3]); },
     'tinted dashed shape', 'A REGION: a named area, in its own colour (a forest’s name, a kingdom). Annotation only.'],
    ['Roads and paths', null],
    [g => road(g, '#9a948a', '#5a4630', [4, 3]), 'path (dashed centre)', 'A footpath the game cuts into the ground. The middle band is its width, coloured by its surface (packed gravel ≈ grey, packed earth ≈ brown); the faint wider band is the graded verge either side. Its name is written beside it.'],
    [g => road(g, '#8d8d90', '#2a2a2a', null), 'road / track (solid centre)', 'Same, for a wider road or cart track.'],
    [g => { line(g, '#c8b48a', 1, null, [[2, 9.5], [SW - 2, 9.5]]); g.fillStyle = '#1c1712'; g.fillRect(15, 7, 5, 5); g.fillStyle = '#ffd866'; g.fillRect(16, 8, 3, 3); },
     'yellow square on a road', 'A junction: two roads share this vertex (shown with the Road tool or a road selected).'],
    [g => { g.fillStyle = '#0e1219'; g.fillRect(12, 4, 11, 9); g.fillStyle = '#c8b48a'; g.fillRect(13, 5, 3, 3); g.fillRect(19, 5, 3, 3); g.fillRect(16, 9, 3, 3); },
     'stepping stones', 'A FORD: a road crossing a river. The engine lays the road deck across the channel inside the road’s width, the river’s water above it.'],
    ['Rivers and lakes (map.json rivers[] / lakes[])', null],
    [g => { g.globalAlpha = 0.7; g.fillStyle = '#2f7fe0'; g.beginPath(); g.moveTo(2, 7); g.lineTo(SW - 2, 5); g.lineTo(SW - 2, 13); g.lineTo(2, 11); g.closePath(); g.fill(); g.globalAlpha = 1;
           line(g, 'rgba(124,192,255,0.5)', 1, null, [[2, 9], [SW - 2, 9]]); line(g, 'rgba(230,245,255,0.85)', 1, null, [[18.5, 3], [18.5, 15]]); },
     'blue ribbon', 'A RIVER at its width, as the engine bakes it (the River tool’s live preview of the bake): flat REACHES of still water stepping down toward the mouth; the thin white ticks across it are where a reach steps down. Too narrow to draw at this zoom = a blue line.'],
    [g => { g.fillStyle = '#0e1219'; g.fillRect(14, 4, 9, 10); g.fillStyle = '#ffffff'; g.fillRect(15, 5, 7, 2); g.fillRect(15, 8, 1, 4); g.fillRect(18, 8, 1, 5); g.fillRect(21, 8, 1, 4); },
     'white fall glyph', 'A WATERFALL: a step where the surface drops more than 2 m (the selected river labels the drop).'],
    [g => { line(g, '#7cc0ff', 1, null, [[2, 9.5], [SW - 2, 9.5]]); g.fillStyle = '#ffffff'; g.fillRect(3, 6, 7, 7); g.fillStyle = '#5ad87a'; g.fillRect(4, 7, 5, 5); g.fillStyle = '#ffffff'; g.fillRect(SW - 10, 6, 7, 7); g.fillStyle = '#2f7fe0'; g.fillRect(SW - 9, 7, 5, 5); },
     'green / blue end squares', 'A river’s knots (River tool or selected): GREEN = the source, BLUE = the mouth. A diamond beside a knot of the selected river drags its width; a yellow square at the mouth = it joins another river there.'],
    [g => { fill(g, '#3c7fd8', 0.5, 4, 3, SW - 8, SH - 6); rect(g, '#9fd0ff', 1, null, 4, 3, SW - 9, SH - 7); },
     'blue polygon', 'A LAKE: water up to its level (the label, m above sea) over every column inside whose ground is lower.'],
    [g => { fill(g, '#3c7fd8', 0.5, 4, 3, 18, SH - 6); for (const x of [24, 28, 32]) { g.fillStyle = '#ff5a3a'; g.fillRect(x - 1, 8, 3, 3); } },
     'red squares by a lake', 'SILL: shore-band ground outside the selected lake that is below its level + 1. The engine raises it to hold the water; move the outline or lower the level if that wall is unwanted.'],
    [g => { line(g, '#9fd8ff', 1, null, [[2, 4], [14, 9]]); line(g, '#5ab4ff', 2, null, [[2, 15], [14, 9]]); line(g, '#2f86e8', 3, null, [[14, 9], [SW - 2, 10]]); },
     'derived network', 'The River tool’s DERIVE preview (Water panel): channels the terrain drains into, coloured and thickened by order (pale = a headwater, deep blue = a trunk); picked ones are white. Accept writes them into rivers[].'],
    [g => line(g, '#ff6a5a', 1.5, [4, 3], [[2, 13], [16, 5], [SW - 2, 9]]), 'red dashed line', 'A river the engine would REFUSE (fewer than two knots, no width / depth at an end): fix it in the Water panel or delete it before saving.'],
    ['Sites (map.json sites[])', null],
    [g => { fill(g, '#7ec8ff', 0.10); rect(g, '#7ec8ff', 2); rect(g, '#7ec8ff', 1, [4, 3], 1, 0, SW - 3, SH - 1); },
     'sky-blue box', 'SOFTENED ground: inside, the biome’s hills, bumps and grain are scaled down (the % in the label) so buildings sit on calm ground. Dashed = the feather where it blends back. Harrowby’s is “harrowby_ground”, the same box as the village’s bounds.'],
    [g => { fill(g, '#c6f06a', 0.12); rect(g, '#c6f06a', 2); rect(g, '#c6f06a', 1, [4, 3], 1, 0, SW - 3, SH - 1); },
     'lime box', 'A CLEARING: no trees grow inside it; dashed = the feather where the forest thins back in.'],
    [g => rect(g, '#ffb454', 2), 'orange box', 'A PAD: flat bare ground (the test harness stands on one).'],
    [g => line(g, '#8dff9a', 2, null, [[18, 3], [24, 9], [18, 15], [12, 9], [18, 3]]),
     'green diamond', 'Where a new game starts (the spawn). Dim = the map names none and the default is used.'],
    [g => { g.beginPath(); g.arc(18, 9, 5, 0, Math.PI * 2); g.strokeStyle = '#5fc8ff'; g.lineWidth = 2; g.stroke(); g.setLineDash([3, 3]); g.lineWidth = 1; g.beginPath(); g.arc(18, 9, 8, 0, Math.PI * 2); g.stroke(); g.setLineDash([]); },
     'blue circle', 'An authored LAKE (preset and radius in the label); dashed ring = its shore band.'],
    [g => line(g, '#6fe07a', 2, null, [[18, 2], [23, 13], [13, 13], [18, 2]]), 'green triangle', 'One hand-placed TREE (species in the label).'],
    [g => rect(g, '#7fd4ff', 2, null, 11, 3, 14, 12), 'cyan square', 'A STAMP: a prefab dropped at that spot.'],
    ['Height lines', null],
    [g => { line(g, '#ff9f4a', 2, null, [[2, 13], [16, 5], [SW - 2, 9]]); }, 'orange line', 'A RIDGE line; blue = a VALLEY line. Dashed = drawn but not yet baked into the height plane. Editor-only: the game reads only the baked heights.'],
    [g => { line(g, 'rgba(232,160,96,0.55)', 1, [2, 4], [[2, 4], [SW - 2, 3]]); line(g, 'rgba(232,160,96,0.55)', 1, [2, 4], [[2, 15], [SW - 2, 14]]); line(g, '#e8a060', 2, [6, 3], [[2, 10], [SW - 2, 8]]);
           g.fillStyle = '#e8a060'; g.beginPath(); g.moveTo(18, 4); g.lineTo(23, 13); g.lineTo(13, 13); g.closePath(); g.fill(); },
     'amber crest + triangles', 'A MOUNTAIN RANGE (the Range tool, map.json ranges[]): the crest spline, a triangle per knot, the dotted lines its feet (the half-width either side). Dashed crest = not baked yet; solid = the plane holds it. The selected range shows each knot’s crest height and a diamond to drag its half-width.'],
    [g => { fill(g, '#9a8f84', 0.85, 2, 2, SW - 4, SH - 4); rect(g, 'rgba(255,216,102,0.7)', 1, [3, 3], 1, 1, SW - 3, SH - 3); },
     'dashed yellow box over terrain', 'A PREVIEW (a range, a template, a landform): a coarse run drawn over the map, NOT written yet. Bake / Generate / Apply makes it real.'],
    [g => { fill(g, '#ff5ad0', 1, 6, 8, 5, 3); line(g, '#ff5ad0', 1, null, [[6, 9.5], [SW - 6, 9.5]]); g.fillStyle = '#ff5ad0'; g.fillRect(4, 7, 5, 5); g.fillRect(SW - 9, 7, 5, 5); },
     'pink route', 'The MEASURE tool’s route: distances at each point; its profile and grades are under the toolbar.'],
    ['Overlays (the display bar)', null],
    [g => { fill(g, 'rgb(92,160,80)', 1, 0, 0, 12, SH); fill(g, 'rgb(214,180,72)', 1, 12, 0, 12, SH); fill(g, 'rgb(200,80,60)', 1, 24, 0, 12, SH); },
     'slope bands', 'The ground coloured by slope: GREEN walkable (under ~35°, where soil and scree hold), YELLOW steep (still climbable: the player steps 0.58 m per 0.1 m), RED cliff (at or past the map’s rockSlope, 68° shipped: bare rock, no soil, snow or plants). Measured post to post on the 6.4 m plane.'],
    [g => { fill(g, '#5a7a4a', 1, 0, 0, SW, SH); line(g, 'rgb(60,170,255)', 2, null, [[2, 3], [12, 9], [20, 10], [SW - 2, 16]]); line(g, 'rgba(100,200,255,0.8)', 1, null, [[14, 2], [17, 9]]); },
     'flow', 'Where rivers would run: drainage area over a ~51 m copy of the plane (water routed downhill across filled hollows). Brighter and thicker = more water. Recomputed a moment after you edit.'],
    [g => { fill(g, '#5a7a4a', 1, 0, 0, SW, SH); fill(g, 'rgb(30,90,230)', 0.8, 9, 4, 18, 10); },
     'lakes', 'Where water would stand: every closed hollow filled to its spill point (Priority-Flood), deeper = darker. A hint: click one with the Lake tool to make it a real lake (lakes[]).'],
    [g => { fill(g, '#5a7a4a', 1, 0, 0, SW, SH); line(g, 'rgba(255,255,255,0.4)', 1, null, [[12.5, 0], [12.5, SH]]); line(g, 'rgba(255,255,255,0.4)', 1, null, [[0, 9.5], [SW, 9.5]]); },
     'km grid', 'A kilometre grid (1 / 2 / 5 / 10 km by zoom), labelled at the edges.'],
    [g => { fill(g, '#3a5a7a', 1, 4, 2, 28, 14); rect(g, '#ffd866', 1, null, 14, 5, 8, 6); },
     'minimap', 'The whole map in the canvas corner with the view’s box; click it to jump there.'],
    ['Climate (the Climate panel’s overlay picker)', null],
    [g => grad(g, [[40, 60, 200], [120, 180, 240], [240, 240, 240], [250, 200, 80], [210, 50, 30]]),
     'temperature', 'Mean annual air temperature, −20 °C (blue) through 5 °C (white) to 30 °C (red): sea temperature + latitude − lapse with height − poleward slopes, milder by the coast.'],
    [g => grad(g, [[150, 100, 50], [220, 200, 90], [90, 180, 70], [30, 120, 200], [20, 40, 160]]),
     'precipitation / moisture', 'Rain a year, 0 (brown) to 3,000 mm (deep blue) — the windward side of a range wet, its lee dry (the rain shadow). MOISTURE is the same ramp on the biome files’ 0..1 scale (rain / 3,000 mm plus rivers, lakes and waterlogged flats).'],
    [g => { fill(g, '#14283c', 1, 0, 0, SW, SH); line(g, 'rgb(120,230,255)', 3, null, [[2, 12], [14, 8], [SW - 2, 6]]); },
     'wetness', 'Where water gathers: flow accumulation and the nearness of map.json rivers and lakes (cyan = wet).'],
    [g => { fill(g, '#2f7a3a', 1, 0, 0, 12, SH); fill(g, '#b7b37a', 1, 12, 0, 12, SH); fill(g, '#1c3b24', 1, 24, 0, 12, SH); rect(g, 'rgba(255,216,102,0.85)', 1, [3, 3], 1, 1, SW - 3, SH - 3); },
     'predicted biomes', 'The biome the generator would give every cell (biome colours). Inside a dashed yellow box it is a Generate PREVIEW: Commit writes it, Discard drops it.'],
    [g => { fill(g, '#5a7a4a', 1, 0, 0, SW, SH); for (let x = -SH; x < SW; x += 4) line(g, 'rgba(255,255,255,0.7)', 1, null, [[x, SH], [x + SH, 0]]); },
     'white hatching', 'LOCKED biome cells (painted by hand with the Biome brush, or with the Lock tool): Generate keeps them. Shown while the Biome or Lock tool is active.'],
    [g => { fill(g, '#2b5d9e', 1, 0, 0, 14, SH); fill(g, '#d9c27a', 1, 14, 0, 4, SH); fill(g, '#5a7a4a', 1, 18, 0, SW - 18, SH); g.beginPath(); g.arc(16, 9, 7, 0, Math.PI * 2); g.strokeStyle = 'rgba(120,220,255,0.9)'; g.lineWidth = 1; g.stroke(); },
     'cyan circle', 'The COAST brush (wobble / beach / cliff / smooth): it moves only the ground near sea level.'],
    ['Highlights', null],
    [g => rect(g, '#ffffff', 2), 'white outline', 'The selected thing.'],
    [g => rect(g, '#ff7a7a', 2), 'red outline', 'Things of the kind the active tool places or edits.'],
  ];
  const LOCALE = [
    ['Inside a locale', null],
    [g => { fill(g, '#000', 0.62, 0, 0, SW, SH); fill(g, '#5a7a4a', 1, 8, 3, 20, 12); rect(g, '#7fd4ff', 2, null, 8, 3, 19, 11); },
     'lit square', 'The open locale’s bounds; everything outside is dimmed. The small cyan cross is its anchor — every ref’s height is stored relative to it.'],
    [g => { fill(g, '#ffd866', 0.22, 8, 3, 20, 12); rect(g, '#ffd866', 1, null, 8, 3, 19, 11); line(g, '#ff5a4a', 2, null, [[8, 15], [28, 15]]); },
     'yellow block, red edge', 'A BUILDING (a structure blueprint placed here). The red edge is its front, the door side. Label = name (blueprint).'],
    [g => { line(g, 'rgba(141,255,154,0.85)', 1, null, [[6, 13.5], [18, 5.5], [30, 11.5]]); for (const [x, y] of [[6, 13], [18, 5], [30, 11]]) { g.fillStyle = '#8dff9a'; g.fillRect(x - 3, y - 3, 7, 7); } },
     'bright green squares + lines', 'The village WALKING GRAPH: waynodes (squares) and the links between them (lines). Villagers do not walk the roads — they walk these lines, taking the shortest chain of links between home, work and bed. A link is two-way. Links also shape the ground: no tree grows within 1.5 m of one, and inside a Soften box the ground along it is flattened further (the “paths” %).'],
    [g => { line(g, 'rgba(141,255,154,0.35)', 1, null, [[6, 12.5], [18, 6.5], [30, 12.5]]); for (const [x, y] of [[6, 12], [18, 6], [30, 12]]) { g.fillStyle = '#5f9f6a'; g.fillRect(x - 2, y - 2, 5, 5); } },
     'dim green squares + lines', 'The same graph INSIDE each building: its rooms and its door’s inside / outside nodes, from the blueprint. A bright line to a dim square is where the village joins a house at its door.'],
    [g => { g.fillStyle = '#e8dcc0'; g.beginPath(); g.arc(18, 9, 3, 0, Math.PI * 2); g.fill(); },
     'cream dot', 'Any other ref: a villager (where they stand at load), a marker, a readable, a door. Label = its id. Edit these in game (F8 / F1 → References).'],
    [g => line(g, '#ffd866', 1, [3, 3], [[4, 9.5], [SW - 4, 9.5]]), 'yellow dashed line', 'The link being drawn with the Link tool.'],
    [null, 'also shown', 'Roads, regions, markers and the sites above paint here too: in Harrowby the brown and grey bands are its gravel and earth footpaths, the sky-blue box is its softened ground.'],
  ];

  const rows = list => list.map(([draw, name, what]) => what == null
    ? el('div', {class: 'mapsub'}, draw)
    : el('div', {class: 'maplegend-row'}, draw ? swatch(el, draw) : el('span', {class: 'maplegend-sw'}),
         el('div', {}, el('b', {}, name), el('div', {class: 'maplegend-what'}, what))));
  const worldBox = el('div', {}, ...rows(WORLD));
  const localeBox = el('div', {}, ...rows(LOCALE));
  const det = el('details', {class: 'maplegend', open: true});
  det.append(el('summary', {}, 'Legend — what the colours, lines and badges mean'), localeBox, worldBox);
  return {
    panel: det,
    /** In a locale the locale's own section leads; the whole-map rows stay (they paint there too). */
    sync(inLocale) { localeBox.style.display = inLocale ? '' : 'none'; },
    css: `
#env-map .maplegend-row{display:flex;gap:8px;align-items:flex-start;margin:4px 0}
#env-map .maplegend-sw{flex:0 0 ${SW}px;width:${SW}px;height:${SH}px;image-rendering:pixelated;border:1px solid #2a3040;border-radius:2px}
#env-map .maplegend-what{color:#8fa0b8;font-size:10px;line-height:1.35}
`,
  };
}

/**
 * THE TOOL GUIDE: a box under the toolbar that explains the ACTIVE tool in
 * full -- what it changes in the world, how to drive it, and its limits --
 * instead of a hover tooltip. Numbers are the loader's (src/sim/worldmap.cpp,
 * worldmap.h kSoften* / kClearing* / kRoute*, ROAD_KINDS in mapvector.js);
 * 10 voxels = 1 m. Each entry: [title, what it does, how to use it, limits].
 */
export const TOOL_GUIDE = {
  site: ['Select',
    'Picks an existing site (spawn, pad, stamp, lake, tree, clearing, soften) so the Sites panel on the right shows its settings.',
    'Click a site’s outline or marker. Edit its numbers in the Sites panel.',
    ''],
  height: ['Height brush — the authored height plane',
    'Paints the map’s actual shape, in metres. One height sample (post) every 6.4 m, blended smoothly between posts. The seeded noise and the sculpt layer ride on top of it.',
    'Left-drag. raise / lower move ground up or down; smooth pulls toward the local average; flatten levels to where the stroke started; set moves toward a typed height above sea (Alt+click picks one); terrace cuts shelves and cliffs; noise adds seeded relief; erode runs water + scree erosion under the brush. [ ] change the radius (1 m to 5 km). MASK limits every mode: land only, sea only, a height band (m above sea) or a slope band (degrees), soft at the edges — paint snow-line rock without touching the valleys, raise the sea floor without touching the coast.',
    'Coarse (6.4 m) — for finer work use Sculpt. A locale (e.g. Harrowby) rides its ground: raise the ground under it and the whole village moves up with it.'],
  shape: ['Shape — stamp a landform',
    'Writes a whole landform into the height plane in one click: peak, ridge, basin, plateau, volcano, mesa.',
    'Pick the kind, height (a rise above the ground, or an absolute top / floor), radius, aspect and rotation in the toolbar; click to stamp. [ ] radius, , . rotate.',
    'It writes posts like the Height brush (Ctrl+Z undoes); nothing remembers it was a shape afterwards.'],
  line: ['Line — ridge and valley lines',
    'Draw a ridge (orange) or valley (blue) along a path, then Bake it into the height plane from the Height tools panel.',
    'Click to add vertices, double-click / Enter ends. Drag a vertex to move it, click a segment to insert one, Shift+click a vertex to delete it.',
    'The game never reads the line — only what you baked. A dashed line is not baked yet.'],
  area: ['Area — a rectangle for erosion, generators and PNG',
    'Marks a rectangle: the Height tools panel erodes it (stream power or droplets) or exports / imports it as a PNG heightmap; the Generate panel generates a template into it or places a landform by it (a canyon runs along its long side, a crater sits at its centre).',
    'Drag a rectangle; Shift+click clears it. Erode / Generate / Apply put the result in the plane as a preview you then Commit or Discard. With no Area, stream-power erosion and templates work on the WHOLE map.',
    'At least 8 posts (51 m) a side. Droplet erosion takes at most ~2048 × 2048 posts.'],
  biome: ['Biome brush',
    'Paints which biome (forest, meadow, desert, …) owns the ground — that decides the ground skin, trees, plants, random ponds and the local relief.',
    'Pick a biome from the palette, left-drag. Alt+click picks the biome under the cursor. While “lock painted cells” is on (Climate panel), every cell you paint is LOCKED (white hatching): the Climate panel’s Generate biomes keeps it; undo takes the paint and the locks back together.',
    'Paints whole 25.6 m cells. For a whole continent, let the Climate panel generate the biomes and paint only what must differ.'],
  sculpt: ['Sculpt — the fine height layer',
    'A separate layer of height offsets every 0.8 m, added after everything else (authored height + noise + sites). Orange on the map = raised, blue = lowered.',
    'Left-drag. raise / lower add or remove height; smooth pulls toward the local average of the real ground; flatten levels to the ground where the stroke started (needs the real terrain heights loaded); erase returns toward the generated ground.',
    'For banks, ditches, mounds — detail the 6.4 m height plane cannot hold.'],
  pad: ['Pad box',
    'A box of calm, bare, flat ground. The test harness’s map stands its fixtures on one.',
    'Drag the box.',
    'One per map (a second is ignored with a warning). Game maps rarely need it — use Soften for a village.'],
  spawn: ['Spawn',
    'Where a new game starts; the calm “home area” of the terrain is centred on it.',
    'Click to put it there.',
    'One per map (a second one refuses to load). Must be on land, off any lake and outside every site’s footprint — the load check warns, and the default map’s gate refuses. Inside a clearing is allowed but warned (you wake in the open).'],
  stamp: ['Stamp site — place a prefab',
    'Drops an authored .vox (assets/prefabs/) at a spot, the same on every seed, standing on a flat pad of ground made for it.',
    'Pick the template and rotation (90° steps) in the toolbar, click to place; Shift+click a stamp deletes it.',
    'The pad is flat over the model + an apron (0–3.2 m) and ramps back to the terrain over the pad margin (0.1–6.4 m). No tree grows on the pad or within 1 m of the model. A missing .vox skips the site with a warning. Two stamps whose ramps would tilt each other’s floor are warned.'],
  water: ['Water — an authored lake',
    'Places a body of water from a preset (Environment → Water bodies: tarn, kettle, marsh, oasis, playa, crater lake, lava pool…), the same spot and shape on every seed. The preset gives the bowl’s depth profile, the berm, the bed materials and the shore plants.',
    'Pick the preset (and optionally a radius in m) in the toolbar, click to place; drag to move; Shift+click deletes. Solid circle = the water’s edge, dashed ring = the shore band (berm, mud, reeds).',
    'THE WATER LEVEL is the natural ground height at the lake’s CENTRE. The whole disc is dug into the preset’s bowl below that level, and a berm is raised round the rim (up to surface + berm height) so it holds water. So on a slope you get a cut bank uphill and an embankment downhill: authored lakes skip the height and slope checks the biome’s random ponds must pass, so nothing stops a lake on a hillside — check it in the preview. ' +
    'The preset must exist (an unknown one refuses to load); radius 0.4–204.8 m; the footprint is the disc + shore band — overlapping another lake, stamp or tree is warned and the earlier site in the list wins; the spawn must be outside it. Random ponds keep off it.'],
  tree: ['Tree — one authored tree',
    'Plants one tree of a chosen species (assets/trees/) on the ground at that spot, the same on every seed.',
    'Pick species, variant and rotation (or leave them rolled), click; drag to move; Shift+click deletes.',
    'Does not grow at or above that spot’s treeline (warned). 0.6 m trunk keep-out; clearings never remove authored trees. An unknown species is skipped with a warning.'],
  clearing: ['Clearing — keep the forest off',
    'A box no procedural tree’s crown reaches over. Past the box, across the feather band, trees are thinned at random (the same every seed) so the edge is ragged, not ruled.',
    'Drag a box; click one to select, drag to move; Shift+click inside one deletes it.',
    'That is ALL it does: terrain, grass and plants, and ponds are untouched, and authored trees stay. Feather defaults to 6.4 m, max 51.2 m. Buildings already clear their own trees, so a village does not need one.'],
  soften: ['Soften — calm the ground without flattening it',
    'Inside the box the biome’s three small-scale reliefs are turned down: hills (the ~13 m swells, default 40 %), bumps (~3 m detail, 20 %), grain (sub-metre lumps, 0 %) — each a percent of the biome’s own. The big landform and the slope it sits on are NOT touched, so a village keeps its place on a hillside but gets walkable ground. Across the feather (default 9.6 m) it eases back to 100 %.',
    'Drag a box; set hills / bumps / grain / paths / feather in the Sites panel. Shift+click inside one deletes it.',
    '“paths” is the hill scale ON the villagers’ walking routes (the green waynode links): fully applied within 1.2 m of a link, fading out by 3.6 m, so the tracks are flatter than the green around them (default 10 %). Values 0–100 % (more relief is what Sculpt / Height are for); feather max 51.2 m. It never flattens to a plateau — for that use the Height brush (set / flatten) or Sculpt. Harrowby’s is “harrowby_ground”.'],
  road: ['Road — graded roads, tracks and paths',
    'A road the game cuts and fills into the ground. Its height profile is baked from the ground under it: smoothed, bumps cut and dips filled equally, held under the max grade. The shoulder blends it back to the terrain.',
    'Click to draw, double-click / Enter ends; drag a vertex; click the selected road to insert one; Shift+click a vertex deletes it. Vertices snap onto other roads to make junctions; Ctrl+click on a road branches from it; Alt = no snap. Kind, surface and numbers in the Vector layers panel, which also shows the baked profile once saved.',
    'Defaults: road 4 m wide + 1.6 m shoulder, max 10 % grade; track 2.4 m + 1.2 m, 15 %; path 1.2 m + 0.6 m, 25 %. Width 0.2–25.6 m, shoulder 0–6.4 m (widened automatically so a cut never steps more than a voxel), grade 1–100 %, depth 1–8 voxels. The surface must be a solid (a powder would slide off the grade). No trees, plants or random ponds on it. Villagers do not follow roads — they walk the waynode links.'],
  region: ['Region — a named area',
    'Outlines a named polygon (a forest’s name, a district) in its own colour.',
    'Click out the polygon, click the first vertex to close; vertices edit like roads.',
    'Annotation only: the game builds nothing from it.'],
  marker: ['Marker — a pin with an icon',
    'A labelled point of interest with a pixel-art icon (hut, village, castle, tower, cave, ruin, shrine, camp, mine, port).',
    'Pick the icon in the Vector layers panel, click to place, drag to move, Shift+click deletes.',
    'Annotation only.'],
  locale: ['Locale — a hand-built place',
    'A locale (like Harrowby) is a box on the map that owns its buildings, villagers, waynodes, markers and its own voxel edit layer. Everything in it is stored relative to its ANCHOR height, so reshaping the ground under it moves the whole place up or down rigidly.',
    'Drag out the bounds (with the icon picked beside the tool); click to select; DOUBLE-CLICK to open it — the page then shows only its own tools (Structure, Link, sculpt and sites inside it) and a 3D view.',
    'There is a minimum size (the toast says). Its files: refs/<id>.json and locales/<id>/ground.svedit.'],
  structure: ['Structure — place a building (inside a locale)',
    'Places a structure blueprint (Environment → Structures) in the open locale. The game levels a flat pad for it and keeps trees off it.',
    'Pick the blueprint, click to place; drag to move; R turns it 90° (Shift+R the other way); Delete removes the selected one. The red edge is its front (door) side.',
    'Written through the same command layer as the in-game F8 editor, so undo works and the game sees the same file.'],
  waylink: ['Link — the villagers’ walking graph',
    'Waynodes (green squares) and the links between them (green lines) are where villagers walk: an NPC goes to the nearest waynode, follows the shortest chain of links to the one nearest its goal (home, work, bed), then walks there. Each building’s blueprint brings its own nodes (dim) for its rooms and door; you link the village nodes (bright) to a house’s outside-door node.',
    'Click a waynode, then another, to join them (again, or with Shift, to part them). Click open ground to make a new waynode joined to the last one.',
    'Links are two-way. They also shape the world: no tree trunk grows within 1.5 m of a link, and inside a Soften box the ground along a link is flattened by the “paths” percent. A link through a door makes the NPC open it. If an NPC cannot walk a link it drops it for itself and re-plans.'],
};

/** The box under the toolbar; `sync(tool)` shows that tool's entry. `extra`
 *  = the entries tools carry themselves (registerTool's `guide`), which win. */
export function buildToolGuide(el, extra = {}) {
  const GUIDE = {...TOOL_GUIDE, ...extra};
  const box = el('div', {class: 'maptoolguide'});
  let cur = null;
  return {
    panel: box,
    css: `
#env-map .maptoolguide{font:11px/1.45 monospace;color:#c7d2e3;background:#12161f;border:1px solid #2a3040;border-radius:6px;padding:5px 9px;margin:2px 0}
#env-map .maptoolguide b{color:#ffd866}
#env-map .maptoolguide .tg-k{color:#8fa0b8;text-transform:uppercase;font-size:9px;letter-spacing:.06em;margin-right:6px}
`,
    sync(tool) {
      if (tool === cur) return;
      cur = tool;
      const g = GUIDE[tool];
      box.style.display = g ? '' : 'none';
      if (!g) return;
      const [title, what, how, limits] = g;
      const row = (k, t) => (t ? el('div', {}, el('span', {class: 'tg-k'}, k), t) : null);
      box.replaceChildren(...[el('b', {}, title), row('what', what), row('how', how), row('limits', limits)].filter(Boolean));
    },
  };
}
