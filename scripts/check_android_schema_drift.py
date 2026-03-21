"""Scaffolding for structured Android schema drift checks."""


def load_contract(data):
    return data


def load_ignores(data):
    return data


def parse_schema_tree(root):
    return {"root": str(root), "services": {}}


def diff_contract(contract, upstream, ignores, label="upstream"):
    return {
        "label": label,
        "items": [],
        "unignoredCount": 0,
        "contract": contract,
        "upstream": upstream,
        "ignores": ignores,
    }
