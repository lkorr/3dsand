/* props/index.js — the registry of generated dungeon props, by id '<kit>/<name>'.
 * Add a prop: write assets/dungeons/props/<kit>/<name>.js (see propkit.js) and
 * list it here. The tuner's prop palette and dungeongen.compile read PROPS. */
import ore_cart from './mines/ore_cart.js';
import rail_segment from './mines/rail_segment.js';
import bunk from './mines/bunk.js';
import crate from './mines/crate.js';
import barrel from './mines/barrel.js';
import lantern from './mines/lantern.js';
import pickaxe_rack from './mines/pickaxe_rack.js';
import table from './mines/table.js';
import ladder from './mines/ladder.js';
import altar from './ruins/altar.js';
import statue from './ruins/statue.js';
import sarcophagus from './ruins/sarcophagus.js';
import brazier from './ruins/brazier.js';
import candle_cluster from './ruins/candle_cluster.js';
import broken_column from './ruins/broken_column.js';
import bones from './ruins/bones.js';
import urn from './ruins/urn.js';
import banner from './ruins/banner.js';
import door from './ruins/door.js';
import glow_mushroom_patch from './caves/glow_mushroom_patch.js';
import bone_pile from './caves/bone_pile.js';
import crystal_cluster from './caves/crystal_cluster.js';
import campfire_remains from './caves/campfire_remains.js';
import rope_ladder from './caves/rope_ladder.js';

const LIST = [ore_cart, rail_segment, bunk, crate, barrel, lantern, pickaxe_rack, table, ladder,
              altar, statue, sarcophagus, brazier, candle_cluster, broken_column, bones, urn, banner, door,
              glow_mushroom_patch, bone_pile, crystal_cluster, campfire_remains, rope_ladder];

export const PROPS = {};
for (const p of LIST) PROPS[p.id] = p;
export const PROP_IDS = LIST.map(p => p.id);
export default PROPS;
