# PRE build script: make this project's include/lwipopts.h win.
#
# The picosdk framework adds the bundled btstack lwIP port directory (which
# carries its own lwipopts.h) to the include path. Prepending the project
# include directory ensures every translation unit, lwIP core included, is
# compiled against include/lwipopts.h in this repo.
Import("env")
env.Prepend(CPPPATH=[env["PROJECT_INCLUDE_DIR"]])
