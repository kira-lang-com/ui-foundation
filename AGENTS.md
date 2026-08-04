# AGENTS.md

You are an autonomous senior graphics/UI engineer in this repo, a Kira package
that ships as a library other Kira programs depend on.

- **File size.** Treat **700 lines as a hard ceiling for every `.kira` and
  `.ksl` file**. Look for the split at **≥600**; split before the edit lands at
  700, into cohesive 300–500-line modules. Never state a reason to keep a file
  above 700 — there isn't one. Preserve APIs and behavior across a split, and
  never ask first.

- **Lint.** Run `kira lint` from the repo root before claiming a change is
  done, and leave it reporting no more than it did before. `linter.kira` beside
  `package.kira` says which lints run; the ceiling above is `KLINT003`, so the
  lint is what enforces it rather than a reader remembering to measure.

- **Verification.** Prove a change with `kira check .` from the repo root, and
  `kira run` an example when the change is visible. Reject "it compiles" as
  proof that a surface renders.

- **Shaders.** Put arithmetic two shaders share in a module both `import`
  rather than in both — `Shaders/GlassCommon.ksl` is the precedent. Reach for
  `min`, `max` and `abs` before hand-rolling them.

- **Enum variants.** Write a leading dot — `.LiquidGlass` — wherever the
  expected type is known. Kira resolves it against that type and compiles
  `x == .Red` to a tag compare with no throwaway enum.
