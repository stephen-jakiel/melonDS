# Vendored Lua 5.4.9 (see LICENSE). Only the library sources are built here;
# the standalone lua/luac executables (lua.c/luac.c) are intentionally omitted
# since we embed Lua directly rather than shelling out to it.
add_library(lua STATIC
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    lcorolib.c
    lctype.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    linit.c
    liolib.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    loslib.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltable.c
    ltablib.c
    ltm.c
    lundump.c
    lutf8lib.c
    lvm.c
    lzio.c
)

target_include_directories(lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
