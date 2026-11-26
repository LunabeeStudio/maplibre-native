import os
from typing import Annotated

import typer

from mln.common import output, proc, repo

app = typer.Typer()

SCRIPT = repo.REPO_ROOT / "scripts" / "vulkan-doctor.sh"


def run(fix: bool, on_line: output.OnLine) -> int:
    command = [str(SCRIPT), *(["--fix"] if fix else [])]
    return proc.stream(command, cwd=repo.REPO_ROOT, env=dict(os.environ), on_line=on_line)


@app.command()
def doctor(
    fix: Annotated[bool, typer.Option("--fix", help="Install what can be scripted.")] = False,
) -> None:
    """List the missing host tools (scripts/vulkan-doctor.sh)."""
    code = output.guarded(lambda: run(fix, output.status))
    raise typer.Exit(code=code)
