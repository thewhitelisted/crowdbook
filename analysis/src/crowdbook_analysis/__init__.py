"""Analysis and experiments for crowdbook simulation runs."""

import os

# The work runs in parallel a market at a time, the markets' engines on every core. Numerical
# libraries that spread each small step over the cores too only contend with them, their idle
# threads spinning: a fit ran several times the CPU it needed. One thread each, unless asked.
for _name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS", "POLARS_MAX_THREADS"):
    os.environ.setdefault(_name, "1")
