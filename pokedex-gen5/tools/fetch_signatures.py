#!/usr/bin/env python3
"""Give every species the move it actually uses in Black / White, with its numbers.

For each of the 649 entries this reads the Gen 5 (Black 2 / White 2 first, then
Black / White) level-up list and keeps the last damaging move the species learns
that way -- the attack it would still be using at the end of the game. Nothing is
spread across the dex for variety: if forty species really end on Earthquake,
forty entries use Earthquake. A move the sketch cannot animate falls through to
the next real one, and a species whose whole list is status moves keeps its last
status move (Ditto and Unown have no usable attack at all and fall back to the
type pool).

The same pass records what the games know about that move and the species: its
type, base power and damage class, and the six base stats. From those it computes
the damage Gen 5's own formula gives at level 50 against a Ditto (base 48 in
every stat, 31 IVs, no EVs, neutral nature, no critical hit, no random roll),
including STAB and the type chart against Ditto's Normal type. That number is
what ``tools/make_signature.py`` scales into the firmware's 100,000 HP pool.

The result is vendored in ``art/signature-moves.json``; the firmware build never
needs the network. Set POKEAPI_CACHE to a directory to reuse responses.

Usage:  POKEAPI_CACHE=build/pokeapi-cache python3 tools/fetch_signatures.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import time
from pathlib import Path

API = "https://pokeapi.co/api/v2/{}/{}"
MOVE_INDEX = "https://pokeapi.co/api/v2/move?limit=1000"
OUT = Path("art/signature-moves.json")
SKETCH = Path("pokedex/pokedex.ino")
CACHE = Path(os.environ["POKEAPI_CACHE"]) if os.environ.get("POKEAPI_CACHE") else None

# Moves the sketch has effects for. Parsed from MOVE_LIST so the two cannot
# drift apart: name, family, type, two textures, count.
MOVE_RE = re.compile(r"X\((\w+),\s*(AF_\w+),\s*(T_\w+),")

GEN5_GROUPS = ("black-2-white-2", "black-white")

# Damage the games compute from weight, HP or speed, where the API reports no
# fixed power. The firmware needs one number, so these use the move's usual case.
NOMINAL_POWER = {
    "GYROBALL": 65, "HEAVYSLAM": 80, "ELECTROBALL": 60, "GRASSKNOT": 60,
    "CRUSHGRIP": 120, "WATERSPOUT": 150, "ERUPTION": 150, "PAYBACK": 50,
    "AVALANCHE": 60, "MIRRORCOAT": 60, "COUNTER": 60, "STOREDPOWER": 40,
    "BEATUP": 15, "REVENGE": 60, "WAKEUPSLAP": 60, "SNORE": 40, "FLAIL": 40,
    "RETURN": 102, "FRUSTRATION": 102, "PUNISHMENT": 60, "WRINGOUT": 60,
    "TRUMPCARD": 60, "MAGNITUDE": 70, "PRESENT": 40, "NATURALGIFT": 60,
    "JUDGMENT": 100, "TECHNOBLAST": 120, "SEEDFLARE": 120, "VCREATE": 180,
}

# A level-50 Ditto with 31 IVs and no EVs: (2 * 48 + 31) * 50 // 100 + 5.
DITTO_DEF = 68

# The ones that are part of a creature's identity. An override is only used when
# the species can actually learn the move by level-up in Gen 5, so an anachronism
# (Dazzling Gleam and Moonblast are Gen 6 moves) cannot slip into a Gen 5 dex.
OVERRIDES: dict[int, str] = {
    1: "VINEWHIP", 2: "RAZORLEAF", 3: "SOLARBEAM",
    4: "EMBER", 5: "FLAMETHROWER", 6: "FLAMETHROWER",
    7: "WATERGUN", 8: "BUBBLEBEAM", 9: "HYDROPUMP",
    11: "TACKLE", 14: "POISONJAB", 16: "GUST", 18: "WINGATTACK",
    25: "THUNDERBOLT", 26: "THUNDERBOLT", 39: "SING", 52: "FURYSWIPES",
    63: "PSYCHIC", 65: "PSYCHIC", 68: "CLOSECOMBAT",
    75: "ROCKSLIDE", 76: "ROCKSLIDE", 94: "SHADOWBALL", 95: "ROCKSLIDE",
    112: "EARTHQUAKE", 130: "HYPERBEAM", 131: "ICEBEAM", 134: "HYDROPUMP",
    132: "TACKLE", 143: "BODYSLAM", 144: "ICEBEAM", 145: "THUNDERBOLT",
    146: "FLAMETHROWER",
    149: "HYPERBEAM", 150: "PSYCHIC", 151: "PSYCHIC",    201: "PSYCHIC",
    202: "MIRRORCOAT", 225: "ICEBEAM", 235: "TACKLE", 248: "STONEEDGE",
    249: "AEROBLAST", 250: "SACREDFIRE", 254: "LEAFBLADE", 257: "FLAREBLITZ",
    260: "EARTHQUAKE", 282: "PSYCHIC", 302: "SHADOWCLAW", 306: "IRONHEAD",
    330: "DRAGONCLAW", 350: "HYDROPUMP", 359: "NIGHTSLASH", 360: "MIRRORCOAT",
    196: "FUTURESIGHT", 197: "FOULPLAY", 101: "SPARK", 113: "SING",
    173: "DAZZLINGGLEAM", 174: "DAZZLINGGLEAM", 440: "DOUBLEEDGE",
    242: "SING", 373: "DRAGONCLAW",
    376: "METEORMASH", 380: "DRAGONPULSE", 381: "DRAGONPULSE",
    382: "WATERSPOUT", 383: "EARTHQUAKE", 384: "DRAGONASCENT", 385: "MOONBLAST",
    386: "PSYCHIC", 445: "DRAGONCLAW", 448: "AURASPHERE", 460: "BLIZZARD",
    461: "NIGHTSLASH", 462: "FLASHCANNON", 463: "LICK", 464: "EARTHQUAKE",
    466: "THUNDERBOLT", 467: "FIREBLAST", 468: "DAZZLINGGLEAM", 471: "ICEBEAM",
    472: "XSCISSOR", 473: "AVALANCHE", 475: "SECRETSWORD", 476: "STONEEDGE",
    477: "SHADOWBALL", 478: "BLIZZARD", 479: "SHADOWBALL", 480: "FUTURESIGHT",
    481: "EXTRASENSORY", 482: "FUTURESIGHT", 483: "ROAROFTIME", 484: "SPACIALREND",
    485: "MAGMASTORM", 486: "CRUSHGRIP", 487: "SHADOWFORCE", 488: "MOONBLAST",
    489: "HYDROPUMP", 490: "HYDROPUMP", 491: "DARKVOID", 492: "SEEDFLARE",
    493: "JUDGMENT", 494: "VCREATE", 495: "VINEWHIP", 496: "LEAFBLADE",
    497: "LEAFSTORM", 498: "EMBER", 499: "FLAREBLITZ", 500: "FLAREBLITZ",
    501: "WATERGUN", 502: "WATERGUN", 503: "HYDROPUMP", 505: "CRUNCH",
    508: "CRUNCH", 509: "NIGHTSLASH", 510: "NIGHTSLASH", 511: "VINEWHIP",
    512: "LEAFBLADE", 513: "EMBER", 514: "FLAMETHROWER", 515: "WATERGUN",
    516: "HYDROPUMP", 517: "DREAMEATER", 518: "DREAMEATER", 519: "GUST",
    520: "AIRSLASH", 521: "AIRSLASH", 523: "WILDCHARGE", 526: "STONEEDGE",
    528: "AIRSLASH", 530: "EARTHQUAKE", 532: "SUPERPOWER", 534: "SUPERPOWER",
    537: "HYDROPUMP", 539: "CLOSECOMBAT", 542: "XSCISSOR", 545: "POISONJAB",
    549: "LEAFSTORM", 553: "EARTHQUAKE",    555: "FLAMEWHEEL", 558: "XSCISSOR",
    560: "CRUNCH",    561: "PSYBEAM", 563: "SHADOWBALL", 565: "AQUATAIL", 567: "WINGATTACK", 569: "GUNKSHOT", 571: "NIGHTSLASH", 576: "FUTURESIGHT",

    579: "PSYCHIC", 581: "AIRSLASH", 584: "BLIZZARD", 586: "LEAFBLADE",
    587: "DISCHARGE", 589: "XSCISSOR", 591: "SPORE", 593: "HYDROPUMP",
    594: "HYDROPUMP", 596: "DISCHARGE", 598: "IRONHEAD", 601: "GEARGRIND",
    604: "THUNDERBOLT", 606: "PSYBEAM", 609: "SHADOWBALL", 612: "OUTRAGE",
    614: "ICICLECRASH", 615: "ICEBEAM", 618: "EARTHPOWER", 620: "AURASPHERE",
    621: "DRAGONCLAW", 623: "IRONHEAD", 625: "NIGHTSLASH", 626: "HEADCHARGE",
    628: "BRAVEBIRD", 630: "DARKPULSE", 631: "FLAMETHROWER", 632: "XSCISSOR",
    635: "DRAGONPULSE", 637: "FIREBLAST", 638: "SECRETSWORD",
    639: "STONEEDGE", 640: "LEAFBLADE", 641: "HURRICANE", 642: "THUNDER",
    643: "BLUEFLARE", 644: "BOLTSTRIKE", 645: "EARTHQUAKE", 646: "ICEBURN",
    647: "SECRETSWORD", 648: "RELICSONG", 649: "TECHNOBLAST",
}


def vocabulary() -> dict[str, str]:
    """Move name -> type token, read from the sketch's move table."""
    text = SKETCH.read_text()
    return {m.group(1): m.group(3) for m in MOVE_RE.finditer(text)}


def norm(name: str) -> str:
    return name.upper().replace("-", "").replace(" ", "").replace("'", "")


def cached(kind: str, key: str) -> Path | None:
    return None if CACHE is None else CACHE / kind / f"{key}.json"


def get(url: str, kind: str, key: str) -> dict:
    path = cached(kind, key)
    if path is not None and path.exists():
        return json.loads(path.read_text())
    # curl rather than urllib: this host's Python has no CA bundle that PokéAPI
    # accepts, while curl's does.
    for attempt in range(4):
        p = subprocess.run(["curl", "-sS", "--fail", "--max-time", "25", url],
                           capture_output=True, text=True)
        if p.returncode == 0 and p.stdout:
            try:
                data = json.loads(p.stdout)
            except json.JSONDecodeError:
                pass
            else:
                if path is not None:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(p.stdout)
                return data
        time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"could not fetch {url}")


def move_index() -> dict[str, str]:
    """Normalised name -> the API's own spelling, for every move in the games."""
    data = get(MOVE_INDEX, "move-index", "index")
    return {norm(m["name"]): m["name"] for m in data["results"]}


def move_info(vocab: dict[str, str], index: dict[str, str]) -> dict[str, dict]:
    """Real type, power and damage class for every move the sketch can draw."""
    info: dict[str, dict] = {}
    missing: list[str] = []
    for name, tok in vocab.items():
        api = index.get(name)
        if api is None:
            missing.append(name)
            continue
        data = get(API.format("move", api), "move", api)
        power = data["power"] or 0
        if power <= 0:
            power = NOMINAL_POWER.get(name, 0)
        info[name] = {
            "name": name,
            "type": data["type"]["name"],
            "class": data["damage_class"]["name"],
            "power": power,
            "sketch_type": tok,
        }
    if missing:
        print(f"no API move for {len(missing)} sketch moves: "
              + ", ".join(sorted(missing)))
    return info


def candidates(pid: int, data: dict, info: dict[str, dict]) -> dict[str, dict]:
    """Every move this species can know by level-up in Gen 5, with its level."""
    types = [t["type"]["name"] for t in data["types"]]
    best: dict[str, dict] = {}
    for mv in data["moves"]:
        name = norm(mv["move"]["name"])
        if name not in info:
            continue
        for v in mv["version_group_details"]:
            if v["move_learn_method"]["name"] != "level-up":
                continue
            vg = v["version_group"]["name"]
            if vg not in GEN5_GROUPS:
                continue
            level = int(v["level_learned_at"])
            rank = 2 if vg == GEN5_GROUPS[0] else 1
            if name in best and (best[name]["level"], best[name]["rank"]) >= (level, rank):
                continue
            best[name] = {
                **info[name],
                "level": level,
                "rank": rank,
                "stab": info[name]["type"] in types,
            }
    return best


def pick(pool: dict[str, dict]) -> dict | None:
    """The last move the species actually learns, damaging moves first."""
    if not pool:
        return None
    damaging = [c for c in pool.values() if c["class"] != "status"]
    choice = damaging or list(pool.values())
    # Level is the rule; the rest only breaks ties between moves learned at the
    # same level (STAB, then the harder hit).
    return max(choice, key=lambda c: (c["level"], c["rank"], c["stab"], c["power"], c["name"]))


def stat_at_50(data: dict, name: str) -> int:
    for s in data["stats"]:
        if s["stat"]["name"] == name:
            return (2 * s["base_stat"] + 31) * 50 // 100 + 5
    raise KeyError(name)


EFF_VS_NORMAL = {"fighting": 2, "ghost": 0}


def damage_of(data: dict, mv: dict) -> int:
    """Gen 5's damage formula at level 50: no crit, no random roll."""
    power = mv["power"]
    if power <= 0 or mv["class"] == "status":
        return 0
    eff = EFF_VS_NORMAL.get(mv["type"], 1)   # Ditto is pure Normal
    if eff == 0:
        return 0
    if mv["class"] == "physical":
        atk, dfn = stat_at_50(data, "attack"), DITTO_DEF
    else:
        atk, dfn = stat_at_50(data, "special-attack"), DITTO_DEF
    base = ((2 * 50 // 5 + 2) * power * atk // dfn) // 50 + 2
    if mv["stab"]:
        base = base * 3 // 2
    return max(1, base * eff)


def main() -> None:
    vocab = vocabulary()
    if not vocab:
        raise SystemExit("could not parse MOVE_LIST from pokedex.ino")
    info = move_info(vocab, move_index())
    print(f"vocabulary: {len(vocab)} moves, {len(info)} with move data")

    raw: dict[int, dict] = {}
    for pid in range(1, 650):
        raw[pid] = get(API.format("pokemon", pid), "pokemon", str(pid))
        if pid % 50 == 0:
            print(f"  {pid}/649")

    result: dict[str, dict] = {}
    dropped: list[str] = []
    for pid in range(1, 650):
        data = raw[pid]
        pool = candidates(pid, data, info)
        chosen = None
        want = OVERRIDES.get(pid)
        if want:
            if want in pool:
                chosen = pool[want]
            else:
                dropped.append(f"{pid}:{want}")
        if chosen is None:
            chosen = pick(pool)

        entry: dict = {"name": data["name"]}
        if chosen is None:
            # No usable move at all (Ditto only learns Transform, Unown only
            # Hidden Power): leave it empty so the firmware uses its type pool.
            entry.update({"move": "", "move_type": "", "power": 0,
                          "class": "status", "level": 0, "damage": 0})
        else:
            entry.update({
                "move": chosen["name"],
                "move_type": chosen["type"],
                "power": chosen["power"],
                "class": chosen["class"],
                "level": chosen["level"],
                "damage": damage_of(data, chosen),
            })
        entry["stats"] = [s["base_stat"] for s in data["stats"]]
        result[str(pid)] = entry

    OUT.write_text(json.dumps(result, indent=1) + "\n")

    counts: dict[str, int] = {}
    for entry in result.values():
        counts[entry["move"] or "(pool)"] = counts.get(entry["move"] or "(pool)", 0) + 1
    ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    dmg = [e["damage"] for e in result.values()]
    print(f"wrote {OUT} ({len(result)} entries, {len(counts)} different moves)")
    print("most common:", ", ".join(f"{name} {count}" for name, count in ranked[:8]))
    print(f"damage vs Ditto: {min(dmg)}-{max(dmg)}, "
          f"{(1 - sum(1 for d in dmg if d == 0) / len(dmg)) * 100:.1f}% of entries connect")
    if dropped:
        print(f"overrides the species cannot learn in Gen 5 ({len(dropped)}): "
              + ", ".join(dropped))


if __name__ == "__main__":
    main()
