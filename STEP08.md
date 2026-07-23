# MDVWB configuration cycle — step 8

Static web editor for `/mdvwb/config`.

The page can:

- add a bus;
- edit its numeric ID, enabled state, device port and polling addresses;
- delete a bus from the local draft;
- cancel all unsaved changes;
- validate IDs, ports and addresses in the browser;
- publish the complete normalized JSON to `/mdvwb/config/set` with retain disabled;
- display `/mdvwb/config/result` returned by the manager.

This step does not add start, stop, restart or discovery buttons. Those are step 9.
