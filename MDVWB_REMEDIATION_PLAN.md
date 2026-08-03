# MDVWB remediation plan

This file is the persistent execution log for the MDVWB audit remediation work.

## Working rules

- Repository: `Lex26p/MDVWB`
- Working branch: `master`
- Initial audited commit: `2f7dd9395be326105671de2027f9b44cf8f19074`
- The repository is the only source of truth for code used in each step.
- Work always continues in the same branch.
- ChatGPT only reads GitHub. It does not create commits, push branches, or modify the remote repository.
- Each step is delivered as a ZIP archive containing paths relative to the repository root.
- A step is complete only after the maintainer:
  1. extracts the archive over the project;
  2. builds and runs the requested tests;
  3. performs the listed manual checks;
  4. commits and pushes the changes;
  5. sends the resulting commit SHA.
- The next step must be based on the pushed SHA from the preceding completed step.
- When a step fails, the same step remains `IN PROGRESS` until a corrected archive is tested and committed.

## Status values

- `PENDING`: not started.
- `IN PROGRESS`: archive prepared or corrections are being made.
- `BLOCKED`: cannot continue because the preceding step failed or required evidence is missing.
- `COMPLETED`: tested, pushed, and linked to a commit SHA.

## Step log

| Step | Status | Subject | Required outcome | Commit SHA |
|---:|---|---|---|---|
| 00 | COMPLETED | Add remediation tracker | This file exists in the repository and becomes the persistent work log. | `606865556ad5ea6ce777c3eda35f8ad3f10b738b` |
| 01 | IN PROGRESS | Scheduler causal factual ordering | A factual message received before command publication can never confirm that command. Add a regression test for callback-before-command ordering. | pending |
| 02 | PENDING | Manager recovery snapshot | A failed apply with missing or damaged `buses.json` restores the real environment files and exact active/enabled service states instead of applying an empty rollback topology. | pending |
| 03 | PENDING | Secure installer temporary paths | Replace predictable `/tmp/...$$` paths with secure `mktemp`/`mktemp -d`, restrictive permissions, and signal-safe cleanup. | pending |
| 04 | PENDING | Shared capability contract | Define one normalized capability model derived from protocol/profile data and publish it through the management and device MQTT contracts. | pending |
| 05 | PENDING | Capability enforcement | Operator controls, group commands, schedules, metadata, and retained cleanup reject or remove unsupported controls. | pending |
| 06 | PENDING | Modbus confirmation and web freshness | Invalid Power read-back marks the device offline in driver and MQTT. After reconnect, web control remains disabled until a fresh factual snapshot is received. | pending |
| 07 | PENDING | Durable MQTT retained lifecycle | Add durable offline and retained-cleanup intents, reconnect replay, empty-payload deletion handling, and protocol/profile-switch reconciliation. | pending |
| 08 | PENDING | Discovery revision and lifecycle safety | Bind discovery to an immutable configuration snapshot, clear previous results on start, reject stale completion, and prevent serial-port lifecycle races. | pending |
| 09 | PENDING | Request correlation and queue semantics | Add request IDs/revision correlation, explicit overload results, non-lossy transactional queues, and bounded manager/scheduler processing batches. | pending |
| 10 | PENDING | Migration, backup, dry-run, provenance | Preserve Modbus fields in migration, restore the actual web root, safely select backups, make dry-run non-mutating, and reject or identify dirty source packages. | pending |
| 11 | PENDING | Package format 2 and verifier | Require a declared component inventory, Modbus runtime, production profile, and complete transitive web module set. | pending |
| 12 | PENDING | Release CI, health, uninstall | Add profile path triggers, real archive lifecycle tests, truthful cleanup outcomes, installation/operational health separation, and release hardening. | pending |
| 13 | PENDING | Documentation and UI cleanup | Update architecture/release documentation and remove the obsolete “Use discovered addresses” button, helper, handler, and tests. | pending |

## Step dependencies

```text
01 Scheduler ordering
   independent, release-blocking

02 Recovery snapshot
   independent, release-blocking
   required before broader installer rollback changes

03 Secure temporary paths
   independent, security-blocking
   required before installer transaction refactoring

04 Shared capability contract
   -> 05 Capability enforcement
   -> 07 Retained reconciliation

06 Driver confirmation and freshness
   -> 07 Durable MQTT lifecycle

08 Discovery revision safety
   benefits from 09 request correlation but must not wait for it

09 Request correlation and queue semantics
   required before final save/execute protocol stabilization

10 Migration and installer correctness
   builds on 02 and 03

11 Package format 2
   -> 12 Release CI and lifecycle tests

13 Documentation and UI cleanup
   follows the stabilized behavior of all prior steps
```

## Release blockers

The release must remain pre-production until all of the following steps are complete:

- Step 01: scheduler causal ordering;
- Step 02: safe recovery from damaged configuration;
- Step 03: secure root temporary files;
- Step 05: end-to-end capability enforcement;
- Step 06: Power mismatch offline and reconnect freshness;
- Step 11: complete package verification;
- the Modbus hardware gate in Step 12.

## Per-step delivery format

Each delivered step must contain:

1. A brief description of the defect and the change.
2. A ZIP archive named:

   ```text
   MDVWB-step-NN-short-description.zip
   ```

3. An extraction command for PowerShell.
4. Only the required build and test commands.
5. A manual verification checklist.
6. Git commands with an appropriate commit message.
7. The number of remaining steps.

## Updating this file

When a step is accepted:

1. change its status to `COMPLETED`;
2. replace `pending` with the pushed commit SHA;
3. change the next step to `IN PROGRESS`;
4. do not rewrite historical commit SHAs;
5. include the updated copy of this file in the next step archive.

If a step is split because its implementation proves too large, insert substeps such as `04A` and `04B`, preserve the original objective, and update the remaining-step count explicitly.

## Current position

- Current step: `01`
- Current status: `IN PROGRESS`
- Completed technical remediation steps: `0`
- Remaining technical remediation steps including the current step: `13`
