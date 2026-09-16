<!--
  This repository is a maintained fork of jarikomppa/emu8051.

  master accepts ONE kind of change: a fix for a defect in the original
  8051/8052 emulator, demonstrated against published documentation.

  Peripheral models, the Python bindings, the GUI and anything else built on
  top of the emulator live on the `sim` branch and are out of scope here.
  If your change is a feature, open it against `sim` instead.
-->

## What is wrong

<!-- The incorrect behaviour, in one or two sentences. -->

## What the documentation says

**Required.** The ground truth for this repository is the Intel *MCS-51
Microcontroller Family User's Manual*, order number **272383-002** (February
1994):

- https://www.bitsavers.org/components/intel/8051/MCS-51_Users_Manual_Feb94.pdf
- https://web.mit.edu/6.115/www/document/8051.pdf (mirror)

Cite the **page number** — both the printed page and the PDF page, because the
scan's two numberings differ:

> Manual 272383-002, printed p. ___ (PDF p. ___), section ___
>
> <!-- The specific sentence, table row or worked example that establishes the
>      correct behaviour. Quote the minimum needed. -->

If the manual does not settle it, say so and cite the device data sheet
instead. **A change with no documentary basis will not be merged**, however
obviously correct it looks.

## Minimal reproduction

<!--
  A test that FAILS before this change and PASSES after it. Put it in tests/
  and keep it standalone C — no framework, no curses.

  Include a negative control: something that must NOT change. A check that can
  only pass is not a check.
-->

## Verification

- [ ] The cited page says what I claim it says, and I have read it.
- [ ] `make -C tests check` passes.
- [ ] The new test fails without the fix. (Paste both outputs below.)
- [ ] This fixes a defect in the **original emulator**, not the `sim` layer.
- [ ] No private repository, path, project or file name appears in the diff,
      the commit messages, or the test data.

## On generated patches

**Patches produced by a language model and submitted without independent
verification will not be merged.** Use whatever tools you like — but you must
have read the cited page yourself and confirmed the behaviour yourself.

This is not a position about AI. It is a position about this codebase: an
AI-assisted patch for opcodes 0x86/0x87 was submitted upstream in
[#41](https://github.com/jarikomppa/emu8051/pull/41), looked plausible, and was
wrong — it implements the opposite instruction.
[#44](https://github.com/jarikomppa/emu8051/pull/44) said so, and an
independent derivation here agrees with #44. A wrong fix that reads well costs
more review time than no fix at all.
