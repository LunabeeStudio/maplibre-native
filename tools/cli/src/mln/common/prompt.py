import sys

import typer

from mln.common.errors import MlnError


def interactive() -> bool:
    return sys.stdin.isatty() and sys.stderr.isatty()


def confirm(question: str, *, assume_yes: bool) -> bool:
    """`--yes` answers yes; a terminal is asked (default no); no terminal refuses."""
    if assume_yes:
        return True
    if not interactive():
        raise MlnError(f"{question} — no terminal to ask: pass --yes")
    return typer.confirm(question, default=False, err=True)
