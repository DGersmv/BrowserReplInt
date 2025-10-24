cmake_minimum_required (VERSION 3.16)

# ================== функции ==================
function (SetCompilerOptions target)
    target_compile_features (${target} PUBLIC cxx_std_17)
    target_compile_options (${target} PUBLIC "$<$<CONFIG:Debug>:-DDEBUG>")
    if (WIN32)
        target_compile_options (${target} PUBLIC /W3 /WX /wd4996 /Zc:wchar_t-)
    else ()
        target_compile_options (${target} PUBLIC -Wall -Werror -fvisibility=hidden
            -Wno-multichar
            -Wno-ctor-dtor-privacy
            -Wno-invalid-offsetof
            -Wno-ignored-qualifiers
            -Wno-reorder
            -Wno-overloaded-virtual
            -Wno-unused-parameter
            -Wno-missing-field-initializers
            -Wno-unknown-pragmas
            -Wno-missing-braces
            -Wno-unused-private-field
            -Wno-return-std-move
            -Wno-unused-value
            -Wno-switch
            -Wno-deprecated
            -Wno-shorten-64-to-32)
    endif ()
endfunction ()

function (AddGSModuleIncludeFolder target folder)
    message (STATUS "AddGSModuleIncludeFolder ${folder}")
    target_include_directories (${target} PUBLIC "${folder}")
endfunction ()

function (AddGSModuleLinkLibrary target library)
    if (WIN32)
        file (GLOB libs "${library}/Win/*.lib")
        foreach (lib ${libs})
            message (STATUS "AddGSModuleLinkLibrary ${lib}")
            target_link_libraries (${target} ${lib})
        endforeach ()
    else ()
        message (STATUS "AddGSModuleLinkLibrary ${library}")
        target_link_libraries (${target} ${library})
    endif ()
endfunction ()


# ================== общие настройки ==================
set_property (GLOBAL PROPERTY USE_FOLDERS ON)

get_filename_component (CMAKE_CURRENT_FOLDER_NAME "${CMAKE_CURRENT_LIST_DIR}" NAME)
set (CMAKE_SUPPRESS_REGENERATION 1)
set (CMAKE_CONFIGURATION_TYPES Debug;Release;RelWithDebInfo)

if (WIN32)
    set (CMAKE_GENERATOR_TOOLSET v142)
    set (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
else ()
    set (CMAKE_OSX_DEPLOYMENT_TARGET ___MACOSX_DEPLOYMENT_TARGET___)
    if (CMAKE_GENERATOR STREQUAL Xcode)
        set (CMAKE_OSX_ARCHITECTURES "$(ARCHS_STANDARD)" CACHE INTERNAL "" FORCE)
        set (CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "NO")
    endif ()
endif ()

set (AC_API_DEVKIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "API DevKit directory.")
set (AC_ADDON_NAME "${CMAKE_CURRENT_FOLDER_NAME}" CACHE STRING "Add-On name.")
set (AC_ADDON_LANGUAGE "INT" CACHE STRING "Add-On language code.")

message (STATUS "APIDevKit directory: ${AC_API_DEVKIT_DIR}")
set (ACAPINC_FILE_LOCATION ${AC_API_DEVKIT_DIR}/Support/Inc/ACAPinc.h)
if (EXISTS ${ACAPINC_FILE_LOCATION})
    file (READ ${ACAPINC_FILE_LOCATION} ACAPIncContent)
    string (REGEX MATCHALL "#define[ \t]+ServerMainVers_([0-9][0-9])" VersionList ${ACAPIncContent})
    set (ARCHICAD_VERSION ${CMAKE_MATCH_1})
    message (STATUS "Archicad Version: ${ARCHICAD_VERSION}")
else ()
    message (FATAL_ERROR "Failed to detect Archicad version, please check the value of the AC_API_DEVKIT_DIR variable.")
endif ()

if (WIN32)
    add_definitions (-DUNICODE -D_UNICODE -D_ITERATOR_DEBUG_LEVEL=0)
else ()
    add_definitions (-Dmacintosh=1)
endif ()
add_definitions (-DACExtension)

project (${AC_ADDON_NAME})

set (AddOnSourcesFolder ./Src)
set (AddOnResourcesFolder .)

# ================== ресурсы ==================
set (ResourceObjectsDir ${CMAKE_BINARY_DIR}/ResourceObjects)

file (GLOB AddOnImageFiles
    ${AddOnResourcesFolder}/RFIX/Images/*.svg
)

if (WIN32)
    file (GLOB AddOnResourceFiles
        ${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc
        ${AddOnResourcesFolder}/RFIX/*.grc
        ${AddOnResourcesFolder}/RFIX/*.grc
        ${AddOnResourcesFolder}/RFIX.win/*.rc2
    )
else ()
    file (GLOB AddOnResourceFiles
        ${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc
        ${AddOnResourcesFolder}/RFIX/*.grc
        ${AddOnResourcesFolder}/RFIX.mac/*.plist
    )
endif ()

source_group ("Images"   FILES ${AddOnImageFiles})
source_group ("Resources" FILES ${AddOnResourceFiles})

add_custom_target (
    AddOnResources ALL
    DEPENDS "${ResourceObjectsDir}/AddOnResources.stamp"
    SOURCES ${AddOnResourceFiles} ${AddOnImageFiles}
)

get_filename_component (AddOnSourcesFolderAbsolute     "${CMAKE_CURRENT_LIST_DIR}/${AddOnSourcesFolder}"     ABSOLUTE)
get_filename_component (AddOnResourcesFolderAbsolute   "${CMAKE_CURRENT_LIST_DIR}/${AddOnResourcesFolder}"   ABSOLUTE)
get_filename_component (APIDevKitToolsFolderAbsolute   "${AC_API_DEVKIT_DIR}/Support/Tools"                  ABSOLUTE)

if (WIN32)
    add_custom_command (
        OUTPUT  "${ResourceObjectsDir}/AddOnResources.stamp"
        DEPENDS ${AddOnResourceFiles} ${AddOnImageFiles}
        COMMENT "Compiling resources..."
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
        COMMAND python "${APIDevKitToolsFolderAbsolute}/CompileResources.py" "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}" "${AddOnSourcesFolderAbsolute}" "${AddOnResourcesFolderAbsolute}" "${ResourceObjectsDir}" "${ResourceObjectsDir}/${AC_ADDON_NAME}.res"
        COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp"
    )
else ()
    add_custom_command (
        OUTPUT  "${ResourceObjectsDir}/AddOnResources.stamp"
        DEPENDS ${AddOnResourceFiles} ${AddOnImageFiles}
        COMMENT "Compiling resources..."
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
        COMMAND python "${APIDevKitToolsFolderAbsolute}/CompileResources.py" "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}" "${AddOnSourcesFolderAbsolute}" "${AddOnResourcesFolderAbsolute}" "${ResourceObjectsDir}" "${CMAKE_BINARY_DIR}/$<CONFIG>/${AC_ADDON_NAME}.bundle/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy "${AC_API_DEVKIT_DIR}/Support/Inc/PkgInfo" "${CMAKE_BINARY_DIR}/$<CONFIG>/${AC_ADDON_NAME}.bundle/Contents/PkgInfo"
        COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp"
    )
endif ()

# ================== исходники аддона ==================
file (GLOB AddOnHeaderFiles
    ${AddOnSourcesFolder}/*.h
    ${AddOnSourcesFolder}/*.hpp
)
file (GLOB AddOnSourceFiles
    ${AddOnSourcesFolder}/*.c
    ${AddOnSourcesFolder}/*.cpp
)
file (GLOB AllCFiles
    ${AddOnSourcesFolder}/*.c
)
set_source_files_properties(${AllCFiles} PROPERTIES LANGUAGE CXX)

set (AddOnFiles
    ${AddOnHeaderFiles}
    ${AddOnSourceFiles}
)
source_group ("Sources" FILES ${AddOnFiles})

if (WIN32)
    add_library (AddOn SHARED ${AddOnFiles})
else ()
    add_library (AddOn MODULE ${AddOnFiles})
endif ()

# ================== свойства цели / имена / выходные каталоги ==================

# 1) Папки вывода — единообразно ToArchicad/<CONFIG>
set_target_properties (AddOn PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/ToArchicad/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/ToArchicad/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/ToArchicad/$<CONFIG>"
)

# 2) Имена бинарников по конфигурациям:
#    Debug   → ${AC_ADDON_NAME}    (как раньше — из имени папки)
#    Release → LandscapeHelper_AC27
#    RelWithDebInfo оставим как ${AC_ADDON_NAME}, чтобы "ничего не ломать".
set_target_properties (AddOn PROPERTIES
    OUTPUT_NAME_DEBUG         "${AC_ADDON_NAME}"
    OUTPUT_NAME_RELEASE       "LandscapeHelper_AC27"
    OUTPUT_NAME_RELWITHDEBINFO "${AC_ADDON_NAME}"
)

if (WIN32)
    # 3) Суффикс .apx для Windows
    set_target_properties (AddOn PROPERTIES SUFFIX ".apx")

    # 4) PDB — каталоги и имена под конфигурации (для удобства отладки/релиза)
    set_target_properties (AddOn PROPERTIES
        PDB_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/ToArchicad/$<CONFIG>"
        COMPILE_PDB_OUTPUT_DIRECTORY     "${CMAKE_SOURCE_DIR}/ToArchicad/$<CONFIG>"
        PDB_NAME_DEBUG                   "${AC_ADDON_NAME}"
        PDB_NAME_RELEASE                 "LandscapeHelper_AC27"
        PDB_NAME_RELWITHDEBINFO          "${AC_ADDON_NAME}"
        COMPILE_PDB_NAME_DEBUG           "${AC_ADDON_NAME}"
        COMPILE_PDB_NAME_RELEASE         "LandscapeHelper_AC27"
        COMPILE_PDB_NAME_RELWITHDEBINFO  "${AC_ADDON_NAME}"
    )
else ()
    # macOS bundle (как было)
    set_target_properties (AddOn PROPERTIES BUNDLE TRUE)
    set_target_properties (AddOn PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/${AddOnResourcesFolder}/RFIX.mac/Info.plist"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
endif ()


# ================== ресурсы линкера и экспорты (Windows) ==================
if (WIN32)
    # подключаем .res, сгенерённый CompileResources.py
    target_link_options (AddOn PUBLIC "${ResourceObjectsDir}/${AC_ADDON_NAME}.res")
    # экспорт обязательных символов Archicad
    target_link_options (AddOn PUBLIC /export:GetExportedFuncAddrs,@1 /export:SetImportedFuncAddrs,@2)
endif ()

# ================== include / libs DevKit ==================
target_include_directories (AddOn PUBLIC
    ${AddOnSourcesFolder}
    ${AC_API_DEVKIT_DIR}/Support/Inc
)

if (WIN32)
    target_link_libraries (AddOn
        "${AC_API_DEVKIT_DIR}/Support/Lib/ACAP_STAT.lib"
    )
else ()
    find_library (CocoaFramework Cocoa)
    target_link_libraries (AddOn
        "${AC_API_DEVKIT_DIR}/Support/Lib/libACAP_STAT.a"
        ${CocoaFramework}
    )
endif ()

SetCompilerOptions (AddOn)
add_dependencies (AddOn AddOnResources)

get_filename_component (APIDevKitModulesDir "${AC_API_DEVKIT_DIR}/Support/Modules" ABSOLUTE)

file (GLOB GSIncludeFolders
    ${APIDevKitModulesDir}/*
)
foreach (includeFolder ${GSIncludeFolders})
    AddGSModuleIncludeFolder (AddOn ${includeFolder})
endforeach ()

if (WIN32)
    set (APIDevKitLinkLibDir "${APIDevKitModulesDir}")
else ()
    get_filename_component (APIDevKitLinkLibDir "${AC_API_DEVKIT_DIR}/Support/Frameworks" ABSOLUTE)
endif ()

file (GLOB GSLinkLibFolders
    ${APIDevKitLinkLibDir}/*
)
foreach (linkLibFolder ${GSLinkLibFolders})
    AddGSModuleLinkLibrary (AddOn ${linkLibFolder})
endforeach ()
