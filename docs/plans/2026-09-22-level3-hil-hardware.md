# Level-3 Hardware-in-the-Loop Shopping List

Date checked: 2026-09-22

## Recommended first build: route-CAN HIL

This is the useful next level after the software-only engagement gate. It feeds physical CAN and ignition into the comma4 through real hardware while replaying a known route.

1. **[panda jungle v2](https://comma.ai/shop/panda-jungle)** — required. It accepts up to six comma devices/pandas and provides a physical ignition switch. The official listing is $199 as checked. An extra red panda is not required for one comma4 because the comma4 already has its internal panda.
2. **[comma four OBD-C cable](https://comma.ai/shop/obd-c-cable)** — required if one is not already available. Select the comma four variant, not the 3/3X cable.
3. **USB data cable from the Linux replay host to the jungle** — required. Use a known data-capable cable, not a charge-only cable. Confirm the connector shipped with the jungle before buying a duplicate.
4. **Linux replay host** — already satisfied by the gateway/Ubuntu host if it can be physically cabled to the bench. The software sources are the official [panda repository](https://github.com/commaai/panda) and openpilot's `tools/replay/can_replay.py`.

This setup proves physical CAN transport, ignition transitions, the comma4's internal panda path, manager engagement, and process health under replay. It still does not prove a specific vehicle's ECUs or actuators.

## Optional development wiring

- **[Development harness connector](https://comma.ai/shop/harness-connector)** — useful for safe access to the 18-pin harness signals when building custom fixtures. Choose the “Development” variant. It does not include a harness box.
- **[Vehicle-specific car harness](https://comma.ai/shop/car-harness)** — needed only when the Level-3 target is a particular car/ECU topology. A car harness includes the harness box and comma power.
- **[Harness box](https://comma.ai/shop/harness-box)** and **[comma power](https://comma.ai/shop/comma-power)** — buy separately only if not using the complete car-harness kit. Comma says both are included with the car harness.
- Fused, current-limited 12–14 V bench supply, CAN breakout leads, and 120-ohm termination — needed for donor ECUs or a custom vehicle harness, not for the basic jungle replay setup. Select supply current from the donor ECU set; do not guess before choosing the target vehicle.

## Full vehicle/actuator HIL

To test the final vehicle-specific path, first choose the exact make/model/year. Then source the matching harness plus the donor gateway/camera/steering/brake ECUs required by that platform, their connectors/pigtails, wiring diagrams, termination, and a current-limited supply. That bill of materials cannot be safely made generic; it depends on the selected vehicle network and should be designed separately.

## Recommendation

Buy the panda jungle v2 and comma-four OBD-C cable first. They deliver the largest jump in coverage without committing to a donor vehicle. Hold the vehicle harness, donor ECUs, and power supply until a specific Level-3 vehicle target is selected.
