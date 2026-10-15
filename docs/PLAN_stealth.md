# Stealth and detection

2026-10-15. The architecture summary is DESIGN.md, "Stealth and detection". This file holds the research the model was calibrated from, the numbers it ships with, and what is still open.

## 1. What the owner asked for

- An advanced stealth and detection system.
- Every creature has its own sneak skill and its own detection skill.
- Crouching hides you better, and crawling hides you more still.
- Light versus shadow matters a great deal.
- Sound is a second way to be detected.
- Once a creature is in combat it is much harder to lose. It goes to where it last saw you, and the threshold for being unseen rises.
- Sneak attacks deal bonus damage, the way charged attacks do.
- The Elder Scrolls is the main inspiration, with ideas from Splinter Cell: Chaos Theory.

## 2. The research the numbers came from

### Skyrim and Oblivion

Sources: UESP Skyrim:Sneak, its talk archive of `fSneak*` settings, the Creation Kit detection pages, and Ordinator and RAID mod documentation.

**The detection formula.** `Detection = base(-15) + A·(Sound + Visual + DetSkill) + (DetSkill − TgtSkill)`, where `A = (1 − d/maxDist)²`. Max distance is about 36 m indoors and 75 m outdoors.

- **Sight** = visual contact × light term × movement term. The light term is `(15 + light) × 0.33`, so even pitch black leaves about 13% of the bright value. In vanilla, movement barely affects sight (×1.01). Running adds ×1.2.
- **Sound** = movement noise plus action noise. Movement noise is `(12 + 0.5 × armour weight) × (2 if running)`. Without line of sight, sound is ×0.3. The sneak skill reduces only the skill term.

**Pool and states.** Detection drains a "stealth points" pool over time, so it builds rather than flipping on.

- The NPC states run Normal → Alert → Combat → Lost → Normal.
- A lost NPC searches for about 10 s (`fCombatDetectionLostTimeLimit`).
- An NPC that was alerted before re-detects more easily.

**Sneak attacks.**

- One-handed weapons ×3, daggers ×3 (×15 with perks), bows ×2, two-handed and unarmed ×2.
- The condition is that the target has not detected you, and in Skyrim you must be in sneak mode.
- Oblivion's version: ×4 for one-handed and hand-to-hand, ×2 for marksman.

### Splinter Cell: Chaos Theory

- **Light meter:** shows the light on the player's body.
- **Sound meter:** has an ambient marker. You cannot be heard while your noise stays under it.
- **Ambient masking:** generators, storms and rain cover your noise.
- **Guards:** they go and look at noises, search your last known position, and do not reset after contact.
- **Alarm stages:** each alarm ratchets guard readiness up.

### Other games

- **Blacklist:** raycasts to body parts, so partial cover counts.
- **MGSV:** a directional indicator for each enemy.
- **Hitman:** a fill per NPC. At 50% they look, at 90% they walk to you, at 100% you are compromised.
- **Thief:** a light gem. Surfaces change how loud you are.

## 3. The model (game/stealth.h)

- Each creature keeps a meter from 0 to 1 for each body it tracks.
  - Sight fills it as `sightRate × cone × (1 − d/reach)²`.
  - Sound fills it as `soundRate × (1 − d·(1+ambient)/radius)`, capped at `soundCap` unless the creature is fighting that body.
  - It drains at `decay` every tick.
- `reach = sightRange × (0.6 + 0.8·awareness/100) × visibility`.
- `visibility = light' × stance × motion × size × (1 − 0.6·sneak/100)`, where `light' = 0.12 + 0.88·light^0.75`.
- The thresholds: look at 0.3, walk over at 0.6, detected at 1.0.

### Worked numbers

These use the defaults: a `guardian` (sight range 260, awareness 65) and the player at sneak 30.

| Situation | Where it gets noticed, roughly |
|---|---|
| Daylight, standing, walking | within about 17 m |
| Daylight, crouched, creeping | about 8 m |
| Shade under a roof, crouched, creeping | about 3.5 m |
| Night under open sky, standing, walking | about 6 m |
| Night, crouched, creeping | about 2.5 m |
| Prone and still at night | only at arm's length |

| Footsteps (heard at awareness 50, in silence) | Distance |
|---|---|
| Walking | about 6 m |
| Creeping crouched | under 1 m |
| Sprinting | about 14 m |
| Sprinting in a full iron set | about 25 m |
| Any of the above in heavy rain | about 55% as far |

## 4. What ships

**Who takes part.** Stealth is opt-in, in both directions. Only creatures whose profile authors a `stealth` block, plus the player on foot, can be hidden or use the meter. Legacy profiles behave exactly as before (fixtures, `duelist`, dummies). For stealth creatures, the rule facts `enemies` and `hostiles` count only strangers they have noticed.

**NPC sneaking is a mode.** It starts with `stalk` and holds a crouch and `stalkSpeed` through every verb, including the swing, until the target turns. A stroke begun crouched is finished crouched.

**Profiles with a `stealth` block:**

- villager, zombie, guardian;
- swordsman, fencer, brawler, berserker, battlemage;
- archer, snake, imp, fiend;
- stalker (new: an NPC that sneaks, uses `Intent::Stalk` and lands sneak attacks).

**Intents and facts.**

- New intents: `investigate`, `search` and `stalk`.
- New facts: `awareness` (0–1) and `alert` (0–3).

**Skills per character.** A mob sidecar can carry `"stealth": { "sneak": N, "awareness": N }`, and it is inherited through `extends`. The player's sneak skill comes from their body's sidecar, or `stealth.playerSneak` (30) if it has none.

**Sneak attacks.**

- Melee ×3 (`stealth.sneakMelee`), arrows ×2 (`stealth.sneakRanged`), the dagger ×5 (`items.json` `sneakAttack`).
- A crouch is required (`stealth.sneakNeedsCrouch`).

**HUD.**

- An eye above the crosshair, with a pip for each watcher.
- Two meters under it: light, and noise with a quiet mark.
- A "SNEAK ATTACK x3.0" toast.

**Noise events.**

- A blow landing, a cry of pain, a landing, a bow loosed, an arrow striking stone, an alarm.
- Footsteps are continuous, published on each actor.

## 5. Decisions for the owner

Each of these is one tuning or JSON edit.

1. **Sight ranges are longer than they were.** Profiles were tuned at 3–6 m (`perception.sightRange` 30–60). The stealth reach is 15–32 m in full daylight (`stealth.sightRange` 150–320), because hiding in shadow means nothing if nobody can see across a room anyway.
   - Zombies and guards now notice a lit, walking player from much further away than before.
   - The old `perception.sightRange` still sets the Allies/Enemies counts that rules read.
2. **Sneak attacks need a crouch.** This is Skyrim's rule, `stealth.sneakNeedsCrouch`. Turn it off to let an unaware creature take the bonus from any blow.
3. **The dagger lands ×5.** That is above Skyrim's unperked ×3, because there is no perk tree here. Each weapon authors its own `sneakAttack`.
4. **`duelist` is left on the legacy perception.** It is the fixture for about 30 gates. `skittish` (deer) is also left alone: its flee rule still reads `enemies` rather than `awareness`.
   - Making the deer stealth-aware would be one rule edit: `"when": { "awareness": ">=0.3" }`.
5. **Neutral villagers notice you too.** Their meter fills, they come to look at noises, and a villager who has seen you is not sneak-attackable. They still never pick a fight unprovoked.

## 6. Gates

- **`stealth`** (pure arbiter arms; 12 arms, A–L).
- **`stealth-world`** (THE tick): a stalker sneaks up on a villager and lands a ×3 sneak attack, and the villager turns on it the next tick.
- **Inherited reds on main.** `snake`, `impact-blunt`, `impact-fist` and `bite-infect` fail on main's own exe with identical failure lines (the control was run 2026-10-15). They are not caused by this work.
- **`mob-cap64`** (64-creature brawl). Its brick-pool check peaks within 0.5% of `kMaxMicroBodyPoolWords` on every run. With the zombies in its mix now noticing, investigating and alarming, the brawl unfolds differently and peaked 0.02% over the cap (36 refusals).
  - This is a capacity margin, not a stealth defect.
  - Owner call: raise the pool, or take the zombies out of that mix's stealth.

## 7. Follow-ups, not done

- **Corpses as an alarm** (Blacklist, Chaos Theory): a guard finding a dead ally becomes alert.
- **Alarm escalation per area** (Chaos Theory's stages).
- **Barks** ("What was that?" / "Must have been the wind"). The dialogue system exists, but these need voice and text assets.
- **NPC footsteps.** They are not yet audible to the player, because nobody drains the NPC footfall queue. The detection side already uses NPC noise.
- **Light occlusion.** Glow is not occluded: a fire behind a wall still lights you. A ray to the brightest chunk would fix it.
- **Light near the player.** It comes from what the chunk caches hold, which the mirror seeds around the player. A glowing cell in a chunk nobody fetched contributes nothing.
