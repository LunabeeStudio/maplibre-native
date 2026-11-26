from mln.tui import model
from mln.tui.model import Action, Kind


def _macos(running_label=None):
    states = [
        model.MacosState("vulkan", True, 42 if running_label else None, running_label),
        model.MacosState("metal", False, None, None),
    ]
    return model.macos_targets(states)


def test_macos_rows_one_per_vulkan_driver_plus_metal():
    rows = _macos()
    assert [row.cells[0] for row in rows] == [
        "macos · vulkan · kosmickrisp",
        "macos · vulkan · moltenvk",
        "macos · metal",
    ]
    assert rows[2].dim and not rows[0].dim


def test_only_the_running_driver_row_shows_running():
    rows = _macos("vulkan · moltenvk")
    assert rows[1].running and rows[1].cells[1] == "running (pid 42)"
    assert not rows[0].running and rows[0].cells[1] == "built"


def test_android_rows():
    states = [model.AndroidState("vulkan", True, True), model.AndroidState("opengl", False, False)]
    rows = model.android_targets(states, "1.0-cumbly-1-abc")
    assert rows[0].cells == (
        "android · vulkan",
        "built (debug)",
        "next 1.0-cumbly-1-abc",
        "in ~/.m2",
    )
    assert rows[1].cells[1] == "not built"
    assert model.android_targets(states, None)[0].cells[2] == "—"


def test_action_state():
    android, macos_row = (
        model.android_targets([model.AndroidState("vulkan", False, False)], None)[0],
        _macos()[0],
    )
    assert model.action_state(Action.PUBLISH_LOCAL.value, android, False) is True
    assert model.action_state(Action.RUN.value, android, False) is None
    assert model.action_state(Action.RUN.value, macos_row, False) is True
    assert model.action_state(Action.STOP.value, macos_row, False) is False
    assert model.action_state(Action.STOP.value, _macos("vulkan · kosmickrisp")[0], False) is True
    assert model.action_state(Action.BUILD.value, macos_row, True) is False
    assert model.action_state(Action.LOGS.value, macos_row, True) is True
    assert model.action_state(Action.DOCTOR.value, None, False) is True
    assert model.action_state(Action.BUILD.value, None, False) is None


def test_cursor_and_selection():
    rows = _macos()
    assert model.cursor_row_for(rows, rows[1].row_key) == 1
    assert model.cursor_row_for(rows, "gone") == 0
    assert model.target_at(rows, 5) is None
    assert model.target_at(rows, 0).kind is Kind.MACOS
