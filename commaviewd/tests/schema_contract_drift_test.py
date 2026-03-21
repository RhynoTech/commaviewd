from pathlib import Path

from scripts.check_android_schema_drift import diff_contract, load_contract, load_ignores, parse_schema_tree

FIXTURES = Path(__file__).parent / "fixtures" / "schema_contract"


def test_additive_field_is_reported_when_unignored():
    contract = load_contract({
        "services": {
            "DeviceState": {
                "file": "cereal/log.capnp",
                "fields": {
                    "started": {"ordinal": 0, "type": "Bool"}
                },
                "enums": {}
            }
        }
    })
    ignores = load_ignores({"ignores": []})
    upstream = parse_schema_tree(FIXTURES / "add_field")

    report = diff_contract(contract, upstream, ignores, label="fixture")

    assert report["unignoredCount"] == 1
    assert report["items"][0]["driftClass"] == "field-added"
