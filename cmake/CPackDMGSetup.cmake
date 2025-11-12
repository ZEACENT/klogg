# Custom CPack DMG setup to prevent automatic bundle fixup
# macdeployqt already handles all RPATH and framework bundling

# Override the fixup_bundle command to do nothing
# This prevents CPack from trying to modify already-fixed RPATHs
macro(fixup_bundle)
  message(STATUS "Skipping fixup_bundle - macdeployqt already handled bundle fixup")
endmacro()
