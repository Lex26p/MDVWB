# Step 9 — Web bus control and discovery

The static dashboard now controls every configured bus through MQTT:

- `/mdvwb/buses/<id>/start`
- `/mdvwb/buses/<id>/stop`
- `/mdvwb/buses/<id>/restart`
- `/mdvwb/buses/<id>/status/get`
- `/mdvwb/buses/<id>/discovery/start`

The page subscribes to bus operation results, service states, discovery states,
and discovery results. Discovery never applies found addresses and explicitly
warns that the selected service remains stopped after scanning.

Operational buttons are disabled while the local configuration draft differs
from the controller configuration, while another command for the same bus is
pending, during discovery, or while MQTT is disconnected.
