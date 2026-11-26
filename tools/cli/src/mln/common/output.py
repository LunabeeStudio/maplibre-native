from collections.abc import Callable
from typing import NoReturn

import typer

from mln.common.errors import MlnError

PREFIX = "mln: "

OnLine = Callable[[str], None]


class ChildLine(str):
    """A line a child process printed (Gradle, CMake, the GLFW app): relayed as is, unprefixed."""


def status(line: str) -> None:
    """The CLI's `on_line`: progress and outcomes go to stderr, stdout stays for data."""
    raw = isinstance(line, ChildLine) or line.startswith((" ", PREFIX))
    typer.echo(line if raw else f"{PREFIX}{line}", err=True)


def fail(message: str) -> NoReturn:
    for line in message.splitlines():
        status(line)
    raise typer.Exit(code=1)


def guarded[T](action: Callable[[], T]) -> T:
    try:
        return action()
    except MlnError as error:
        fail(str(error))
