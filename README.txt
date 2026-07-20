DrmLauncher — Win32 GUI для запуска N_m3u8DL-RE на удалённом сервере по SSH
=============================================================================

Что делает программа
---------------------
Показывает окно с полями:
  - Аргумент $1  (то, что передаётся в N_m3u8DL-RE первым — ссылка/манифест)
  - Аргумент $2  (то, что передаётся в --key)
и кнопкой "Выполнить".

Адрес сервера зашит в код константой SSH_HOST = "192.168.8.45"
(main.cpp, вверху файла) — поля для него в GUI нет.

По кнопке приложение:
  1. Подключается по SSH к 192.168.8.45 (порт 22),
     логин pi / пароль 639639 (заданы константами в коде).
  2. Выполняет на сервере команду:
     N_m3u8DL-RE $1 -M format=mp4 --key $2 -sv worst --save-name "drm" \
       --save-dir "$HOME" --live-pipe-mux --select-audio id="audio_aar=128000"
  3. Стримит stdout/stderr удалённого процесса в лог в реальном времени.

Значения $1 и $2 автоматически экранируются под POSIX shell (одинарные кавычки),
поэтому в них можно смело вставлять URL со спецсимволами.

Зависимости
-----------
Нужна библиотека libssh2 (заголовки + скомпилированная .lib/.a для Windows).
Проще всего поставить через vcpkg:

    git clone https://github.com/microsoft/vcpkg
    .\vcpkg\bootstrap-vcpkg.bat
    .\vcpkg\vcpkg install libssh2:x64-windows

Сборка (MinGW-w64)
------------------
    g++ -O2 -std=c++17 main.cpp -o DrmLauncher.exe ^
        -I <путь_к_libssh2>\include ^
        -L <путь_к_libssh2>\lib ^
        -lssh2 -lws2_32 -mwindows

Сборка (MSVC, из "x64 Native Tools Command Prompt for VS")
------------------------------------------------------------
    cl /EHsc /std:c++17 main.cpp ^
        /I <путь_к_vcpkg>\installed\x64-windows\include ^
        /link /LIBPATH:<путь_к_vcpkg>\installed\x64-windows\lib ^
        libssh2.lib ws2_32.lib user32.lib gdi32.lib comctl32.lib ^
        /Fe:DrmLauncher.exe

Если используете vcpkg с CMake — проще всего интегрировать через
vcpkg integrate install и обычный CMakeLists.txt со ссылкой на libssh2::libssh2.

Важные замечания
----------------
- Адрес сервера, пароль и логин захардкожены в коде (SSH_HOST / SSH_USER /
  SSH_PASS в начале main.cpp) — как было указано в задаче. Если что-то из
  этого изменится, достаточно поправить соответствующую константу.
- На сервере должен быть установлен и доступен в PATH бинарник N_m3u8DL-RE,
  а команда --save-dir "$HOME" разворачивается shell'ом на удалённой стороне.
- Long-running процесс (--live-pipe-mux) может выполняться долго — окно лога
  просто продолжает получать данные, пока канал не закроется (EOF).
