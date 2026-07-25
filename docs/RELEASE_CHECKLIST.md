# MDVWB pre-release checklist

This checklist describes the verified project state before the offline Wiren Board package is produced.

## Source tree

The canonical C++ sources live only under `src/driver`, `src/manager`, and `src/scheduler`.
Intermediate `STEP*.md` files and duplicate root-level source copies are not part of the project.

## Local verification

```powershell
cmake --preset x64-debug
cmake --build "out\build\x64-debug"
ctest --test-dir "out\build\x64-debug" -C Debug --output-on-failure
```

Expected result: all 11 CTest tests pass.

## Functional areas covered

- MDV XYE protocol framing, parsing, polling and command queue;
- arbitrary configured RS-485 buses and discovery;
- manager configuration and service synchronization;
- multiple independent dashboard panels and background images;
- individual and group fan-coil control;
- weekly and one-time schedules;
- scheduler execution, confirmation timeout and duplicate-run protection;
- compact administrator and user web interfaces.

## Final hardware checks

The final release step still requires validation on the target Wiren Board with real RS-485 hardware, MQTT, systemd and the production web server.
