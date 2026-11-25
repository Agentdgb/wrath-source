# FindJvmHeaders.cmake — locates jni.h / jni_md.h / jvmti.h.
# Target never links jvm.lib; JVM symbols are resolved at runtime via
# GetProcAddress on the target process's jvm.dll. We only need headers.

if (NOT DEFINED DT_JAVA_HOME OR NOT DT_JAVA_HOME)
    set(DT_JAVA_HOME "$ENV{JAVA_HOME}")
endif()

if (NOT DT_JAVA_HOME)
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
        list(GET _cand 0 DT_JAVA_HOME)
    endif()
endif()

if (NOT DT_JAVA_HOME OR NOT EXISTS "${DT_JAVA_HOME}/include/jni.h")
    message(FATAL_ERROR "Could not find JDK headers (jni.h/jvmti.h). Set JAVA_HOME to a JDK 17+.")
endif()

set(DT_JVM_INCLUDE_DIRS
    "${DT_JAVA_HOME}/include"
    "${DT_JAVA_HOME}/include/win32"
    PARENT_SCOPE
)

# Re-expose for the parent CMakeLists (include() runs in the same scope).
set(DT_JVM_INCLUDE_DIRS
    "${DT_JAVA_HOME}/include"
    "${DT_JAVA_HOME}/include/win32"
)

message(STATUS "JDK headers: ${DT_JAVA_HOME}/include")
