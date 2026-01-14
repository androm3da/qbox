#!/usr/bin/env python3
"""
Combine multiple .deps.json files to create dependency summaries and visualizations.

This script processes dependency information from multiple artifacts and generates:
- A text summary of dependencies with license information
- A graphviz visualization showing the dependency hierarchy
"""

import sys
import json
import os
import re
import argparse
import subprocess
import textwrap
from pathlib import Path
from typing import Dict, List, Set, Tuple, Optional
from collections import defaultdict


class LicenseDetector:
    """Detect common open source licenses from library names and known patterns."""

    # Cache for package manager queries to avoid repeated lookups
    _dpkg_cache = {}
    _apt_cache = {}

    # Known project libraries and their licenses (can be extended)
    PROJECT_LIB_LICENSES = {
        'libqbox': ('QBox', 'Apache-2.0'),
        'libqemu': ('QEMU', 'GPL-2.0'),
        'libsystemc': ('SystemC', 'Apache-2.0'),
        'libcci': ('SystemC CCI', 'Apache-2.0'),
        'libfmt': ('fmt', 'MIT'),
        'libzip': ('libzip', 'BSD-3-Clause'),
        'liblua': ('Lua', 'MIT'),
        'librpc': ('rpclib', 'MIT'),
        'libreporting': ('SCP Report', 'Apache-2.0'),
    }

    @classmethod
    def detect_license(cls, lib_name: str, is_system: bool = False) -> Tuple[str, str]:
        """
        Detect the repository and license for a library.
        Returns (repository, license) tuple.
        """
        # Normalize library name
        base_name = cls._extract_base_name(lib_name)

        # Check project libraries first (more specific)
        for pattern, (repo, license) in cls.PROJECT_LIB_LICENSES.items():
            if pattern in base_name or base_name == pattern:
                return repo, license

        # For system libraries, try to query the package manager
        if is_system:
            result = cls._query_system_package_info(base_name)
            if result != ('Unknown', 'Unknown'):
                return result

        # Default
        return 'Unknown', 'Unknown'

    @classmethod
    def _query_system_package_info(cls, lib_name: str) -> Tuple[str, str]:
        """
        Query the system package manager for library information.
        Returns (package_name, license) tuple.
        """
        # Check cache first
        if lib_name in cls._dpkg_cache:
            return cls._dpkg_cache[lib_name]

        try:
            # First, try to find which package provides this library
            # Try with .so extension first
            lib_file_patterns = [
                f"{lib_name}.so*",
                f"{lib_name}.a",
                f"*/{lib_name}.so*",
                f"*/lib/{lib_name}.so*",
                f"/usr/lib/*/{lib_name}.so*",
                f"/lib/*/{lib_name}.so*"
            ]

            package_name = None
            for pattern in lib_file_patterns:
                try:
                    # Use dpkg -S to find which package owns the file
                    result = subprocess.run(
                        ['dpkg', '-S', pattern],
                        capture_output=True,
                        text=True,
                        timeout=5
                    )
                    if result.returncode == 0 and result.stdout:
                        # Parse the output (format: "package: file")
                        lines = result.stdout.strip().split('\n')
                        if lines:
                            package_name = lines[0].split(':')[0].strip()
                            # Remove architecture suffix if present (e.g., libc6:amd64 -> libc6)
                            package_name = package_name.split(':')[0]
                            break
                except (subprocess.TimeoutExpired, subprocess.SubprocessError):
                    continue

            if not package_name:
                # Try alternative: search for package by library name
                # This handles cases like libc -> libc6
                alt_names = cls._get_alternative_package_names(lib_name)
                for alt_name in alt_names:
                    try:
                        result = subprocess.run(
                            ['dpkg', '-l', alt_name],
                            capture_output=True,
                            text=True,
                            timeout=5
                        )
                        if result.returncode == 0 and 'ii' in result.stdout:
                            package_name = alt_name
                            break
                    except (subprocess.TimeoutExpired, subprocess.SubprocessError):
                        continue

            if package_name:
                # Now get the license information
                license_info = cls._get_package_license(package_name)
                if license_info:
                    result = (package_name, license_info)
                    cls._dpkg_cache[lib_name] = result
                    return result

        except Exception as e:
            # Silently handle errors
            pass

        # Cache the failure too
        cls._dpkg_cache[lib_name] = ('Unknown', 'Unknown')
        return ('Unknown', 'Unknown')

    @staticmethod
    def _get_alternative_package_names(lib_name: str) -> List[str]:
        """Get alternative package names for common libraries."""
        alternatives = []

        # Common mappings
        mappings = {
            'libc': ['libc6', 'libc6-dev', 'glibc'],
            'libm': ['libc6', 'libc6-dev'],
            'libdl': ['libc6', 'libc6-dev'],
            'libpthread': ['libc6', 'libc6-dev'],
            'librt': ['libc6', 'libc6-dev'],
            'libgcc': ['libgcc-s1', 'libgcc1', 'gcc-libs'],
            'libgcc_s': ['libgcc-s1', 'libgcc1'],
            'libstdc++': ['libstdc++6', 'libstdc++-dev'],
            'ld-linux': ['libc6', 'libc-bin'],
            'libz': ['zlib1g', 'zlib1g-dev'],
            'libelf': ['libelf1', 'libelf-dev'],
            'libpython': ['libpython3-stdlib', 'python3-minimal'],
        }

        # Direct mapping
        base_name = lib_name.replace('lib', '', 1) if lib_name.startswith('lib') else lib_name
        for key, values in mappings.items():
            if key in lib_name or base_name in key:
                alternatives.extend(values)

        # Generic patterns
        if lib_name.startswith('lib'):
            alternatives.append(lib_name)
            alternatives.append(f"{lib_name}-dev")
            alternatives.append(f"{lib_name}1")

        return alternatives

    @staticmethod
    def _get_package_license(package_name: str) -> Optional[str]:
        """Get license information for a package."""
        try:
            # Try to get copyright file
            copyright_files = [
                f"/usr/share/doc/{package_name}/copyright",
                f"/usr/share/doc/{package_name}/COPYRIGHT",
                f"/usr/share/doc/{package_name}/LICENSE"
            ]

            for copyright_file in copyright_files:
                if os.path.exists(copyright_file):
                    try:
                        with open(copyright_file, 'r', errors='ignore') as f:
                            content = f.read(4096)  # Read first 4KB

                        # Look for common license patterns
                        license_patterns = [
                            (r'License:\s*(.+)', 1),
                            (r'licensed under the (.+) license', 1),
                            (r'This is free software; see the source for copying conditions', 'GPL'),
                            (r'GNU General Public License', 'GPL'),
                            (r'GNU Lesser General Public License', 'LGPL'),
                            (r'GNU Library General Public License', 'LGPL'),
                            (r'Apache License, Version 2.0', 'Apache-2.0'),
                            (r'MIT License', 'MIT'),
                            (r'BSD License', 'BSD'),
                            (r'LGPL-2\.1\+', 'LGPL-2.1+'),
                            (r'GPL-2\.0\+', 'GPL-2.0+'),
                            (r'GPL version 2', 'GPL-2.0'),
                            (r'GPL version 3', 'GPL-3.0'),
                            (r'LGPL version 2\.1', 'LGPL-2.1'),
                            (r'The zlib/libpng License', 'Zlib'),
                            (r'zlib License', 'Zlib'),
                        ]

                        # Special handling for glibc
                        if 'GNU C Library' in content or 'glibc' in content:
                            if 'GNU Lesser General Public' in content:
                                return 'LGPL-2.1+'
                            elif 'GNU Library General Public' in content:
                                return 'LGPL-2.1+'

                        for pattern, group in license_patterns:
                            if isinstance(group, int):
                                match = re.search(pattern, content, re.IGNORECASE)
                                if match:
                                    license_text = match.group(group).strip()
                                    # Clean up the license text
                                    license_text = license_text.split('\n')[0]
                                    license_text = license_text.split(',')[0]
                                    return license_text
                            else:
                                if re.search(pattern, content, re.IGNORECASE):
                                    return group

                        # If we found a copyright file but no clear license, indicate that
                        return 'See copyright file'

                    except Exception:
                        continue

            # Alternative: use apt-cache show
            try:
                result = subprocess.run(
                    ['apt-cache', 'show', package_name],
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                if result.returncode == 0:
                    # Look for License field in output
                    for line in result.stdout.split('\n'):
                        if line.startswith('License:'):
                            return line.split(':', 1)[1].strip()
            except (subprocess.TimeoutExpired, subprocess.SubprocessError):
                pass

        except Exception:
            pass

        return None

    @staticmethod
    def _extract_base_name(lib_path: str) -> str:
        """Extract the base library name from a path."""
        # Get filename
        lib_name = os.path.basename(lib_path)

        # Remove version numbers and extensions
        # e.g., libfoo.so.1.2.3 -> libfoo
        lib_name = re.sub(r'\.so(\.\d+)*$', '', lib_name)
        lib_name = re.sub(r'\.a$', '', lib_name)
        lib_name = re.sub(r'\.dylib$', '', lib_name)
        lib_name = re.sub(r'\.dll$', '', lib_name)

        return lib_name


class DependencyCombiner:
    """Combine dependency information from multiple artifacts."""

    def __init__(self):
        self.artifacts = {}  # artifact_name -> dependencies dict
        self.all_libraries = defaultdict(set)  # library -> set of artifacts using it
        self.library_info = {}  # library -> (repo, license)
        self.local_libraries = set()
        self.system_libraries = set()

    def load_deps_file(self, deps_file: str) -> None:
        """Load a single .deps.json file."""
        try:
            with open(deps_file, 'r') as f:
                data = json.load(f)

            target = data.get('target', 'unknown')
            deps = data.get('dependencies', {})

            # Store artifact dependencies
            self.artifacts[target] = {
                'local': [self._normalize_lib_name(lib) for lib in deps.get('local_libraries', [])],
                'system': [self._normalize_lib_name(lib) for lib in deps.get('system_libraries', [])]
            }

            # Track all libraries
            for lib in self.artifacts[target]['local']:
                self.all_libraries[lib].add(target)
                self.local_libraries.add(lib)

            for lib in self.artifacts[target]['system']:
                self.all_libraries[lib].add(target)
                self.system_libraries.add(lib)

        except Exception as e:
            print(f"Error loading {deps_file}: {e}", file=sys.stderr)

    def _normalize_lib_name(self, lib_path: str) -> str:
        """Normalize library name to just the library name without path."""
        return LicenseDetector._extract_base_name(lib_path)

    def detect_all_licenses(self) -> None:
        """Detect licenses for all libraries."""
        for lib in self.all_libraries:
            is_system = lib in self.system_libraries
            repo, license = LicenseDetector.detect_license(lib, is_system)
            self.library_info[lib] = (repo, license)

    def generate_text_summary(self, output_file: str) -> None:
        """Generate a text summary of dependencies."""
        try:
            output = []

            # Header
            output.append(f"""{'=' * 80}
DEPENDENCY SUMMARY BY ARTIFACT
{'=' * 80}

""")

            # Per-artifact dependencies
            for artifact in sorted(self.artifacts.keys()):
                deps = self.artifacts[artifact]
                artifact_section = f"""Artifact: {artifact}
{'-' * 40}
"""

                # Local libraries
                if deps['local']:
                    local_libs = '\n'.join(f"  - {lib}" for lib in sorted(deps['local']))
                    artifact_section += f"Local Libraries:\n{local_libs}\n"
                else:
                    artifact_section += "Local Libraries: None\n"

                artifact_section += "\n"

                # System libraries
                if deps['system']:
                    system_libs = '\n'.join(f"  - {lib}" for lib in sorted(deps['system']))
                    artifact_section += f"System Libraries:\n{system_libs}\n"
                else:
                    artifact_section += "System Libraries: None\n"

                output.append(artifact_section + "\n\n")

            # All libraries summary
            output.append(f"""{'=' * 80}
ALL LIBRARIES SUMMARY
{'=' * 80}

""")

            # Local libraries table
            local_header = f"""LOCAL LIBRARIES:
{'-' * 40}
{'Library':<30} {'Repository':<30} {'License':<20}
{'-' * 80}
"""
            local_rows = []
            for lib in sorted(self.local_libraries):
                repo, license = self.library_info.get(lib, ('Unknown', 'Unknown'))
                local_rows.append(f"{lib:<30} {repo:<30} {license:<20}")

            output.append(local_header + '\n'.join(local_rows) + '\n\n')

            # System libraries table
            system_header = f"""SYSTEM LIBRARIES:
{'-' * 40}
{'Library':<30} {'Repository':<30} {'License':<20}
{'-' * 80}
"""
            system_rows = []
            for lib in sorted(self.system_libraries):
                repo, license = self.library_info.get(lib, ('Unknown', 'Unknown'))
                system_rows.append(f"{lib:<30} {repo:<30} {license:<20}")

            output.append(system_header + '\n'.join(system_rows) + '\n\n')

            # Usage statistics
            stats = f"""USAGE STATISTICS:
{'-' * 40}
Total artifacts analyzed: {len(self.artifacts)}
Total local libraries: {len(self.local_libraries)}
Total system libraries: {len(self.system_libraries)}
"""
            output.append(stats)

            # Write to file
            with open(output_file, 'w') as f:
                f.write(''.join(output))

            print(f"Text summary written to: {output_file}")

        except Exception as e:
            print(f"Error writing text summary: {e}", file=sys.stderr)

    def generate_graphviz(self, output_file: str) -> None:
        """Generate a graphviz dot file showing dependency hierarchy."""
        try:
            output = []

            # Header and basic setup
            output.append("""digraph Dependencies {
    rankdir=LR;
    node [shape=box];

    // Node styles
    node [fontname="Arial"];
    edge [fontname="Arial"];

""")

            # Artifacts subgraph
            artifacts_nodes = []
            for artifact in sorted(self.artifacts.keys()):
                safe_name = self._make_safe_node_name(artifact)
                artifacts_nodes.append(f'        "{safe_name}" [label="{artifact}",shape=component];')

            artifacts_section = f"""    // Artifacts
    subgraph cluster_artifacts {{
        label="Artifacts";
        style=filled;
        fillcolor=lightgrey;
        node [style=filled,fillcolor=white];
{chr(10).join(artifacts_nodes)}
    }}

"""
            output.append(artifacts_section)

            # Local libraries subgraph
            local_nodes = []
            for lib in sorted(self.local_libraries):
                safe_name = self._make_safe_node_name(lib)
                local_nodes.append(f'        "{safe_name}" [label="{lib}"];')

            if local_nodes:
                local_section = f"""    // Local libraries
    subgraph cluster_local {{
        label="Local Libraries";
        style=filled;
        fillcolor=lightblue;
        node [style=filled,fillcolor=lightcyan];
{chr(10).join(local_nodes)}
    }}

"""
                output.append(local_section)

            # System libraries subgraph
            system_nodes = []
            for lib in sorted(self.system_libraries):
                safe_name = self._make_safe_node_name(lib)
                system_nodes.append(f'        "{safe_name}" [label="{lib}"];')

            if system_nodes:
                system_section = f"""    // System libraries
    subgraph cluster_system {{
        label="System Libraries";
        style=filled;
        fillcolor=lightyellow;
        node [style=filled,fillcolor=lemonchiffon];
{chr(10).join(system_nodes)}
    }}

"""
                output.append(system_section)

            # Dependencies edges
            # This creates a unified dependency graph where libraries that appear as both
            # artifacts (with their own .deps.json) and dependencies will naturally form
            # chains. For example: if vp depends on libqbox, and libqbox depends on libqemu,
            # the graph will show: vp -> libqbox -> libqemu
            dependency_edges = []
            for artifact, deps in self.artifacts.items():
                artifact_node = self._make_safe_node_name(artifact)

                for lib in deps['local']:
                    lib_node = self._make_safe_node_name(lib)
                    dependency_edges.append(f'    "{artifact_node}" -> "{lib_node}" [color=blue];')

                for lib in deps['system']:
                    lib_node = self._make_safe_node_name(lib)
                    dependency_edges.append(f'    "{artifact_node}" -> "{lib_node}" [color=orange];')

            if dependency_edges:
                output.append("    // Dependencies\n")
                output.append('\n'.join(dependency_edges) + '\n    \n')

            # Legend
            legend = """    // Legend
    subgraph cluster_legend {
        label="Legend";
        style=filled;
        fillcolor=white;

        legend_artifact [label="Artifact",shape=component];
        legend_local [label="Local Library",style=filled,fillcolor=lightcyan];
        legend_system [label="System Library",style=filled,fillcolor=lemonchiffon];

        legend_artifact -> legend_local [label="depends on",color=blue];
        legend_artifact -> legend_system [label="depends on",color=orange];

        {rank=same; legend_artifact; legend_local; legend_system}
    }
}
"""
            output.append(legend)

            # Write to file
            with open(output_file, 'w') as f:
                f.write(''.join(output))

            print(f"Graphviz file written to: {output_file}")
            print(f"To generate an image, run: dot -Tpng {output_file} -o dependencies.png")

        except Exception as e:
            print(f"Error writing graphviz file: {e}", file=sys.stderr)

    def _make_safe_node_name(self, name: str) -> str:
        """Make a name safe for use as a graphviz node identifier."""
        # Replace special characters with underscores
        safe_name = re.sub(r'[^a-zA-Z0-9_]', '_', name)
        return f"node_{safe_name}"


def main():
    parser = argparse.ArgumentParser(
        description='Combine multiple .deps.json files and generate dependency reports'
    )
    parser.add_argument(
        'deps_files',
        nargs='+',
        help='Paths to .deps.json files to analyze'
    )
    parser.add_argument(
        '-t', '--text',
        default='dependencies_summary.txt',
        help='Output text file path (default: dependencies_summary.txt)'
    )
    parser.add_argument(
        '-g', '--graphviz',
        default='dependencies.dot',
        help='Output graphviz file path (default: dependencies.dot)'
    )

    args = parser.parse_args()

    # Create combiner instance
    combiner = DependencyCombiner()

    # Load all dependency files
    for deps_file in args.deps_files:
        if not os.path.exists(deps_file):
            print(f"Warning: File '{deps_file}' not found, skipping", file=sys.stderr)
            continue
        combiner.load_deps_file(deps_file)

    if not combiner.artifacts:
        print("Error: No valid dependency files loaded", file=sys.stderr)
        sys.exit(1)

    # Detect licenses
    combiner.detect_all_licenses()

    # Generate outputs
    combiner.generate_text_summary(args.text)
    combiner.generate_graphviz(args.graphviz)


if __name__ == "__main__":
    main()
