# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# The Original Code is Copyright (C) 2008, Blender Foundation
# All rights reserved.
# ***** END GPL LICENSE BLOCK *****

set(INC
  .
  ../blenkernel
  ../blenlib
  ../blentranslation
  ../editors/include
  ../gpu
  ../imbuf
  ../makesdna
  ../makesrna
  ../../../intern/glew-mx
  ../../../intern/guardedalloc
)

set(INC_SYS
  ${FREETYPE_INCLUDE_DIRS}
)

set(SRC
  intern/blf.c
  intern/blf_default.c
  intern/blf_dir.c
  intern/blf_font.c
  intern/blf_font_default.c
  intern/blf_glyph.c
  intern/blf_thumbs.c
  intern/blf_util.c

  BLF_api.h
  intern/blf_internal.h
  intern/blf_internal_types.h
)

set(LIB
  bf_gpu
  bf_intern_guardedalloc

  ${FREETYPE_LIBRARY}
)

if(WIN32)
  list(APPEND SRC
    intern/blf_font_win32_compat.c
  )
endif()

if(WITH_INTERNATIONAL)
  add_definitions(-DWITH_INTERNATIONAL)
endif()

if(WITH_PYTHON)
  add_definitions(-DWITH_PYTHON)
  list(APPEND INC
    ../python
  )
endif()


blender_add_lib(bf_blenfont "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RAQM
# macOS:
#   $% brew install libraqm
# windows:
#   - check https://github.com/microsoft/vcpkg
#   > vcpkg integrate install
#   > vcpkg install libraqm
# linux:
#   - build from https://github.com/HOST-Oman/libraqm

# create make.bat in ..\blender with DOS script:
# ---------------------------------------
# @echo off
# REM This batch file does an out-of-source CMake build in ../build_windows
# REM This is for users who like to configure & build Blender with a single command.
# setlocal EnableDelayedExpansion
# setlocal ENABLEEXTENSIONS
# set BLENDER_DIR=%~dp0blender
# set RAQM_LIB="%VCPKG_ROOT%\installed\x86-windows\lib\raqm.lib"
# set RAQM_INCLUDE="%VCPKG_ROOT%\installed\x86-windows\include"
# 
# echo RAQM_LIB=%RAQM_LIB%
# echo RAQM_LIB=%RAQM_INCLUDE%
# 
# echo %BLENDER_DIR%\make.bat %*
# %BLENDER_DIR%\make.bat %*
# 
# :EOF
# ---------------------------------------


if(NOT WIN32)
  find_package(PkgConfig)
  pkg_check_modules(MY_RAQM REQUIRED IMPORTED_TARGET raqm)
  target_link_libraries(bf_blenfont PUBLIC PkgConfig::MY_RAQM)
else(WIN32)
  target_link_libraries(bf_blenfont PUBLIC $ENV{RAQM_LIB})
  target_include_directories(bf_blenfont PUBLIC $ENV{RAQM_INCLUDE})
endif()
add_definitions(-DWITH_FONT_RAQM)