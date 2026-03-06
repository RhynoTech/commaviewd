# Bridge Modularization Baseline (Phase 1 Guardrail)

Date: 2026-03-06
Source: `bridge/cpp/commaview-bridge.cc` (monolith baseline)

## Frozen runtime contract
- Ports: `8200` (road), `8201` (wide), `8202` (driver)
- Frame types: `MSG_VIDEO=0x01`, `MSG_META=0x02`, `MSG_CONTROL=0x03`
- Keep current CLI/launch behavior and stream routing unchanged
- No behavior changes allowed in Phase 1 refactor

## Function extraction map

### Net
- `create_server`
- `send_all`, `send_frame`, `send_meta_json`
- `client_socket_alive`
- `accept_loop`

### Control
- `extract_json_string_field`, `extract_json_bool_field`
- `parse_set_policy_control`
- `consume_client_control_frames`
- `set_session_policy`, `get_session_policy`

### Video
- `read_encode_data`
- video path in `handle_client`
- per-port validation helpers (`expected_video_which_for_port`)

### Telemetry
- `build_car_state_json`
- `build_selfdrive_state_json`
- `build_device_state_json`
- `build_model_v2_json`
- `build_radar_state_json`
- `build_live_calibration_json`
- `build_telemetry_json`
- service/queue helpers

### Main wiring
- `sig_handler`
- `main`

## Verification gate for COM-44 completion
- Plan + boundaries documented
- Baseline mapping captured
- COM-45..49 can execute against frozen boundaries
