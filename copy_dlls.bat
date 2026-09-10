@echo off
if "%1"=="" (
    echo Usage: copy_dlls.bat ^<build_directory^>
    echo Example: copy_dlls.bat build\debug_vs2022
    exit /b 1
)

set BUILD_DIR=%1
set BASE_DIR=%~dp0..\..

echo Copying Mesa DLLs from %BUILD_DIR%...

:: Define source paths
set ZLIB_DLL=%BUILD_DIR%\subprojects\zlib-1.3.1\z-1.dll
set GLESV2_DLL=%BUILD_DIR%\src\mesa\glapi\es2api\libGLESv2.dll
set GALLIUM_WGL_DLL=%BUILD_DIR%\src\gallium\targets\wgl\libgallium_wgl.dll
set OPENGL32_DLL=%BUILD_DIR%\src\gallium\targets\libgl-gdi\opengl32.dll
set EGL_DLL=%BUILD_DIR%\src\egl\libEGL.dll

:: Check if Release directory exists and copy
if exist "%BASE_DIR%\build\Release" (
    echo Copying to Release directory...
    copy /Y "%ZLIB_DLL%" "%BASE_DIR%\build\Release\" 2>nul
    copy /Y "%GLESV2_DLL%" "%BASE_DIR%\build\Release\" 2>nul
    copy /Y "%GALLIUM_WGL_DLL%" "%BASE_DIR%\build\Release\" 2>nul
    copy /Y "%EGL_DLL%" "%BASE_DIR%\build\Release\" 2>nul
    copy /Y "%OPENGL32_DLL%" "%BASE_DIR%\build\Release\RemoteGPUopengl32.dll" 2>nul
    if errorlevel 1 (
        echo Warning: Some files could not be copied to Release directory
    ) else (
        echo Successfully copied DLLs to Release directory
    )
) else (
    echo Release directory does not exist, skipping...
)

:: Check if Debug directory exists and copy
if exist "%BASE_DIR%\build\Debug" (
    echo Copying to Debug directory...
    copy /Y "%ZLIB_DLL%" "%BASE_DIR%\build\Debug\" 2>nul
    copy /Y "%GLESV2_DLL%" "%BASE_DIR%\build\Debug\" 2>nul
    copy /Y "%GALLIUM_WGL_DLL%" "%BASE_DIR%\build\Debug\" 2>nul
    copy /Y "%EGL_DLL%" "%BASE_DIR%\build\Debug\" 2>nul
    copy /Y "%OPENGL32_DLL%" "%BASE_DIR%\build\Debug\RemoteGPUopengl32.dll" 2>nul
    if errorlevel 1 (
        echo Warning: Some files could not be copied to Debug directory
    ) else (
        echo Successfully copied DLLs to Debug directory
    )
) else (
    echo Debug directory does not exist, skipping...
)

:: Copy all DLLs to driver directory with original names
echo Copying all DLLs to driver directory...
if not exist "%BASE_DIR%\driver\windows\third_party\mesa\bin64" (
    mkdir "%BASE_DIR%\driver\windows\third_party\mesa\bin64"
)
copy /Y "%ZLIB_DLL%" "%BASE_DIR%\driver\windows\third_party\mesa\bin64\" 2>nul
copy /Y "%GLESV2_DLL%" "%BASE_DIR%\driver\windows\third_party\mesa\bin64\" 2>nul
copy /Y "%GALLIUM_WGL_DLL%" "%BASE_DIR%\driver\windows\third_party\mesa\bin64\" 2>nul
copy /Y "%EGL_DLL%" "%BASE_DIR%\driver\windows\third_party\mesa\bin64\" 2>nul
copy /Y "%OPENGL32_DLL%" "%BASE_DIR%\driver\windows\third_party\mesa\bin64\" 2>nul
if errorlevel 1 (
    echo Warning: Some files could not be copied to driver directory
) else (
    echo Successfully copied all DLLs to driver directory
)

echo Done! 