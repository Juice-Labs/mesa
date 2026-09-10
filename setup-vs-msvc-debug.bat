meson setup --backend=vs2022 --buildtype=debug --wrap-mode=forcefallback -Dgallium-drivers=zink -Dgles2=enabled -Degl=enabled -Degl=enabled -Dzlib=enabled -Dprefix=%~dp0build\debug build\debug_vs2022
