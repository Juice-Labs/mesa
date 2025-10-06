@echo off
if "%1"=="" (
    echo Usage: copy_opengl_dlls_for_desktop_app.bat ^<build_directory^> ^<app_directory^>
    echo Example: copy_opengl_dlls_for_desktop_app.bat build\debug_vs2022 "C:\Program Files\Blender Foundation\Blender 4.5"
    exit /b 1
)

set BUILD_DIR=%1

if not exist %BUILD_DIR% (
    echo "The build directory does not exist:" %BUILD_DIR%
    exit /b 1
)

set APP_DIR=%2

if not exist %APP_DIR% (
    echo "The desktop app directory does not exist:" %APP_DIR%
    exit /b 1
)

echo Copying Mesa OpenGL DLLs from %BUILD_DIR% to %APP_DIR%

@echo on
copy /Y %BUILD_DIR%\subprojects\zlib-1.2.13\z.dll %APP_DIR%
copy /Y %BUILD_DIR%\src\gallium\targets\wgl\libgallium_wgl.dll %APP_DIR%
copy /Y %BUILD_DIR%\src\gallium\targets\libgl-gdi\opengl32.dll %APP_DIR%
copy /Y %BUILD_DIR%\src\mapi\shared-glapi\libglapi.dll %APP_DIR%
