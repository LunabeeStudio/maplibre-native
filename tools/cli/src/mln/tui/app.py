from collections.abc import Callable, Sequence
from typing import Any

from rich.text import Text
from textual import on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.screen import ModalScreen
from textual.widgets import DataTable, Footer, RichLog, Static
from textual.worker import get_current_worker

from mln.android import core as android_core
from mln.android import system as android
from mln.common import proc, repo
from mln.common.errors import MlnError
from mln.common.output import PREFIX, OnLine
from mln.doctor import commands as doctor
from mln.macos import core as macos_core
from mln.macos import system as macos
from mln.tui import model
from mln.tui.confirm import Confirm
from mln.tui.model import Action, Kind, Target

REFRESH_SECONDS = 2
OUTPUT_MAX_LINES = 5000
FOLLOW_GROUP = "follow"
RUN_GROUP = "run"


def _describe(error: Exception) -> str:
    """MlnError messages are written for the user; any other exception may carry a secret."""
    return str(error) if isinstance(error, MlnError) else type(error).__name__


class DashboardApp(App[None]):
    TITLE = "mln"
    CSS = """
    #context { height: 1; padding: 0 1; color: $text-muted; }
    #targets { height: auto; max-height: 12; border: round $primary; }
    #output { border: round $primary; }
    """
    BINDINGS = [
        Binding("b", Action.BUILD.value, "Build"),
        Binding("r", Action.RUN.value, "Run"),
        Binding("s", Action.STOP.value, "Stop"),
        Binding("l", Action.LOGS.value, "Logs"),
        Binding("p", Action.PUBLISH_LOCAL.value, "Publish local"),
        Binding("P", Action.PUBLISH_ARTIFACTORY.value, "Publish Artifactory"),
        Binding("D", Action.DOCTOR.value, "Doctor"),
        Binding("q", Action.QUIT.value, "Quit"),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._targets: list[Target] = []
        self._selected: Target | None = None
        self._cursor_key: str | None = None
        self._busy = False
        self._poll_error: str | None = None

    def compose(self) -> ComposeResult:
        yield Static("", id="context", markup=False)
        yield DataTable(id="targets", cursor_type="row")
        yield RichLog(id="output", max_lines=OUTPUT_MAX_LINES, wrap=True, markup=False)
        yield Footer()

    def on_mount(self) -> None:
        branch = repo.branch() or "(detached)"
        self.query_one("#context", Static).update(f"{repo.REPO_ROOT}  {branch}")
        table = self.query_one("#targets", DataTable)
        table.border_title = "Targets"
        table.add_columns("Target", "State", "Detail", "Note")
        self.query_one("#output", RichLog).border_title = "Output"
        self.poll_targets()
        self.set_interval(REFRESH_SECONDS, self.poll_targets)

    def check_action(self, action: str, parameters: tuple[object, ...]) -> bool | None:
        if isinstance(self.screen, ModalScreen) and action in model.DASHBOARD_ACTIONS:
            return None
        return model.action_state(action, self._selected, self._busy)

    # Actions

    def action_build(self) -> None:
        target = self._selected
        if target is None:
            return
        if target.kind is Kind.ANDROID:
            self._start(
                f"build android {target.key} (debug, all ABIs)",
                lambda on_line: on_line(f"aar → {android.build(target.key, False, None, on_line)}"),
            )
        else:
            self._start(
                f"build macos {target.key}",
                lambda on_line: on_line(f"binary → {macos.build(target.key, on_line)}"),
            )

    def action_run(self) -> None:
        target = self._selected
        if target is None or target.kind is not Kind.MACOS:
            return
        driver = target.driver or macos_core.DEFAULT_DRIVER
        self._start(
            f"run {target.cells[0]}",
            lambda on_line: macos.start(target.key, driver, None, True, on_line),
            then=lambda _: self._follow_log(target.key),
        )

    def action_stop(self) -> None:
        target = self._selected
        if target is not None and target.kind is Kind.MACOS:
            self._start(f"stop {target.cells[0]}", lambda on_line: macos.stop(target.key, on_line))

    def action_logs(self) -> None:
        target = self._selected
        if target is not None and target.kind is Kind.MACOS:
            self._follow_log(target.key)

    def action_publish_local(self) -> None:
        target = self._selected
        if target is not None and target.kind is Kind.ANDROID:
            self._publish(target.key, "local")

    def action_publish_artifactory(self) -> None:
        target = self._selected
        if target is None or target.kind is not Kind.ANDROID:
            return
        flavor = target.key

        def ask(version: str) -> None:
            def answered(confirmed: bool | None) -> None:
                if confirmed:
                    self._publish(flavor, "artifactory")

            self.push_screen(Confirm(model.publish_question(flavor, version)), answered)

        def check(on_line: OnLine) -> str:
            version = android.next_version()
            android_core.check_publishable("artifactory", version)
            return version

        self._start("check the version to publish", check, then=ask)

    def _publish(self, flavor: str, destination: str) -> None:
        self._start(
            f"publish android {flavor} to {destination}",
            lambda on_line: on_line(
                f'pin in Cumbly: maplibre = "{android.publish(destination, flavor, None, on_line)}"'
            ),
        )

    def action_doctor(self) -> None:
        def check(on_line: OnLine) -> None:
            if doctor.run(False, on_line) != 0:
                raise MlnError("vulkan-doctor found missing tools: mln doctor --fix installs them")

        self._start("doctor", check)

    def _follow_log(self, backend_key: str) -> None:
        path = macos.log_path(backend_key)
        self.workers.cancel_group(self, FOLLOW_GROUP)
        self._write(f"── {path.name}")
        self.follow(path)

    # Workers

    def _start(
        self,
        title: str,
        job: Callable[[OnLine], Any],
        then: Callable[[Any], None] | None = None,
    ) -> None:
        self.workers.cancel_group(self, FOLLOW_GROUP)
        self._set_running(True)
        self._write(f"── {title}")
        self.run_job(job, then)

    @work(thread=True, exclusive=True, group=RUN_GROUP, exit_on_error=False)
    def run_job(self, job: Callable[[OnLine], Any], then: Callable[[Any], None] | None) -> None:
        try:
            result = job(self._write_threadsafe)
        except Exception as error:
            for line in model.output_lines(_describe(error)):
                self._write_threadsafe(line if line.startswith(" ") else f"{PREFIX}{line}")
            self.call_from_thread(self._set_running, False)
        else:
            self.call_from_thread(self._set_running, False)
            if then is not None:
                self.call_from_thread(then, result)

    @work(thread=True, exclusive=True, group=FOLLOW_GROUP, exit_on_error=False)
    def follow(self, path: Any) -> None:
        worker = get_current_worker()
        if not path.exists():
            self._write_threadsafe(f"{PREFIX}no log yet: r runs the app")
            return
        for line in proc.follow(path, lambda: worker.is_cancelled):
            self._write_threadsafe(line)

    @work(thread=True, exclusive=True, group="targets", exit_on_error=False)
    def poll_targets(self) -> None:
        try:
            targets = _read_targets()
        except Exception as error:
            message = f"{PREFIX}{_describe(error)}"
            if message != self._poll_error:
                self._poll_error = message
                self._write_threadsafe(message)
            return
        self._poll_error = None
        self.call_from_thread(self._show_targets, targets)

    # Table

    @on(DataTable.RowHighlighted)
    def _on_row_highlighted(self) -> None:
        self._sync_selection()

    def _show_targets(self, targets: Sequence[Target]) -> None:
        self._targets = list(targets)
        table = self.query_one("#targets", DataTable)
        table.clear()
        for target in self._targets:
            style = "dim" if target.dim else ""
            table.add_row(*(Text(cell, style=style) for cell in target.cells), key=target.row_key)
        table.move_cursor(row=model.cursor_row_for(self._targets, self._cursor_key))
        self._sync_selection()

    def _sync_selection(self) -> None:
        row = self.query_one("#targets", DataTable).cursor_row
        self._selected = model.target_at(self._targets, row)
        self._cursor_key = self._selected.row_key if self._selected else None
        self.refresh_bindings()

    def _set_running(self, running: bool) -> None:
        self._busy = running
        self.refresh_bindings()

    def _write(self, line: str) -> None:
        self.query_one("#output", RichLog).write(Text(line))

    def _write_threadsafe(self, line: str) -> None:
        self.call_from_thread(self._write, line)


def _read_targets() -> list[Target]:
    try:
        version: str | None = android.next_version()
    except MlnError:
        version = None
    android_states = [
        model.AndroidState(
            flavor,
            android.aar(flavor, False).is_file(),
            version is not None and android.in_maven_local(flavor, version),
        )
        for flavor in android_core.FLAVORS
    ]
    macos_states = []
    for key in macos_core.BACKENDS:
        running = macos.running(key)
        macos_states.append(
            model.MacosState(
                key,
                macos.binary(key).is_file(),
                running[0] if running else None,
                running[1] if running else None,
            )
        )
    return [*model.android_targets(android_states, version), *model.macos_targets(macos_states)]
