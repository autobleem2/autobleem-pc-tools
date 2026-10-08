# EMU-4: the Windows installer must place a fonts/ folder next to the pcsx-abnxt emulator (emunxt\fonts) and
# clear a previous emunxt\ on update. Run with: cmake -DNSI=<installer/windows/autobleem.nsi> -P check_nsi_layout.cmake
file(READ "${NSI}" text)
foreach(needle
    "RMDir /r \"$INSTDIR\\emunxt\""
    "$INSTDIR\\emunxt\\fonts\\NotoSansSC-Regular.otf"
    "CopyFiles /SILENT \"$INSTDIR\\fonts\\*.*\" \"$INSTDIR\\emunxt\\fonts\"")
  string(FIND "${text}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "autobleem.nsi lacks: ${needle}")
  endif()
endforeach()
