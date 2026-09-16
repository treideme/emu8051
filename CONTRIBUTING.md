Contributing
============

This is a maintained fork of [jarikomppa/emu8051](https://github.com/jarikomppa/emu8051).
Upstream has been quiet since 2022 and carries a number of unmerged fixes; this
fork exists to keep those moving. Contributions are welcome under the rules
below, which exist to keep the fork mergeable back upstream should Jari return
to it.

Scope: which branch
-------------------

| branch | accepts |
| --- | --- |
| `master` | **defects in the original 8051/8052 emulator only** — the CPU core, the opcode implementations, timing, and the curses UI |
| `sim` | the peripheral-simulation layer, device models, Python bindings, GUI, and anything built on top |

**`master` is deliberately narrow.** Every commit on it should be a clean patch
against upstream's head, so that the defect fixes stay separable from this
fork's own experiments. A feature PR opened against `master` will be asked to
move to `sim`.

Ground truth
------------

**The Intel *MCS-51 Microcontroller Family User's Manual*, order number
272383-002 (February 1994), decides questions of correct behaviour.**

- https://www.bitsavers.org/components/intel/8051/MCS-51_Users_Manual_Feb94.pdf
- https://web.mit.edu/6.115/www/document/8051.pdf (mirror)
- https://archive.org/details/bitsavers_intel8051M4_15073500 (Internet Archive)

Where the manual is silent, cite the data sheet for the specific part.

**Every defect fix must cite a page.** Give both the printed page number and
the PDF page number — the scan's two numberings do not agree, and a citation
that cannot be found is not a citation. Quote only the sentence, table row or
worked example that carries the point.

The manual's own worked examples are the best evidence available: several
defects already fixed here were caught precisely because the emulator did not
reproduce them.

Another implementation agreeing with you is **corroboration, not a citation**.
If you want to lean on one — SDCC's µCsim is the obvious candidate — first
show that it matches the manual on the same instruction, then use it. An
implementation that has not been checked against the specification is just a
second opinion.

Tests
-----

Defect fixes need a test that **fails before the change and passes after it**:

```
make -C tests check
```

Standalone C, no framework, no curses. Include a **negative control** — an
assertion that must not change — because a check that can only pass is not a
check. Paste both the failing and the passing output into the pull request.

Generated patches
-----------------

**A patch produced by a language model and submitted without independent
verification will not be merged.** Use any tools you find useful; the
requirement is that you have read the cited page yourself and confirmed the
behaviour yourself before asking anyone to review it.

The reason is specific to this codebase rather than general. An AI-assisted fix
for opcodes 0x86/0x87 was submitted upstream in
[#41](https://github.com/jarikomppa/emu8051/pull/41). It reads convincingly and
is wrong — it changes the accessors and ends up implementing the opposite
instruction. [#44](https://github.com/jarikomppa/emu8051/pull/44) pointed this
out, and deriving the fix independently here produced #44's version. Reviewing
a plausible wrong patch costs more than reviewing no patch, and in an emulator
a wrong fix is worse than an absent one: it silently corrupts every program run
through it afterwards.

Privacy
-------

Do not include private repository names, local paths, internal project names or
proprietary firmware in patches, tests, commit messages or comments. Where a
test needs an external binary, take the location from an environment variable —
see `EMU8051_DEMO_BUILD` in `python/tests/`.
