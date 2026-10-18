"""CMA-ES, the search the fits and the learning probe share."""

import math

import numpy as np


class Cma:
    """CMA-ES, minimizing a function of a point (Hansen, 2016, with its default settings): each
    generation samples candidates around a mean from a normal distribution, then moves the mean
    toward the best of them and adapts the distribution's shape and size to the steps that
    worked."""

    def __init__(self, mean: np.ndarray, sigma: float, seed: int):
        n = len(mean)
        self.n, self.mean, self.sigma = n, np.array(mean, dtype=float), sigma
        self.size = 4 + int(3 * math.log(n))
        self.parents = self.size // 2
        weights = math.log(self.parents + 0.5) - np.log(np.arange(1, self.parents + 1))
        self.weights = weights / weights.sum()
        self.mueff = 1.0 / float(np.sum(self.weights**2))
        self.cc = (4 + self.mueff / n) / (n + 4 + 2 * self.mueff / n)
        self.cs = (self.mueff + 2) / (n + self.mueff + 5)
        self.c1 = 2 / ((n + 1.3) ** 2 + self.mueff)
        self.cmu = min(1 - self.c1,
                       2 * (self.mueff - 2 + 1 / self.mueff) / ((n + 2) ** 2 + self.mueff))
        self.damps = 1 + 2 * max(0.0, math.sqrt((self.mueff - 1) / (n + 1)) - 1) + self.cs
        self.chi = math.sqrt(n) * (1 - 1 / (4 * n) + 1 / (21 * n * n))
        self.pc, self.ps, self.covariance = np.zeros(n), np.zeros(n), np.eye(n)
        self.rng = np.random.default_rng(seed)
        self.generation = 0

    def ask(self) -> np.ndarray:
        values, vectors = np.linalg.eigh(self.covariance)
        self._vectors, self._scales = vectors, np.sqrt(np.maximum(values, 1e-20))
        steps = self.rng.standard_normal((self.size, self.n)) * self._scales @ vectors.T
        return self.mean + self.sigma * steps

    def tell(self, points: np.ndarray, scores: np.ndarray) -> None:
        best = points[np.argsort(scores)[: self.parents]]
        steps = (best - self.mean) / self.sigma
        step = self.weights @ steps
        self.mean = self.mean + self.sigma * step
        whitening = self._vectors @ np.diag(1 / self._scales) @ self._vectors.T
        self.ps = ((1 - self.cs) * self.ps
                   + math.sqrt(self.cs * (2 - self.cs) * self.mueff) * whitening @ step)
        self.generation += 1
        norm = float(np.linalg.norm(self.ps))
        held = (norm / math.sqrt(1 - (1 - self.cs) ** (2 * self.generation)) / self.chi
                < 1.4 + 2 / (self.n + 1))
        self.pc = (1 - self.cc) * self.pc + held * math.sqrt(
            self.cc * (2 - self.cc) * self.mueff) * step
        rank = (steps.T * self.weights) @ steps
        self.covariance = ((1 - self.c1 - self.cmu) * self.covariance
                           + self.c1 * (np.outer(self.pc, self.pc)
                                        + (not held) * self.cc * (2 - self.cc) * self.covariance)
                           + self.cmu * rank)
        self.sigma *= math.exp((self.cs / self.damps) * (norm / self.chi - 1))
