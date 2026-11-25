# Mini vMac (GPL 2, see COPYING.txt and UPSTREAM_COMMIT.txt) set up as a
# Macintosh SE, plus its glue for this board (osglu_cyd.c).
set(CORE ADBEMDEV GLOBGLUE IWMEMDEV M68KITAB MINEM68K MOUSEMDV PROGMAIN ROMEMDEV
         RTCEMDEV SCCEMDEV SCRNEMDV SCSIEMDV SNDEMDEV SONYEMDV VIAEMDEV)
set(SRCS osglu_cyd.c)
foreach(f ${CORE})
    list(APPEND SRCS src/${f}.c)
endforeach()
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "cfg" "src"
    REQUIRES macui)
# Upstream code: its warnings aren't ours to fix; fastest code for the CPU
target_compile_options(${COMPONENT_LIB} PRIVATE -w -O2 -fno-strict-aliasing)
