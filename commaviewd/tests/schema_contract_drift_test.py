from pathlib import Path

from scripts.check_android_schema_drift import build_ignore_candidate, diff_contract, load_contract, load_ignores, parse_schema_tree

FIXTURES = Path(__file__).parent / "fixtures" / "schema_contract"


def contract_fixture():
    return load_contract(
        {
            "version": 1,
            "services": {
                "DeviceState": {
                    "file": "cereal/log.capnp",
                    "fields": {
                        "started": {"ordinal": 0, "type": "Bool"},
                    },
                    "enums": {},
                },
                "ThermalStatusHolder": {
                    "file": "cereal/car.capnp",
                    "fields": {},
                    "enums": {
                        "ThermalStatus": {
                            "green": {"ordinal": 0},
                            "yellow": {"ordinal": 1},
                        }
                    },
                },
            },
        }
    )


def test_additive_field_is_reported_when_unignored():
    report = diff_contract(
        contract_fixture(),
        parse_schema_tree(FIXTURES / "add_field"),
        load_ignores({"version": 1, "ignores": []}),
        label="fixture",
    )

    assert report["unignoredCount"] == 1
    assert {(item["service"], item["symbol"], item["driftClass"]) for item in report["items"]} == {
        ("DeviceState", "engaged", "field-added")
    }


def test_removed_field_is_reported():
    report = diff_contract(
        contract_fixture(),
        parse_schema_tree(FIXTURES / "remove_field"),
        load_ignores({"version": 1, "ignores": []}),
        label="fixture",
    )

    assert (
        "DeviceState",
        "started",
        "field-removed",
    ) in {(item["service"], item["symbol"], item["driftClass"]) for item in report["items"]}


def test_type_change_is_reported():
    report = diff_contract(
        contract_fixture(),
        parse_schema_tree(FIXTURES / "type_change"),
        load_ignores({"version": 1, "ignores": []}),
        label="fixture",
    )

    assert (
        "DeviceState",
        "started",
        "field-type-changed",
    ) in {(item["service"], item["symbol"], item["driftClass"]) for item in report["items"]}


def test_enum_add_is_reported():
    report = diff_contract(
        contract_fixture(),
        parse_schema_tree(FIXTURES / "enum_add"),
        load_ignores({"version": 1, "ignores": []}),
        label="fixture",
    )

    assert (
        "ThermalStatusHolder.ThermalStatus",
        "red",
        "enum-value-added",
    ) in {(item["service"], item["symbol"], item["driftClass"]) for item in report["items"]}


def test_service_add_is_reported():
    report = diff_contract(
        contract_fixture(),
        parse_schema_tree(FIXTURES / "ignore_case"),
        load_ignores({"version": 1, "ignores": []}),
        label="fixture",
    )

    assert (
        "IgnoredThing",
        "IgnoredThing",
        "service-added",
    ) in {(item["service"], item["symbol"], item["driftClass"]) for item in report["items"]}


def test_ignored_drift_is_suppressed():
    report = diff_contract(
        contract_fixture(),
        parse_schema_tree(FIXTURES / "ignore_case"),
        load_ignores(
            {
                "version": 1,
                "ignores": [
                    {
                        "upstream": "fixture",
                        "service": "IgnoredThing",
                        "symbol": "IgnoredThing",
                        "driftClass": "service-added",
                        "reason": "fixture suppression",
                    }
                ],
            }
        ),
        label="fixture",
    )

    assert report["unignoredCount"] == 0
    assert report["items"] == []


def test_source_reordering_keeps_same_normalized_keys():
    base = parse_schema_tree(FIXTURES / "base")
    reordered = parse_schema_tree(FIXTURES / "reordered")

    assert base == reordered


def test_ignore_candidate_is_generated_from_report_items():
    candidate = build_ignore_candidate(
        {
            "label": "sunnypilot",
            "items": [
                {
                    "service": "CarEvent",
                    "symbol": "CarEvent",
                    "driftClass": "service-added",
                    "file": "cereal/car.capnp",
                },
                {
                    "service": "CarState",
                    "symbol": "accEnabled",
                    "driftClass": "field-added",
                    "file": "cereal/car.capnp",
                },
            ],
        }
    )

    assert candidate == {
        "version": 1,
        "generatedFrom": "sunnypilot",
        "notes": "Candidate ignore manifest generated from drift report for review.",
        "ignores": [
            {
                "upstream": "sunnypilot",
                "service": "CarEvent",
                "symbol": "CarEvent",
                "driftClass": "service-added",
                "reason": "bootstrap review candidate from drift report",
            },
            {
                "upstream": "sunnypilot",
                "service": "CarState",
                "symbol": "accEnabled",
                "driftClass": "field-added",
                "reason": "bootstrap review candidate from drift report",
            },
        ],
    }
