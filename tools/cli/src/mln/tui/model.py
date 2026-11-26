from collections.abc import Sequence
from dataclasses import dataclass
from enum import Enum

from mln.android import core as android
from mln.macos import core as macos

NOT_BUILT = "not built"
BUILT = "built"


class Kind(Enum):
    ANDROID = "android"
    MACOS = "macos"


class Action(Enum):
    BUILD = "build"
    RUN = "run"
    STOP = "stop"
    LOGS = "logs"
    PUBLISH_LOCAL = "publish_local"
    PUBLISH_ARTIFACTORY = "publish_artifactory"
    DOCTOR = "doctor"
    QUIT = "quit"


DASHBOARD_ACTIONS = frozenset(action.value for action in Action)

ACTIONS_BY_KIND: dict[Kind, frozenset[Action]] = {
    Kind.ANDROID: frozenset({Action.BUILD, Action.PUBLISH_LOCAL, Action.PUBLISH_ARTIFACTORY}),
    Kind.MACOS: frozenset({Action.BUILD, Action.RUN, Action.STOP, Action.LOGS}),
}
GLOBAL_ACTIONS = frozenset({Action.DOCTOR, Action.QUIT})
# Read-only: allowed while a job runs.
_IDLE_FREE = frozenset({Action.LOGS, Action.QUIT})


@dataclass(frozen=True)
class AndroidState:
    flavor: str
    aar_built: bool
    in_maven_local: bool


@dataclass(frozen=True)
class MacosState:
    backend: str
    built: bool
    running_pid: int | None
    running_label: str | None


@dataclass(frozen=True)
class Target:
    """One row of the Targets table: an Android flavor, or a macOS backend (and Vulkan driver)."""

    kind: Kind
    key: str
    driver: str | None
    running: bool
    cells: tuple[str, str, str, str]

    @property
    def row_key(self) -> str:
        return f"{self.kind.value}:{self.key}:{self.driver or ''}"

    @property
    def dim(self) -> bool:
        return self.cells[1] == NOT_BUILT


def android_targets(states: Sequence[AndroidState], version: str | None) -> list[Target]:
    rows = []
    for state in states:
        note = "in ~/.m2" if state.in_maven_local else "—"
        cells = (
            f"android · {state.flavor}",
            f"{BUILT} (debug)" if state.aar_built else NOT_BUILT,
            f"next {version}" if version else "—",
            note,
        )
        rows.append(Target(Kind.ANDROID, state.flavor, None, False, cells))
    return rows


def macos_targets(states: Sequence[MacosState]) -> list[Target]:
    rows = []
    for state in states:
        backend = macos.backend(state.backend)
        drivers: Sequence[str | None] = (
            tuple(macos.DRIVER_ICDS) if backend.key == "vulkan" else (None,)
        )
        for driver in drivers:
            label = macos.label(backend, driver)
            running = state.running_pid is not None and state.running_label == label
            if running:
                status = f"running (pid {state.running_pid})"
            else:
                status = BUILT if state.built else NOT_BUILT
            note = "V adds Sky + Track" if backend.key == "vulkan" else "—"
            cells = (f"macos · {label}", status, f"build-{backend.preset}", note)
            rows.append(Target(Kind.MACOS, backend.key, driver, running, cells))
    return rows


def action_state(action: str, target: Target | None, busy: bool) -> bool | None:
    """Textual's check_action contract: True shows, False greys out, None hides."""
    try:
        kind = Action(action)
    except ValueError:
        return True
    if busy and kind not in _IDLE_FREE:
        return False
    if kind in GLOBAL_ACTIONS:
        return True
    if target is None or kind not in ACTIONS_BY_KIND[target.kind]:
        return None
    if kind is Action.STOP:
        return target.running
    return True


def publish_question(flavor: str, version: str) -> str:
    return f"Publish {android.artifact(flavor)}:{version} to Artifactory?"


def target_at(targets: Sequence[Target], row: int) -> Target | None:
    return targets[row] if 0 <= row < len(targets) else None


def cursor_row_for(targets: Sequence[Target], row_key: str | None) -> int:
    for index, target in enumerate(targets):
        if target.row_key == row_key:
            return index
    return 0


def output_lines(message: str) -> list[str]:
    return message.splitlines() or [message]
