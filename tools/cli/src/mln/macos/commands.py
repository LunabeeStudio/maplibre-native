from typing import Annotated

import typer

from mln.common import output
from mln.common.errors import MlnError
from mln.macos import core, system

app = typer.Typer(name="macos", help="Build and run the GLFW app on macOS.", no_args_is_help=True)

BackendOption = Annotated[
    str,
    typer.Option("--backend", "-b", help="Renderer: vulkan (CMake preset macos-vulkan) or metal."),
]


@app.command()
def build(backend: BackendOption = core.DEFAULT_BACKEND) -> None:
    """Configure (first time) and build mbgl-glfw. Prints the binary path."""
    typer.echo(output.guarded(lambda: system.build(backend, output.status)))


@app.command(context_settings={"allow_extra_args": True, "ignore_unknown_options": True})
def run(
    context: typer.Context,
    backend: BackendOption = core.DEFAULT_BACKEND,
    driver: Annotated[
        str, typer.Option("--driver", help="Vulkan driver: kosmickrisp or moltenvk.")
    ] = core.DEFAULT_DRIVER,
    style: Annotated[str | None, typer.Option("--style", "-s", help="Style URL.")] = None,
    no_build: Annotated[bool, typer.Option("--no-build", help="Run the last build as is.")] = False,
) -> None:
    """Build, then run mbgl-glfw until its window closes. Extra arguments go to the app."""

    def go() -> None:
        code = system.run_foreground(
            backend, driver, style, context.args, not no_build, output.status
        )
        if code != 0:
            raise MlnError(f"{core.GLFW_TARGET} exited {code}")

    output.guarded(go)


@app.command()
def stop(backend: BackendOption = core.DEFAULT_BACKEND) -> None:
    """Stop the GLFW app the dashboard started."""
    output.guarded(lambda: system.stop(backend, output.status))
