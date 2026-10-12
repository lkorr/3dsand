// loot.h — LOOT TABLES: what a container holds the first time it is opened
// (dungeons D3, docs/PLAN_dungeons.md §3.7).
//
//   assets/loot/<name>.json
//   {
//     "about": "What miners left behind.",
//     "rolls": [1, 3],                       // draws from `rows`: a number or [min, max]
//     "rows": [                               // ONE row per draw, by weight
//       { "item": "flask", "weight": 3, "count": 1 },
//       { "item": "dagger", "weight": 1 },
//       { "weight": 4 }                       // no item: the draw comes up empty
//     ],
//     "always": [                             // every roll, before the draws
//       { "item": "pouch", "chance": 0.5, "count": [1, 2], "dye": "#6b5a3e" }
//     ]
//   }
//
// A row's fields are the mob pack's (assets/mobs/human.json `loot`): `item`
// (an id in assets/items/items.json), `count` (a number or [min, max]),
// `chance` (0..1, `always` rows), `dye` ("#RRGGBB"); a `rows` entry adds
// `weight` (> 0, default 1). An item the library lacks is skipped at roll
// time with a warning; it never fails the table.
//
// DETERMINISM. A roll is a pure function of (table, seed): the container's
// seed is RefHash(ref id), so the same chest holds the same things on every
// machine, every replay and every new game -- and it is rolled ONCE, on the
// first open (refs_doors.cpp), after which the contents are the ref's delta
// like any chest's. Integer weights (x1000) and rng::Hash3 draws: no float
// in the pick.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Bag;
struct ItemLibrary;

namespace loot {

struct Row {
  std::string item;          // "" = nothing (a `rows` entry that draws empty)
  uint32_t weight = 1000;    // x1000
  int countMin = 1, countMax = 1;
  uint32_t chance = 1000;    // x1000 (always rows)
  uint32_t dye = 0;          // 0 = undyed
};

struct Table {
  std::string name;
  int rollsMin = 1, rollsMax = 1;
  std::vector<Row> rows;
  std::vector<Row> always;
  uint64_t TotalWeight() const;
};

// Parse one file's text; problems are "<label>: rows[2].weight: ..." and a bad
// row is skipped. False only for unreadable JSON.
bool Parse(const std::string& name, const std::string& text, const std::string& label,
           Table& out, std::vector<std::string>& problems);

// The process library, loaded on first use from <AssetDir>/loot (R reloads).
const Table* Find(const std::string& name);
void Reload();
const std::vector<std::string>& Problems();
// Install / replace one in memory (gates).
void Add(Table t);

// Roll `t` with `seed` into `out` (merging stacks). Items `lib` lacks are
// skipped and named in `skipped`. Returns the number of stacks added.
int Roll(const Table& t, uint32_t seed, const ItemLibrary* lib, Bag& out,
         std::vector<std::string>* skipped = nullptr);

}  // namespace loot
