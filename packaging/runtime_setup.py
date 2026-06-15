from __future__ import annotations

import os
from pathlib import Path

from setuptools import find_packages, setup
from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel
from setuptools.command.install import install as _install


def _read_version() -> str:
    version_file = Path(__file__).resolve().parents[1] / "VERSION"
    return version_file.read_text(encoding="utf-8").splitlines()[0].strip()


class PlatformInstall(_install):
    def finalize_options(self) -> None:
        super().finalize_options()
        self.install_lib = self.install_platlib


class PlatformWheel(_bdist_wheel):
    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self) -> tuple[str, str, str]:
        _, _, platform_tag = super().get_tag()
        return "py3", "none", platform_tag


class MacOSMetalWheel(PlatformWheel):
    def get_tag(self) -> tuple[str, str, str]:
        _, _, platform_tag = super().get_tag()
        arch = os.environ.get("MACOS_ARCH")
        target = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
        if arch == "arm64" and target:
            major, minor, *_ = target.split(".") + ["0"]
            platform_tag = f"macosx_{major}_{minor}_arm64"
        return "py3", "none", platform_tag


def setup_runtime_package(
    *,
    name: str,
    module: str,
    description: str,
    macos_metal: bool = False,
) -> None:
    setup(
        name=name,
        version=_read_version(),
        description=description,
        python_requires=">=3.9",
        package_dir={"": "python"},
        packages=find_packages(where="python"),
        package_data={
            module: [
                "bin/*",
                "lib/*",
                "metadata/*",
                "metadata/licenses/*",
            ]
        },
        cmdclass={
            "bdist_wheel": MacOSMetalWheel if macos_metal else PlatformWheel,
            "install": PlatformInstall,
        },
    )
