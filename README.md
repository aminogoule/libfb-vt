# libfb-vt

Консольная графика через framebuffer и зародыш display-сервера для FreeBSD 10+ (vt(4)).
Только нативные интерфейсы FreeBSD — никаких `/dev/mem`, kvm, DRM/KMS или linuxkpi
(кроме бэкенда `.vga`/`.svga`, которым `/dev/mem`/`/dev/io` нужны напрямую — см.
[Documentation/backends.md](Documentation/backends.md)).

## Слои

`fb.c`/`fb.h` (framebuffer + двойная буферизация), `ppm.c`/`ppm.h` (загрузчик PPM),
`vtcon.c`/`vtcon.h` (владение VT/консолью), `mouse.c`/`mouse.h` (`/dev/sysmouse`),
`kbd.c`/`kbd.h` (сырые скан-коды), `proto.c`/`proto.h` (протокол компоновщика),
`fontspleen.h` (встроенный шрифт 8×16), `mgl.c`/`mgl.h` (программный OpenGL-подобный
слой). Подробности — [Documentation/architecture.md](Documentation/architecture.md).

## Программы

`ppm2fb`, `fbshow`, `server` (компоновщик), `term` (графический терминал-клиент),
`cube`/`glcube` (демо-куб, отдельный VT), `cubewin` (тот же куб как клиент компоновщика).
Полный список, флаги и что умеет `term` — [Documentation/programs.md](Documentation/programs.md).

## Быстрый старт

    make                                # собрать всё (все бэкенды + term + cubewin)
    sudo ./server.svga -e ./term        # компоновщик + терминал на VMware SVGA II

Требуется root, на консоли vt(4) (`sysctl kern.vty` должен сообщать `vt`). Подробный
разбор запуска, обоев, `TERM_CMD` и т.п. — [Documentation/programs.md](Documentation/programs.md#запуск-компоновщик--терминал).

## Документация

- [Documentation/architecture.md](Documentation/architecture.md) — слои и модули.
- [Documentation/programs.md](Documentation/programs.md) — все программы, сборка,
  запуск, полный список того, что умеет `term`, и раздел про `mgl`.
- [Documentation/backends.md](Documentation/backends.md) — три бэкенда framebuffer
  (`vt_fb`/`vt_vga`/VMware SVGA II), смена разрешения на лету, переключение VT.
- [Documentation/input.md](Documentation/input.md) — мышь и клавиатура: xterm
  mouse-reporting, scrollback, `K_CODE`, диагностика (`MOUSE_DEBUG`, `KBD_DEBUG`,
  `TERM_DEBUG`), известные аппаратные особенности (PS/2 + `moused -t auto`).
- [Documentation/troubleshooting.md](Documentation/troubleshooting.md) — чёрный
  экран вместо рабочего стола и другие проблемы запуска.
- [Documentation/en.md](Documentation/en.md) — короткая англоязычная версия (менее
  подробная и не всегда синхронизированная; при расхождении верить русским разделам
  и коду).

## Примечания

- vt(4) владеет видеорежимом; в userland нет установки режима через VESA.
- Порядок пикселей — `0x00RRGGBB`.
- Быстрое тестовое изображение:

      printf 'P6\n2 2\n255\n' > test.ppm && \
      printf '\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00' >> test.ppm

Лицензия — [LICENSE](LICENSE).
