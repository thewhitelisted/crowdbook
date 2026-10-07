"""One chart style for every figure: thin marks, recessive axes and a validated palette.

The three series colors are the first three slots of a categorical palette validated as a set for
color-vision deficiencies. The third falls below 3:1 contrast on the surface, so multi-series
charts always carry a legend and, where they fit, direct labels at the line ends.
"""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
SERIES = ("#2a78d6", "#eb6834", "#1baf7a")

# Sizes in points at 100 dpi: 2 px lines, 8 px markers with a 2 px ring in the surface color.
LINE = 1.44
MARKER = 5.8
RING = 1.44
HAIRLINE = 0.72


def figure(title: str, xlabel: str, ylabel: str):
    """A figure with one styled axis."""
    fig, ax = plt.subplots(figsize=(7.2, 4.2), dpi=100)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", color=INK, fontsize=12, pad=14)
    ax.set_xlabel(xlabel, color=INK_SECONDARY, fontsize=10)
    ax.set_ylabel(ylabel, color=INK_SECONDARY, fontsize=10)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(AXIS)
        ax.spines[side].set_linewidth(HAIRLINE)
    ax.tick_params(colors=AXIS, labelcolor=INK_MUTED, labelsize=9, length=3, width=HAIRLINE)
    ax.grid(True, color=GRID, linewidth=HAIRLINE)
    ax.set_axisbelow(True)
    return fig, ax


def panels(title: str, subtitles: list[str], xlabel: str, ylabel: str):
    """A figure of side-by-side styled axes sharing a y scale, one per subtitle."""
    fig, axes = plt.subplots(1, len(subtitles), figsize=(7.2, 4.0), dpi=100, sharey=True)
    fig.patch.set_facecolor(SURFACE)
    fig.suptitle(title, x=0.02, ha="left", color=INK, fontsize=12)
    for index, (ax, subtitle) in enumerate(zip(axes, subtitles, strict=True)):
        ax.set_facecolor(SURFACE)
        ax.set_title(subtitle, loc="left", color=INK_SECONDARY, fontsize=10)
        ax.set_xlabel(xlabel, color=INK_SECONDARY, fontsize=10)
        if index == 0:
            ax.set_ylabel(ylabel, color=INK_SECONDARY, fontsize=10)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(AXIS)
            ax.spines[side].set_linewidth(HAIRLINE)
        ax.tick_params(colors=AXIS, labelcolor=INK_MUTED, labelsize=9, length=3, width=HAIRLINE)
        ax.grid(True, color=GRID, linewidth=HAIRLINE)
        ax.set_axisbelow(True)
    return fig, axes


def band(ax, low: float, high: float, label: str) -> None:
    """A light gray band, such as the range noise alone would produce."""
    ax.axhspan(low, high, color=INK_MUTED, alpha=0.15, linewidth=0, label=label)


def line(ax, x, y, slot: int, label: str, *, markers: bool = True, error=None) -> None:
    """A 2 px line in series color `slot`, with ringed markers and optional error bars."""
    color = SERIES[slot]
    if error is not None:
        ax.errorbar(x, y, yerr=error, fmt="none", ecolor=color, elinewidth=HAIRLINE, capsize=0)
    ax.plot(
        x,
        y,
        color=color,
        linewidth=LINE,
        solid_capstyle="round",
        solid_joinstyle="round",
        marker="o" if markers else None,
        markersize=MARKER,
        markerfacecolor=color,
        markeredgecolor=SURFACE,
        markeredgewidth=RING,
        label=label,
    )


def reference(ax, x, y, label: str) -> None:
    """A thin muted line for a reference curve, such as a normal distribution."""
    ax.plot(x, y, color=INK_MUTED, linewidth=HAIRLINE * 1.5, label=label)


def stems(ax, x, y, slot: int, label: str, offset: float = 0.0) -> None:
    """Thin stems with dots, for a distribution over a few discrete values."""
    color = SERIES[slot]
    x = np.asarray(x, dtype=float) + offset
    ax.vlines(x, 0, y, color=color, linewidth=LINE)
    ax.plot(
        x,
        y,
        linestyle="none",
        marker="o",
        markersize=MARKER,
        markerfacecolor=color,
        markeredgecolor=SURFACE,
        markeredgewidth=RING,
        label=label,
    )


def finish(fig, ax, path: Path, *, end_labels: bool = True) -> None:
    """Adds the legend (for two or more series) and direct labels at the line ends when they do
    not collide, then saves the figure. With several panels, the legend goes on the first."""
    axes = list(np.atleast_1d(ax))
    handles, _ = axes[0].get_legend_handles_labels()
    if len(handles) >= 2:
        axes[0].legend(frameon=False, labelcolor=INK_SECONDARY, fontsize=9)
    if end_labels:
        for each in axes:
            _label_line_ends(fig, each)
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=200, facecolor=SURFACE)
    plt.close(fig)


def _label_line_ends(fig, ax, min_gap_px: float = 14.0) -> None:
    lines = [artist for artist in ax.get_lines() if artist.get_color() in SERIES]
    if len(lines) < 2:
        return
    ends = []
    for artist in lines:
        x, y = artist.get_xdata(), artist.get_ydata()
        finite = np.isfinite(np.asarray(y, dtype=float))
        if not finite.any():
            return
        last = np.flatnonzero(finite)[-1]
        ends.append((x[last], y[last], artist.get_label()))
    fig.canvas.draw()
    heights = sorted(ax.transData.transform((x, y))[1] for x, y, _ in ends)
    if any(b - a < min_gap_px for a, b in zip(heights, heights[1:], strict=False)):
        return  # labels would collide; the legend carries identity instead
    for x, y, label in ends:
        ax.annotate(
            label,
            (x, y),
            xytext=(8, 0),
            textcoords="offset points",
            va="center",
            color=INK_SECONDARY,
            fontsize=9,
            annotation_clip=False,
        )
