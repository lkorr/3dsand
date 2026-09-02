#!/usr/bin/env bash
# Put the MSVC x64 toolchain on PATH/INCLUDE/LIB for THIS shell, the way a
# "x64 Native Tools" prompt does — Ninja needs cl.exe/link.exe reachable by
# name, which the Visual Studio generator never did because MSBuild found them
# itself. Sourced by build.sh; runnable standalone to inspect what it finds.
#
#   source scripts/vsenv.sh        # exports PATH/INCLUDE/LIB/LIBPATH
#   bash scripts/vsenv.sh          # prints the resolved VS root and cl.exe
#
# The env is captured once per VS install into C:/sv-deps/vsenv-<hash>.txt
# (vcvarsall takes ~2 s, and every build.sh would otherwise pay it), keyed on
# the VS root path so a VS upgrade re-captures. Delete the file to force it.
sv_vsenv() {
  local vswhere="C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  [ -x "$vswhere" ] || { echo "vsenv: vswhere.exe not found" >&2; return 1; }
  local vsroot
  vsroot=$("$vswhere" -latest -products '*' \
             -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
             -property installationPath 2>/dev/null | tr -d '\r')
  [ -n "$vsroot" ] || { echo "vsenv: no VS with the C++ x64 tools" >&2; return 1; }
  local vcvars="$vsroot\\VC\\Auxiliary\\Build\\vcvarsall.bat"
  local key
  key=$(printf '%s' "$vsroot" | cksum | cut -d' ' -f1)
  local cache="C:/sv-deps/vsenv-$key.txt"
  if [ ! -s "$cache" ]; then
    mkdir -p "C:/sv-deps"
    # `set` after vcvarsall dumps the whole environment; keep only the rows
    # the toolchain adds. Through a .bat rather than an inline `cmd /c "..."`:
    # MSYS rewrites the inner quotes of an inline command into literal \" and
    # cmd then looks for a program named '"C:\Program'.
    local bat="C:/sv-deps/vsenv-$key.bat"
    printf '@call "%s" x64 >nul 2>&1\r\n@set\r\n' "$vcvars" > "$bat"
    MSYS_NO_PATHCONV=1 cmd.exe /d /c "$(cygpath -w "$bat")" \
      | tr -d '\r' \
      | grep -E '^(PATH|INCLUDE|LIB|LIBPATH|VCToolsInstallDir|VCToolsVersion|WindowsSdkDir|WindowsSDKVersion|UCRTVersion|VSCMD_ARG_TGT_ARCH|VSINSTALLDIR|VCINSTALLDIR)=' \
      > "$cache.tmp" && mv "$cache.tmp" "$cache"
    [ -s "$cache" ] || { echo "vsenv: vcvarsall produced no environment" >&2; rm -f "$cache"; return 1; }
  fi
  local line name val
  while IFS= read -r line; do
    name=${line%%=*}
    val=${line#*=}
    if [ "$name" = "PATH" ]; then
      # Windows PATH → MSYS PATH, so bash resolves cl.exe/link.exe by name.
      export PATH="$(cygpath -up "$val")"
    else
      export "$name=$val"
    fi
  done < "$cache"
  export SV_VSROOT="$vsroot"
  return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  sv_vsenv || exit 1
  echo "VS root: $SV_VSROOT"
  echo "cl.exe:  $(command -v cl.exe || echo 'NOT FOUND')"
  echo "ninja:   $(command -v ninja || echo 'NOT FOUND')"
else
  sv_vsenv
fi
