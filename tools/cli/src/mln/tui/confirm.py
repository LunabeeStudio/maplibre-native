from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.screen import ModalScreen
from textual.widgets import Button, Label


class Confirm(ModalScreen[bool]):
    """Yes/No question; dismisses with True on Yes, False on No or Escape. No has the focus."""

    DEFAULT_CSS = """
    Confirm { align: center middle; }
    #confirm { width: 60; height: auto; border: round $primary; background: $surface;
        padding: 0 1; }
    #confirm-buttons { height: auto; }
    """
    BINDINGS = [Binding("escape", "answer_no", "Cancel", show=False)]

    def __init__(self, question: str) -> None:
        super().__init__()
        self._question = question

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm"):
            yield Label(self._question, markup=False)
            with Horizontal(id="confirm-buttons"):
                yield Button("Yes", id="confirm-yes")
                yield Button("No", id="confirm-no")

    def on_mount(self) -> None:
        self.query_one("#confirm-no", Button).focus()

    def action_answer_no(self) -> None:
        self.dismiss(False)

    @on(Button.Pressed, "#confirm-yes")
    def _on_yes(self) -> None:
        self.dismiss(True)

    @on(Button.Pressed, "#confirm-no")
    def _on_no(self) -> None:
        self.dismiss(False)
