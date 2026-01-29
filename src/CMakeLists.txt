# CMakeLists.txt - ПРОСТОЙ ВАРИАНТ ДЛЯ PLATFORMIO

# Этот файл был автоматически сгенерирован для проектов
# без стандартного файла 'CMakeLists.txt'.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# Добавляем файлы из библиотек
FILE(GLOB_RECURSE lib_sources
    ${CMAKE_SOURCE_DIR}/lib/*.c
    ${CMAKE_SOURCE_DIR}/lib/*/*.c
)

# Объединяем все исходные файлы
set(ALL_SOURCES ${app_sources} ${lib_sources})

idf_component_register(SRCS ${ALL_SOURCES})