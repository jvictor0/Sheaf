from __future__ import annotations

from sheaf.tools.simple_tool import tool


def test_untyped_parameters_default_to_string_schema() -> None:
    @tool("demo_untyped")
    def demo(foo, bar: int = 1) -> str:  # noqa: ANN001
        return f"{foo}:{bar}"

    schema = demo.parameters_schema
    assert schema["properties"]["foo"]["type"] == "string"
    assert schema["properties"]["bar"]["type"] == "integer"
    assert schema["required"] == ["foo"]
