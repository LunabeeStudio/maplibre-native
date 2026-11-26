from typing import Annotated

import typer

from mln.android import core, system
from mln.common import output, prompt

app = typer.Typer(name="android", help="Build and publish the Android AAR.", no_args_is_help=True)

Flavor = Annotated[str, typer.Option("--flavor", "-f", help="Renderer flavor: vulkan or opengl.")]
Abis = Annotated[
    str | None,
    typer.Option(
        "--abis", "-a", help='ABIs, e.g. "arm64-v8a" or "arm64-v8a,x86_64" (default all).'
    ),
]


@app.command()
def build(
    flavor: Flavor = core.DEFAULT_FLAVOR,
    abis: Abis = None,
    release: Annotated[bool, typer.Option("--release", help="Release build type.")] = False,
) -> None:
    """Build the MapLibreAndroid AAR. Prints its path."""
    path = output.guarded(lambda: system.build(flavor, release, abis, output.status))
    typer.echo(path)


@app.command()
def publish(
    target: Annotated[str, typer.Argument(help="local (~/.m2) or artifactory.")],
    flavor: Flavor = core.DEFAULT_FLAVOR,
    abis: Abis = None,
    yes: Annotated[
        bool, typer.Option("--yes", "-y", help="Publish to Artifactory unasked.")
    ] = False,
) -> None:
    """Publish the AAR through scripts/publish-cumbly.sh. Prints the version to pin in Cumbly."""

    def run() -> str:
        core.check_target(target)
        if target == "artifactory":
            version = system.next_version()
            core.check_publishable(target, version)
            question = f"Publish {core.artifact(flavor)}:{version} to Artifactory?"
            if not prompt.confirm(question, assume_yes=yes):
                raise typer.Exit(code=1)
        return system.publish(target, flavor, abis, output.status)

    typer.echo(output.guarded(run))


@app.command()
def version() -> None:
    """Print the version a local publish of this checkout would get."""
    typer.echo(output.guarded(system.next_version))
