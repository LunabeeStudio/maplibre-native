import os
import sys

import typer

from mln.android import commands as android_commands
from mln.doctor import commands as doctor_commands
from mln.macos import commands as macos_commands

NO_TUI_ENV = "MLN_NO_TUI"

app = typer.Typer(
    help="MapLibre Native dev CLI: Android AAR build and publish, GLFW app on macOS. "
    "Run without arguments in a terminal for the dashboard.",
    add_completion=False,
)
for _sub in (android_commands.app, macos_commands.app):
    app.add_typer(_sub)
app.registered_commands.extend(doctor_commands.app.registered_commands)


@app.callback(invoke_without_command=True)
def main(context: typer.Context) -> None:
    if context.invoked_subcommand is not None:
        return
    if sys.stdin.isatty() and sys.stdout.isatty() and not os.environ.get(NO_TUI_ENV):
        from mln import tui

        tui.run()
    else:
        typer.echo(context.get_help())
