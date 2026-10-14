"""Fail-closed JSON Schema Draft 2020-12 subset used by tracked P0 schemas.

The project cannot assume that the third-party ``jsonschema`` Python package is
installed in every CMake-selected interpreter. This module implements exactly
the assertion and applicator keywords used by the editor parity schemas and
rejects unknown keywords instead of silently ignoring them.
"""

from __future__ import annotations

import math
import re
from datetime import datetime
from typing import Any


DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"
ANNOTATION_KEYWORDS = {"$schema", "$id", "$comment", "title", "description"}
APPLICATOR_KEYWORDS = {
    "$ref",
    "$defs",
    "allOf",
    "oneOf",
    "not",
    "if",
    "then",
    "else",
    "properties",
    "additionalProperties",
    "propertyNames",
    "items",
    "contains",
}
ASSERTION_KEYWORDS = {
    "type",
    "const",
    "enum",
    "required",
    "minProperties",
    "maxProperties",
    "minItems",
    "maxItems",
    "uniqueItems",
    "minContains",
    "minLength",
    "pattern",
    "format",
    "minimum",
}
SUPPORTED_KEYWORDS = ANNOTATION_KEYWORDS | APPLICATOR_KEYWORDS | ASSERTION_KEYWORDS
JSON_TYPES = {"object", "array", "string", "integer", "number", "boolean", "null"}
SUPPORTED_FORMATS = {"date-time"}
NON_NEGATIVE_INTEGER_KEYWORDS = {
    "minProperties",
    "maxProperties",
    "minItems",
    "maxItems",
    "minContains",
    "minLength",
}
_DATE_TIME_RE = re.compile(
    r"^(?P<year>\d{4})-(?P<month>\d{2})-(?P<day>\d{2})[Tt]"
    r"(?P<hour>\d{2}):(?P<minute>\d{2}):(?P<second>\d{2})(?:\.\d+)?"
    r"(?P<offset>[Zz]|[+-]\d{2}:\d{2})$"
)


class JsonSchemaError(ValueError):
    """A schema-definition or instance-validation failure with a stable path."""

    def __init__(self, path: str, message: str) -> None:
        self.path = path
        self.message = message
        super().__init__(f"{path}: {message}")

    @property
    def depth(self) -> int:
        return self.path.count(".") + self.path.count("[")


def _fail(path: str, message: str) -> None:
    raise JsonSchemaError(path, message)


def _json_equal(left: Any, right: Any) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return isinstance(left, bool) and isinstance(right, bool) and left == right
    if left is None or right is None:
        return left is None and right is None
    if (isinstance(left, (int, float)) and not isinstance(left, bool) and
            isinstance(right, (int, float)) and not isinstance(right, bool)):
        return left == right
    if isinstance(left, str) or isinstance(right, str):
        return isinstance(left, str) and isinstance(right, str) and left == right
    if isinstance(left, list) or isinstance(right, list):
        return (isinstance(left, list) and isinstance(right, list) and
                len(left) == len(right) and
                all(_json_equal(a, b) for a, b in zip(left, right)))
    if isinstance(left, dict) or isinstance(right, dict):
        return (isinstance(left, dict) and isinstance(right, dict) and
                set(left) == set(right) and
                all(_json_equal(left[key], right[key]) for key in left))
    return type(left) is type(right) and left == right


def _is_finite_number(value: Any) -> bool:
    return ((isinstance(value, int) and not isinstance(value, bool)) or
            (isinstance(value, float) and math.isfinite(value)))


def _resolve_ref(root_schema: dict[str, Any], reference: Any, path: str) -> Any:
    if not isinstance(reference, str):
        _fail(path, "$ref must be a string")
    if not reference.startswith("#/"):
        _fail(path, f"only local JSON Pointer references are supported, got {reference!r}")
    value: Any = root_schema
    for raw_token in reference[2:].split("/"):
        token = raw_token.replace("~1", "/").replace("~0", "~")
        if not isinstance(value, dict) or token not in value:
            _fail(path, f"unresolved schema reference {reference!r}")
        value = value[token]
    return value


def _schema_child_path(path: str, keyword: str, name: str | int | None = None) -> str:
    result = f"{path}.{keyword}"
    if name is not None:
        result += f"[{name!r}]"
    return result


def _check_schema(
    schema: Any,
    root_schema: dict[str, Any],
    path: str,
    checking: set[int],
    checked: set[int],
) -> None:
    if isinstance(schema, bool):
        return
    if not isinstance(schema, dict):
        _fail(path, "schema must be an object or boolean")

    identity = id(schema)
    if identity in checked or identity in checking:
        return
    checking.add(identity)
    try:
        unknown = set(schema) - SUPPORTED_KEYWORDS
        if unknown:
            _fail(path, f"unsupported schema keywords: {sorted(unknown)}")

        for keyword in ("$schema", "$id", "$comment", "title", "description"):
            if keyword in schema and not isinstance(schema[keyword], str):
                _fail(path, f"{keyword} must be a string")
        if "$schema" in schema and schema["$schema"] != DRAFT_2020_12:
            _fail(path, f"unsupported JSON Schema dialect {schema['$schema']!r}")

        if "$ref" in schema:
            referenced = _resolve_ref(root_schema, schema["$ref"], path)
            _check_schema(referenced, root_schema, f"{path}.$ref", checking, checked)

        for keyword in ("properties", "$defs"):
            if keyword not in schema:
                continue
            value = schema[keyword]
            if not isinstance(value, dict):
                _fail(path, f"{keyword} must be an object")
            for name, child in value.items():
                _check_schema(
                    child, root_schema, _schema_child_path(path, keyword, name), checking, checked)

        for keyword in ("allOf", "oneOf"):
            if keyword not in schema:
                continue
            value = schema[keyword]
            if not isinstance(value, list) or not value:
                _fail(path, f"{keyword} must be a non-empty array")
            for index, child in enumerate(value):
                _check_schema(
                    child, root_schema, _schema_child_path(path, keyword, index), checking, checked)

        for keyword in ("not", "if", "then", "else", "propertyNames", "items", "contains"):
            if keyword in schema:
                _check_schema(
                    schema[keyword], root_schema, _schema_child_path(path, keyword), checking, checked)

        if "additionalProperties" in schema:
            _check_schema(
                schema["additionalProperties"], root_schema,
                _schema_child_path(path, "additionalProperties"), checking, checked)

        if "type" in schema:
            value = schema["type"]
            values = value if isinstance(value, list) else [value]
            if (not values or not all(isinstance(item, str) for item in values) or
                    len(values) != len(set(values)) or not set(values).issubset(JSON_TYPES)):
                _fail(path, "type must be a supported string or non-empty unique string array")

        if "enum" in schema:
            value = schema["enum"]
            if not isinstance(value, list) or not value:
                _fail(path, "enum must be a non-empty array")
            for index, item in enumerate(value):
                if any(_json_equal(item, prior) for prior in value[:index]):
                    _fail(path, "enum values must be unique")

        if "required" in schema:
            value = schema["required"]
            if (not isinstance(value, list) or
                    not all(isinstance(item, str) for item in value) or
                    len(value) != len(set(value))):
                _fail(path, "required must be a unique string array")

        for keyword in NON_NEGATIVE_INTEGER_KEYWORDS:
            if keyword in schema:
                value = schema[keyword]
                if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                    _fail(path, f"{keyword} must be a non-negative integer")

        if "uniqueItems" in schema and not isinstance(schema["uniqueItems"], bool):
            _fail(path, "uniqueItems must be a boolean")
        if "minimum" in schema:
            value = schema["minimum"]
            if not _is_finite_number(value):
                _fail(path, "minimum must be a finite number")
        if "pattern" in schema:
            value = schema["pattern"]
            if not isinstance(value, str):
                _fail(path, "pattern must be a string")
            try:
                re.compile(value)
            except re.error as exc:
                _fail(path, f"pattern is invalid: {exc}")
        if "format" in schema:
            value = schema["format"]
            if not isinstance(value, str):
                _fail(path, "format must be a string")
            if value not in SUPPORTED_FORMATS:
                _fail(path, f"unsupported format {value!r}")
    finally:
        checking.remove(identity)
    checked.add(identity)


def check_schema_supported(schema: Any) -> None:
    if not isinstance(schema, dict):
        _fail("$schema", "root schema must be an object")
    if schema.get("$schema") != DRAFT_2020_12:
        _fail("$schema", f"root schema must declare Draft 2020-12 as {DRAFT_2020_12!r}")
    _check_schema(schema, schema, "$schema", set(), set())


def _matches_type(instance: Any, type_name: str) -> bool:
    if type_name == "object":
        return isinstance(instance, dict)
    if type_name == "array":
        return isinstance(instance, list)
    if type_name == "string":
        return isinstance(instance, str)
    if type_name == "integer":
        return (isinstance(instance, int) and not isinstance(instance, bool)) or (
            isinstance(instance, float) and math.isfinite(instance) and instance.is_integer())
    if type_name == "number":
        return _is_finite_number(instance)
    if type_name == "boolean":
        return isinstance(instance, bool)
    if type_name == "null":
        return instance is None
    return False


def _property_path(path: str, name: str) -> str:
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        return f"{path}.{name}"
    return f"{path}[{name!r}]"


def _validation_error(
    instance: Any,
    schema: Any,
    root_schema: dict[str, Any],
    path: str,
    active: set[tuple[int, int]],
) -> JsonSchemaError | None:
    try:
        _validate(instance, schema, root_schema, path, active)
    except JsonSchemaError as exc:
        return exc
    return None


def _validate_date_time(instance: str, path: str) -> None:
    if re.search(r"(?:[Zz]|[+-]\d{2}:\d{2})$", instance) is None:
        _fail(path, "date-time must include an explicit UTC offset")
    match = _DATE_TIME_RE.fullmatch(instance)
    if match is None:
        _fail(path, f"invalid date-time {instance!r}")
    try:
        datetime(int(match["year"]), int(match["month"]), int(match["day"]))
    except ValueError as exc:
        raise JsonSchemaError(path, f"invalid date-time {instance!r}") from exc
    if (int(match["hour"]) > 23 or int(match["minute"]) > 59 or
            int(match["second"]) > 60):
        _fail(path, f"invalid date-time {instance!r}")
    offset = match["offset"]
    if offset not in {"Z", "z"} and (int(offset[1:3]) > 23 or int(offset[4:6]) > 59):
        _fail(path, f"invalid date-time {instance!r}")


def _validate(
    instance: Any,
    schema: Any,
    root_schema: dict[str, Any],
    path: str,
    active: set[tuple[int, int]],
) -> None:
    if schema is True:
        return
    if schema is False:
        _fail(path, "boolean schema rejected the value")

    pair = (id(schema), id(instance))
    if pair in active:
        _fail(path, "cyclic schema reference is outside the supported subset")
    active.add(pair)
    try:
        if "$ref" in schema:
            _validate(instance, _resolve_ref(root_schema, schema["$ref"], path), root_schema, path, active)

        for child in schema.get("allOf", []):
            _validate(instance, child, root_schema, path, active)

        if "oneOf" in schema:
            errors: list[JsonSchemaError] = []
            matches = 0
            for child in schema["oneOf"]:
                error = _validation_error(instance, child, root_schema, path, active)
                if error is None:
                    matches += 1
                else:
                    errors.append(error)
            if matches != 1:
                if matches == 0 and errors:
                    closest = max(errors, key=lambda error: (error.depth, len(str(error))))
                    _fail(
                        path,
                        "expected exactly one matching schema in oneOf, matched 0; "
                        f"closest rejection: {closest}",
                    )
                _fail(path, f"expected exactly one matching schema in oneOf, matched {matches}")

        if "not" in schema:
            if _validation_error(instance, schema["not"], root_schema, path, active) is None:
                _fail(path, "value matches the prohibited schema in not")

        if "if" in schema:
            condition_matches = _validation_error(instance, schema["if"], root_schema, path, active) is None
            branch = "then" if condition_matches else "else"
            if branch in schema:
                _validate(instance, schema[branch], root_schema, path, active)

        if "type" in schema:
            expected = schema["type"]
            expected_types = expected if isinstance(expected, list) else [expected]
            if not any(_matches_type(instance, item) for item in expected_types):
                _fail(path, f"expected type {expected_types}, got {type(instance).__name__}")

        if "const" in schema and not _json_equal(instance, schema["const"]):
            _fail(path, f"expected constant {schema['const']!r}")
        if "enum" in schema and not any(_json_equal(instance, item) for item in schema["enum"]):
            _fail(path, f"value {instance!r} is not in the enum")

        if isinstance(instance, dict):
            properties = schema.get("properties", {})
            for required_name in schema.get("required", []):
                if required_name not in instance:
                    _fail(path, f"missing required property {required_name!r}")
            if len(instance) < schema.get("minProperties", 0):
                _fail(path, f"object has fewer than {schema['minProperties']} properties")
            if "maxProperties" in schema and len(instance) > schema["maxProperties"]:
                _fail(path, f"object has more than {schema['maxProperties']} properties")

            if "propertyNames" in schema:
                for name in instance:
                    _validate(name, schema["propertyNames"], root_schema, f"{path} property name {name!r}", active)

            for name, child_schema in properties.items():
                if name in instance:
                    _validate(instance[name], child_schema, root_schema, _property_path(path, name), active)

            additional = schema.get("additionalProperties", True)
            extra = [name for name in instance if name not in properties]
            if additional is False and extra:
                _fail(path, f"unexpected properties: {sorted(extra)}")
            if isinstance(additional, dict):
                for name in extra:
                    _validate(instance[name], additional, root_schema, _property_path(path, name), active)

        if isinstance(instance, list):
            if len(instance) < schema.get("minItems", 0):
                _fail(path, f"array has fewer than {schema['minItems']} items")
            if "maxItems" in schema and len(instance) > schema["maxItems"]:
                _fail(path, f"array has more than {schema['maxItems']} items")
            if schema.get("uniqueItems"):
                for index, item in enumerate(instance):
                    if any(_json_equal(item, prior) for prior in instance[:index]):
                        _fail(path, f"array items are not unique (duplicate at index {index})")
            if "items" in schema:
                for index, value in enumerate(instance):
                    _validate(value, schema["items"], root_schema, f"{path}[{index}]", active)
            if "contains" in schema:
                matches = sum(
                    _validation_error(value, schema["contains"], root_schema, f"{path}[{index}]", active) is None
                    for index, value in enumerate(instance)
                )
                minimum = schema.get("minContains", 1)
                if matches < minimum:
                    _fail(path, f"array contains {matches} matching items, fewer than minContains {minimum}")

        if isinstance(instance, str):
            if len(instance) < schema.get("minLength", 0):
                _fail(path, f"string is shorter than {schema['minLength']}")
            if "pattern" in schema and re.search(schema["pattern"], instance) is None:
                _fail(path, f"string does not match {schema['pattern']!r}")
            if schema.get("format") == "date-time":
                _validate_date_time(instance, path)

        if isinstance(instance, (int, float)) and not isinstance(instance, bool):
            if "minimum" in schema and instance < schema["minimum"]:
                _fail(path, f"number is below minimum {schema['minimum']}")
    finally:
        active.remove(pair)


class JsonSchemaValidator:
    """Checked, reusable validator for the supported Draft 2020-12 subset."""

    def __init__(self, schema: Any) -> None:
        check_schema_supported(schema)
        self.schema = schema

    def validate(self, instance: Any) -> None:
        _validate(instance, self.schema, self.schema, "$", set())


def validate_json_schema(instance: Any, schema: Any) -> None:
    JsonSchemaValidator(schema).validate(instance)
