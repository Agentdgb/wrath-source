# FindJvmHeaders.cmake — locates jni.h / jni_md.h / jvmti.h.
# VantaClient never links jvm.lib; JVM symbols are resolved at runtime via
# GetProcAddress on the target process's jvm.dll. We only need headers.

if (NOT DEFINED VANTA_JAVA_HOME OR NOT VANTA_JAVA_HOME)
    set(VANTA_JAVA_HOME "$ENV{JAVA_HOME}")
endif()

if (NOT VANTA_JAVA_HOME)
    file(GLOB _cand
        "C:/Program Files/Eclipse Adoptium/jdk-21*"
        "C:/Program Files/Zulu/zulu-21*"
        "C:/Program Files/Microsoft/jdk-21*"
        "C:/Program Files/Eclipse Adoptium/jdk-*"
        "C:/Program Files/Java/jdk-*"
        "C:/Program Files/Zulu/zulu-*"
        "C:/Program Files/Microsoft/jdk-*"
        "C:/Program Files/Amazon Corretto/jdk*"
    )
    if (_cand)
        list(GET _cand 0 VANTA_JAVA_HOME)
    endif()
endif()

if (NOT VANTA_JAVA_HOME OR NOT EXISTS "${VANTA_JAVA_HOME}/include/jni.h")
    message(FATAL_ERROR "Could not find JDK headers (jni.h/jvmti.h). Set JAVA_HOME to a JDK 17+.")
endif()

set(VANTA_JVM_INCLUDE_DIRS
    "${VANTA_JAVA_HOME}/include"
    "${VANTA_JAVA_HOME}/include/win32"
    PARENT_SCOPE
)

# Re-expose for the parent CMakeLists (include() runs in the same scope).
set(VANTA_JVM_INCLUDE_DIRS
    "${VANTA_JAVA_HOME}/include"
    "${VANTA_JAVA_HOME}/include/win32"
)

message(STATUS "VantaClient JDK headers: ${VANTA_JAVA_HOME}/include")
