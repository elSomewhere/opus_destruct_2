#!/usr/bin/env python3
"""The engine's structure, checked (docs/CORE.md §1).

  check_engine.py layers           every library includes only itself and what it is built on
                                   (CMakeLists.txt), another library only through its public
                                   headers, and a third-party header only from its sources
  check_engine.py libm BUILD_DIR   no library calls the platform's transcendental maths (sin,
                                   exp, pow, cbrt...: they differ in the last bit between libms);
                                   the deterministic ones are svx/base/dmath.hpp's

Exit status 1 with a list of what breaks a rule; 0 when there is none.
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# library -> (its directory, its public include directories, its private include directories)
LIBRARIES = {
    "core": ("core", ["core/include"], []),
    "mesh": ("mesh", ["mesh/include"], []),
    "env": ("env", ["env/include"], []),
    "anim": ("anim", ["anim/include"], ["third_party/nlohmann"]),
    "city": ("level/city", ["level/city/include"], ["level/city/src"]),
    "api": ("game/src/api", [], []),
    "game": ("game", ["game/include"], []),
    "procgen": ("procgen", ["procgen/include"], ["third_party/nlohmann"]),
}
# what each is built on (target_link_libraries, with what that brings)
DEPENDS = {
    "core": set(),
    "mesh": {"core"},
    "env": {"core"},
    "anim": {"core"},
    "city": {"core"},
    "game": {"core", "mesh", "env", "anim"},
    "procgen": {"game", "city", "core", "mesh", "env", "anim"},
    "api": {"game", "procgen", "city", "core", "mesh", "env", "anim"},
}
# headers written by the build (cmake/embed_data.cmake, embed_presets.cmake)
GENERATED = {
    "svx/anim/content_data.hpp": "anim",
    "svx/data.hpp": "city",
    "svx/procgen/presets.hpp": "procgen",
}
SOURCES = (".cpp", ".hpp", ".h", ".inc")
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def owner(path: pathlib.Path):
    rel = path.relative_to(ROOT).as_posix()
    if rel.startswith("third_party/"):
        return "third_party"
    best = None
    for name, (directory, _, _) in LIBRARIES.items():
        if rel.startswith(directory + "/") and (best is None or len(directory) > len(LIBRARIES[best][0])):
            best = name
    return best


def public(path: pathlib.Path, library: str) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    return any(rel.startswith(d + "/") for d in LIBRARIES[library][1])


def resolve(including: pathlib.Path, name: str, library: str):
    local = (including.parent / name).resolve()
    if local.is_file():
        return local
    search = [d for lib in DEPENDS[library] | {library} for d in LIBRARIES[lib][1]]
    search += LIBRARIES[library][2]
    # (every library's, last: an include of one not depended on resolves, and is reported)
    search += [d for lib in LIBRARIES for d in LIBRARIES[lib][1] + LIBRARIES[lib][2]]
    for directory in search:
        candidate = ROOT / directory / name
        if candidate.is_file():
            return candidate.resolve()
    return None


def check_layers() -> list[str]:
    problems = []
    for library, (directory, _, _) in LIBRARIES.items():
        for path in sorted((ROOT / directory).rglob("*")):
            if path.suffix not in SOURCES or owner(path) != library:
                continue
            text = path.read_text(errors="replace")
            for match in INCLUDE.finditer(text):
                name = match.group(1)
                line = text.count("\n", 0, match.start()) + 1
                where = f"{path.relative_to(ROOT)}:{line}: {library} includes \"{name}\""
                if name in GENERATED:
                    other = GENERATED[name]
                    if other != library and other not in DEPENDS[library]:
                        problems.append(f"{where} - {other}'s, which {library} is not built on")
                    continue
                target = resolve(path, name, library)
                if target is None:
                    problems.append(f"{where} - not found")
                    continue
                other = owner(target)
                if other == library:
                    continue
                if other == "third_party":
                    if public(path, library):
                        problems.append(f"{where} - a third-party header in a public header")
                    elif not any(target.is_relative_to(ROOT / d) for d in LIBRARIES[library][2]):
                        problems.append(f"{where} - a third-party header {library} does not declare")
                    continue
                if other is None:
                    problems.append(f"{where} - outside every library ({target.relative_to(ROOT)})")
                elif other not in DEPENDS[library]:
                    problems.append(f"{where} - {other}'s, which {library} is not built on")
                elif not public(target, other):
                    problems.append(f"{where} - {other}'s private header {target.relative_to(ROOT)}")
    return problems


# the platform maths that is not correctly rounded (sqrt, floor, fmod... are exact everywhere)
TRANSCENDENTAL = {
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "asinh", "acosh",
    "atanh", "exp", "exp2", "exp10", "expm1", "log", "log2", "log10", "log1p", "pow", "cbrt", "hypot",
    "erf", "erfc", "tgamma", "lgamma", "sincos", "sincos_stret",
}
UNDEFINED = re.compile(r"^(?:(?P<archive>[^:\s]+):)?(?P<member>[^:\s]+):\s+U\s+(?P<symbol>\S+)$")


def libm_name(symbol: str):
    name = symbol.split("@", 1)[0].lstrip("_")
    finite = re.fullmatch(r"(\w+)_finite", name)
    if finite:
        name = finite.group(1)
    if name in TRANSCENDENTAL:
        return name
    if name[-1:] in ("f", "l") and name[:-1] in TRANSCENDENTAL:
        return name[:-1]
    return None


def check_libm(build: pathlib.Path, nm: str) -> list[str]:
    problems = []
    archives = {}
    for library in LIBRARIES:
        found = sorted(build.rglob(f"libsvx_{library}.a"))
        if found:
            archives[library] = found[0]
    if "core" not in archives:
        return [f"{build}: no libsvx_core.a (build the engine first)"]
    for library, archive in archives.items():
        out = subprocess.run([nm, "-A", str(archive)], capture_output=True, text=True)
        if out.returncode != 0:
            problems.append(f"{archive}: {nm} failed: {out.stderr.strip()}")
            continue
        for line in out.stdout.splitlines():
            match = UNDEFINED.match(line.strip())
            if not match:
                continue
            name = libm_name(match.group("symbol"))
            if name:
                problems.append(f"{library}: {match.group('member')} calls the platform's {name} (use svx::dm::{name})")
    return sorted(set(problems))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="check", required=True)
    sub.add_parser("layers")
    libm = sub.add_parser("libm")
    libm.add_argument("build", type=pathlib.Path)
    libm.add_argument("--nm", default="nm")
    args = parser.parse_args()
    problems = check_layers() if args.check == "layers" else check_libm(args.build, args.nm)
    for problem in problems:
        print(problem)
    print(f"{args.check}: {len(problems)} problem(s)", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
