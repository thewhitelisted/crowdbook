"""Many labelled synthetic markets, fast: each one's settings drawn at random from given ranges,
written as tapes that a learner or an analyst reads, with each market's hidden settings and who
made money in it as its ground truth.

    uv run --project analysis crowdbook-generate spec.toml --markets 10000 --out dataset

The spec is a calibration spec (see `calibrate`): a scenario template with ${name} wherever a
setting goes, and each setting's range. Each market draws every setting afresh, uniformly on its
scale (in proportion where `log`, the default for a positive range), whole numbers rounded, and
runs under a seed of its own. `--fixed name=value,...` holds settings where they are given.

The dataset is a directory of shards, each a tape (`shard-00000`, ...), so that a large dataset
never has to fit in memory at once, beside `markets.csv`, every market's labels in one table:

- `market`, `shard` and `seed`: which market, where it is, and its seed, which replays it exactly
  (with its settings, through the template);
- one column per drawn setting, as the scenario used it;
- `pnl_<group>` and `traded_<group>`: what each group of traders made after fees, valued at
  settlement or the true value where there is one, and the lots it traded;
- `outcome`, where the market settled.

`dataset.toml` records the spec, the template and the seeds, so that the dataset can be made again
byte for byte.
"""

import argparse
import json
import os
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis import tape as tapes
from crowdbook_analysis.calibrate import Spec, read_spec
from crowdbook_analysis.runner import Run, find_tool

FIRST_SEED = 1_000_001
SHARD = 500


def draw(spec: Spec, rng: np.random.Generator, fixed: dict[str, float]) -> dict[str, float]:
    """One market's settings: each drawn uniformly on its scale, unless fixed."""
    values = {}
    for p in spec.parameters:
        values[p.name] = fixed[p.name] if p.name in fixed else p.from_unit(float(rng.random()))
    return values


def generate(spec: Spec, tool: Path, markets: int, out: Path, *, seed: int = 1,
             fixed: dict[str, float] | None = None, shard: int = SHARD,
             parquet: bool = True, sample_interval: float = tapes.SAMPLE_INTERVAL,
             workers: int | None = None, log=print) -> pl.DataFrame:
    """Writes the dataset and returns its table of labels."""
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(seed)
    settings = [draw(spec, rng, fixed or {}) for _ in range(markets)]
    rows = []
    started = time.time()
    for first in range(0, markets, shard):
        chosen = range(first, min(first + shard, markets))
        name = f"shard-{first // shard:05d}"

        def one(index: int, scratch: Path) -> tapes.Market:
            values = settings[index]
            job = Run(spec.scenario(values), FIRST_SEED + seed * 10_000_000 + index)
            market = tapes.simulated_market(tool, job, scratch, str(index),
                                            {k: float(v) for k, v in values.items()},
                                            sample_interval)
            market.labels["shard"] = name
            return market

        with tempfile.TemporaryDirectory() as scratch, ThreadPoolExecutor(
                max_workers=workers or os.cpu_count()) as pool:
            made = list(pool.map(lambda i: one(i, Path(scratch)), chosen))
        options = tapes.options_for(spec.scenario(settings[chosen[0]]))
        tapes.write_tape(tapes.Tape(made, options), out / name, parquet=parquet)
        for m in made:
            rows.append({"market": m.name} | m.labels | (
                {"outcome": m.outcome} if m.outcome is not None else {}))
        done = chosen[-1] + 1
        rate = done / (time.time() - started)
        log(f"{done} of {markets} markets, {rate:.0f} a second")
    labels = pl.DataFrame(rows, infer_schema_length=None)
    labels.write_csv(out / "markets.csv")
    return labels


def read_dataset(directory: Path | str):
    """A dataset's markets one at a time, shard by shard, so that it never has to fit in memory:
    each a `tape.Market` whose labels hold its settings and outcomes. The table of every market's
    labels is `markets.csv` beside the shards."""
    directory = Path(directory)
    for shard in sorted(directory.glob("shard-*")):
        yield from tapes.read_tape(shard).markets


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("spec", type=Path)
    parser.add_argument("--markets", type=int, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=1, help="draws the settings and the seeds")
    parser.add_argument("--fixed", default="", help="settings to hold, name=value,...")
    parser.add_argument("--shard", type=int, default=SHARD, help="markets a shard")
    parser.add_argument("--csv", action="store_true", help="write CSV, not Parquet")
    parser.add_argument("--sample-interval", type=float,
                        help="seconds between samples of the book and the value (default: the "
                        "spec's sample_interval, else 0.1)")
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    args = parser.parse_args()
    spec = read_spec(args.spec)
    names = {p.name for p in spec.parameters}
    fixed = {}
    for item in filter(None, args.fixed.split(",")):
        name, _, value = item.partition("=")
        if name not in names:
            parser.error(f"--fixed: {name} is not a setting of the spec")
        fixed[name] = float(value)
    if args.sample_interval is None:
        args.sample_interval = spec.sample_interval
    generate(spec, find_tool(args.crowdbook), args.markets, args.out, seed=args.seed,
             fixed=fixed, shard=args.shard, parquet=not args.csv,
             sample_interval=args.sample_interval)
    first = FIRST_SEED + args.seed * 10_000_000
    record = [f"markets = {args.markets}", f"seed = {args.seed}",
              f"# market i ran under seed first_seed + i", f"first_seed = {first}",
              f"sample_interval = {args.sample_interval}",
              f"template = {json.dumps(spec.template)}", "", "[fixed]"]
    record += [f"{k} = {v}" for k, v in fixed.items()]
    for p in spec.parameters:
        record += ["", f"[ranges.{p.name}]", f"low = {p.low}", f"high = {p.high}",
                   f"log = {str(p.log).lower()}", f"whole = {str(p.whole).lower()}",
                   f'kind = "{p.kind}"']
    (args.out / "dataset.toml").write_text("\n".join(record) + "\n")


if __name__ == "__main__":
    main()
