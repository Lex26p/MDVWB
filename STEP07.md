# MDVWB configuration cycle — step 7

This step adds a read-only static web interface under `www/mdvwb/`.

The page:

- connects to the Wiren Board MQTT WebSocket endpoint `/mqtt`;
- reads retained configuration from `/mdvwb/config`;
- reads manager status from `/mdvwb/status`;
- reads dynamic bus states from `/mdvwb/buses/+/status`;
- reads the last discovery state and result for every bus;
- creates cards for any number of configured buses;
- contains no external CDN or internet dependency;
- does not edit configuration or send control commands yet.

For local visual checking, open `index.html?demo=1` through a local HTTP server.
