# MDVWB developer documentation

> Purpose: entry point for developers and AI agents working on MDVWB.
>
> Read this file first when continuing protocol or driver development.

## Current project documentation

### Modbus RTU work

Read in this order:

1. [`modbus/ARCHITECTURE.md`](modbus/ARCHITECTURE.md)  
   Core architectural decisions: logical addresses `1..63`, profile-driven Modbus, scan model, separation of logical address / Slave ID / registers, common semantic state.

2. [`modbus/PROFILE_FORMAT.md`](modbus/PROFILE_FORMAT.md)  
   Proposed data-driven profile format: addressing models, read/write points, scaling, enum mappings, capabilities, composite values, validation and safe scan probes.

3. [`modbus/REFERENCE_VRF_ADD_CONTROLLER.md`](modbus/REFERENCE_VRF_ADD_CONTROLLER.md)  
   Analysis of the first supplied real Modbus equipment table. Contains confirmed information and explicitly marked unknowns that must not be guessed.

4. [`modbus/ROADMAP.md`](modbus/ROADMAP.md)  
   Intended implementation sequence from documentation through reusable Modbus support and hardware validation.

5. [`modbus/STATUS.md`](modbus/STATUS.md)  
   Actual development state. Read this before starting work. Do not infer completion from the roadmap.

### MDV protocol research

- [`protocols/MDV_V2_RESEARCH.md`](protocols/MDV_V2_RESEARCH.md)  
  Research notes for the newer MDV protocol variant. Implementation is intentionally postponed until missing speed-response behavior is studied.

## Rules for future AI/developer work

Before changing protocol code:

1. Read `modbus/STATUS.md` to know what is actually complete.
2. Read the relevant architecture/specification document.
3. Re-check the current repository code before implementing, because documentation describes intended architecture and the code may have evolved.
4. Do not treat hypotheses or `OPEN / VERIFY` items as protocol facts.
5. Do not invent undocumented register behavior.
6. Keep manufacturer-specific Modbus knowledge in profiles whenever possible.
7. Preserve existing MDV behavior unless the task explicitly requires changing it.
8. Prefer small, testable implementation steps.
9. Update `STATUS.md` when an implementation step is accepted.
10. If an architectural decision changes, update the architecture/specification documents as part of the same accepted change.

## Modbus design summary

The high-level model is:

```text
                     Common MDVWB semantic model
                               |
                  protocol-independent driver boundary
                       /                   \
                    MDV                  Modbus
                                           |
                                    equipment profile
                                           |
                                      Modbus RTU
```

Important invariants:

```text
MDVWB logical device address: 1..63
```

A logical address is **not necessarily** the Modbus Slave ID.

Supported physical layouts must include:

```text
different Slave IDs + same register map
```

and:

```text
one Slave ID + different register blocks
```

Modbus scan always evaluates logical candidates:

```text
1..63
```

using a **known profile and safe read-only probe**.

There is no blind discovery of unknown Modbus equipment.

## Profile principle

A normal new Modbus air-conditioner/fan-coil should usually be added as a new profile file.

The common Modbus engine should not need manufacturer-specific branches for ordinary equipment.

A profile may define:

- serial defaults;
- physical addressing rules;
- Slave ID mapping;
- register offsets/stride;
- read/write register locations;
- Power/Mode/FanSpeed mappings;
- numeric transformations;
- min/max/step;
- capabilities;
- safe probe;
- composite semantic values where one MDVWB value requires multiple Modbus points.

For numeric values the generic model includes:

```text
physical = raw * scale + offset
```

and for writes:

```text
raw = (physical - offset) / scale
```

## Documentation discipline

Use the documents for different purposes:

```text
ARCHITECTURE.md     -> why the system is structured this way
PROFILE_FORMAT.md   -> how Modbus profiles are intended to be described
REFERENCE_*.md      -> facts/unknowns for concrete equipment
ROADMAP.md          -> planned implementation order
STATUS.md           -> what is actually done now
```

Do not turn `ROADMAP.md` into a completion log.

Do not mark an item complete in `STATUS.md` merely because code exists; mark it complete after the agreed verification succeeds.
